#include "demuxer.h"

#include <assert.h>
#include <inttypes.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>

#include "packet_merger.h"
#include "util/binary.h"
#include "util/log.h"

#define SC_PACKET_HEADER_SIZE 12

#define SC_PACKET_FLAG_CONFIG    (UINT64_C(1) << 63)
#define SC_PACKET_FLAG_KEY_FRAME (UINT64_C(1) << 62)

#define SC_PACKET_PTS_MASK (SC_PACKET_FLAG_KEY_FRAME - 1)

static enum AVCodecID
sc_demuxer_to_avcodec_id(uint32_t codec_id) {
#define SC_CODEC_ID_H264 UINT32_C(0x68323634) // "h264" in ASCII
#define SC_CODEC_ID_H265 UINT32_C(0x68323635) // "h265" in ASCII
#define SC_CODEC_ID_AV1 UINT32_C(0x00617631) // "av1" in ASCII
#define SC_CODEC_ID_OPUS UINT32_C(0x6f707573) // "opus" in ASCII
#define SC_CODEC_ID_AAC UINT32_C(0x00616163) // "aac" in ASCII
#define SC_CODEC_ID_FLAC UINT32_C(0x666c6163) // "flac" in ASCII
#define SC_CODEC_ID_RAW UINT32_C(0x00726177) // "raw" in ASCII
    switch (codec_id) {
        case SC_CODEC_ID_H264:
            return AV_CODEC_ID_H264;
        case SC_CODEC_ID_H265:
            return AV_CODEC_ID_HEVC;
        case SC_CODEC_ID_AV1:
#ifdef SCRCPY_LAVC_HAS_AV1
            return AV_CODEC_ID_AV1;
#else
            LOGE("AV1 not supported by this FFmpeg version");
            return AV_CODEC_ID_NONE;
#endif
        case SC_CODEC_ID_OPUS:
            return AV_CODEC_ID_OPUS;
        case SC_CODEC_ID_AAC:
            return AV_CODEC_ID_AAC;
        case SC_CODEC_ID_FLAC:
            return AV_CODEC_ID_FLAC;
        case SC_CODEC_ID_RAW:
            return AV_CODEC_ID_PCM_S16LE;
        default:
            LOGE("Unknown codec id 0x%08" PRIx32, codec_id);
            return AV_CODEC_ID_NONE;
    }
}

static bool
sc_demuxer_recv_codec_id(struct sc_demuxer *demuxer, uint32_t *codec_id) {
    uint8_t data[4];
    ssize_t r = net_recv_all(demuxer->socket, data, 4);
    if (r < 4) {
        return false;
    }

    *codec_id = sc_read32be(data);
    return true;
}

static bool
sc_demuxer_recv_video_size(struct sc_demuxer *demuxer, uint32_t *width,
                           uint32_t *height) {
    uint8_t data[8];
    ssize_t r = net_recv_all(demuxer->socket, data, 8);
    if (r < 8) {
        return false;
    }

    *width = sc_read32be(data);
    *height = sc_read32be(data + 4);
    return true;
}

enum sc_recv_result {
    SC_RECV_OK,
    SC_RECV_EOS,
    SC_RECV_PAUSED, // zero-length packet sentinel
};

static enum sc_recv_result
sc_demuxer_recv_packet(struct sc_demuxer *demuxer, AVPacket *packet) {
    // The video and audio streams contain a sequence of raw packets (as
    // provided by MediaCodec), each prefixed with a "meta" header.
    //
    // The "meta" header length is 12 bytes:
    // [. . . . . . . .|. . . .]. . . . . . . . . . . . . . . ...
    //  <-------------> <-----> <-----------------------------...
    //        PTS        packet        raw packet
    //                    size
    //
    // It is followed by <packet_size> bytes containing the packet/frame.
    //
    // The most significant bits of the PTS are used for packet flags:
    //
    //  byte 7   byte 6   byte 5   byte 4   byte 3   byte 2   byte 1   byte 0
    // CK...... ........ ........ ........ ........ ........ ........ ........
    // ^^<------------------------------------------------------------------->
    // ||                                PTS
    // | `- key frame
    //  `-- config packet

    uint8_t header[SC_PACKET_HEADER_SIZE];
    ssize_t r = net_recv_all(demuxer->socket, header, SC_PACKET_HEADER_SIZE);
    if (r < SC_PACKET_HEADER_SIZE) {
        return SC_RECV_EOS;
    }

    uint64_t pts_flags = sc_read64be(header);
    uint32_t len = sc_read32be(&header[8]);

    if (len == 0)
        return SC_RECV_PAUSED;

    if (av_new_packet(packet, len)) {
        LOG_OOM();
        return SC_RECV_EOS;
    }

    r = net_recv_all(demuxer->socket, packet->data, len);
    if (r < 0 || ((uint32_t) r) < len) {
        av_packet_unref(packet);
        return SC_RECV_EOS;
    }

    if (pts_flags & SC_PACKET_FLAG_CONFIG) {
        packet->pts = AV_NOPTS_VALUE;
    } else {
        packet->pts = pts_flags & SC_PACKET_PTS_MASK;
    }

    if (pts_flags & SC_PACKET_FLAG_KEY_FRAME) {
        packet->flags |= AV_PKT_FLAG_KEY;
    }

    packet->dts = packet->pts;
    return SC_RECV_OK;
}

static int
run_demuxer(void *data) {
    struct sc_demuxer *demuxer = data;
    struct scrcpy_options *options = demuxer->cbs_userdata;

    // Flag to report end-of-stream (i.e. device disconnected)
    enum sc_demuxer_status status = SC_DEMUXER_STATUS_ERROR;

    bool first_run = true;
    for (;;) {
        bool paused = false;

        uint32_t raw_codec_id;
        bool ok = sc_demuxer_recv_codec_id(demuxer, &raw_codec_id);
        if (!ok) {
            if (first_run)
                LOGE("Demuxer '%s': stream disabled due to connection error",
                     demuxer->name);
            else
                status = SC_DEMUXER_STATUS_EOS;
            break;
        }

        if (raw_codec_id == 0) {
            first_run = false;
            if (demuxer->cbs->on_paused)
                demuxer->cbs->on_paused(demuxer, demuxer->cbs_userdata);
            continue;
        }

        if (raw_codec_id == 1) {
            LOGE("Demuxer '%s': stream configuration error on the device",
                 demuxer->name);
            break;
        }

        enum AVCodecID codec_id = sc_demuxer_to_avcodec_id(raw_codec_id);
        if (codec_id == AV_CODEC_ID_NONE) {
            LOGE("Demuxer '%s': stream disabled due to unsupported codec",
                 demuxer->name);
            if (first_run) {
                sc_packet_source_sinks_disable(&demuxer->packet_source);
            }
            break;
        }

        const AVCodec *codec = avcodec_find_decoder(codec_id);
        if (options->prefer_libopus &&
            codec_id == AV_CODEC_ID_OPUS)
        {
            const AVCodec *libopus = avcodec_find_decoder_by_name("libopus");
            if (libopus)
                codec = libopus;
        }
        if (!codec) {
            LOGE("Demuxer '%s': stream disabled due to missing decoder",
                 demuxer->name);
            if (first_run) {
                sc_packet_source_sinks_disable(&demuxer->packet_source);
            }
            break;
        }

        AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
        if (!codec_ctx) {
            LOG_OOM();
            break;
        }

        codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;

        if (codec->type == AVMEDIA_TYPE_VIDEO) {
            uint32_t width;
            uint32_t height;
            ok = sc_demuxer_recv_video_size(demuxer, &width, &height);
            if (!ok) {
                avcodec_free_context(&codec_ctx);
                break;
            }

            codec_ctx->width = width;
            codec_ctx->height = height;
            codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        } else {
            // Hardcoded audio properties
#ifdef SCRCPY_LAVU_HAS_CHLAYOUT
            codec_ctx->ch_layout = (AVChannelLayout) AV_CHANNEL_LAYOUT_STEREO;
#else
            codec_ctx->channel_layout = AV_CH_LAYOUT_STEREO;
            codec_ctx->channels = 2;
#endif
            codec_ctx->sample_rate = 48000;

            if (raw_codec_id == SC_CODEC_ID_FLAC) {
                // The sample_fmt is not set by the FLAC decoder
                codec_ctx->sample_fmt = AV_SAMPLE_FMT_S16;
            }
        }

        if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
            LOGE("Demuxer '%s': could not open codec", demuxer->name);
            avcodec_free_context(&codec_ctx);
            break;
        }

        if (!sc_packet_source_sinks_open(&demuxer->packet_source, codec_ctx)) {
            avcodec_free_context(&codec_ctx);
            break;
        }

        if (!first_run && demuxer->cbs->on_resumed) {
            demuxer->cbs->on_resumed(demuxer, demuxer->cbs_userdata);
        }
        first_run = false;

        // Config packets must be merged with the next non-config packet only
        // for H.26x
        bool must_merge_config_packet = raw_codec_id == SC_CODEC_ID_H264
                                     || raw_codec_id == SC_CODEC_ID_H265;

        struct sc_packet_merger merger;

        if (must_merge_config_packet) {
            sc_packet_merger_init(&merger);
        }

        AVPacket *packet = av_packet_alloc();
        if (!packet) {
            LOG_OOM();
            sc_packet_source_sinks_close(&demuxer->packet_source);
            avcodec_free_context(&codec_ctx);
            break;
        }

        for (;;) {
            enum sc_recv_result result = sc_demuxer_recv_packet(demuxer, packet);
            if (result == SC_RECV_PAUSED) {
                paused = true;
                break;
            }
            if (result != SC_RECV_OK) {
                // end of stream
                status = SC_DEMUXER_STATUS_EOS;
                break;
            }

            if (must_merge_config_packet) {
                // Prepend any config packet to the next media packet
                ok = sc_packet_merger_merge(&merger, packet);
                if (!ok) {
                    av_packet_unref(packet);
                    break;
                }
            }

            ok = sc_packet_source_sinks_push(&demuxer->packet_source, packet);
            av_packet_unref(packet);
            if (!ok) {
                // The sink already logged its concrete error
                break;
            }
        }

        LOGD("Demuxer '%s': end of frames", demuxer->name);

        if (must_merge_config_packet) {
            sc_packet_merger_destroy(&merger);
        }

        av_packet_free(&packet);
        sc_packet_source_sinks_close(&demuxer->packet_source);
        avcodec_free_context(&codec_ctx);

        if (paused) {
            if (demuxer->cbs->on_paused) {
                demuxer->cbs->on_paused(demuxer, demuxer->cbs_userdata);
            }
            continue;
        }

        break;
    }

    demuxer->cbs->on_ended(demuxer, status, demuxer->cbs_userdata);

    return 0;
}

void
sc_demuxer_init(struct sc_demuxer *demuxer, const char *name, sc_socket socket,
                const struct sc_demuxer_callbacks *cbs, void *cbs_userdata) {
    assert(socket != SC_SOCKET_NONE);

    demuxer->name = name; // statically allocated
    demuxer->socket = socket;
    sc_packet_source_init(&demuxer->packet_source);

    assert(cbs && cbs->on_ended);

    demuxer->cbs = cbs;
    demuxer->cbs_userdata = cbs_userdata;
}

bool
sc_demuxer_start(struct sc_demuxer *demuxer) {
    LOGD("Demuxer '%s': starting thread", demuxer->name);

    bool ok = sc_thread_create(&demuxer->thread, run_demuxer, "scrcpy-demuxer",
                               demuxer);
    if (!ok) {
        LOGE("Demuxer '%s': could not start thread", demuxer->name);
        return false;
    }
    return true;
}

void
sc_demuxer_join(struct sc_demuxer *demuxer) {
    sc_thread_join(&demuxer->thread, NULL);
}
