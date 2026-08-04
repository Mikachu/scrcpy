#include "terminal_controller.h"

#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "control_msg.h"
#include "events.h"
#include "util/log.h"
#include "util/str.h"

struct pause_data {
    struct sc_screen *screen;
    bool paused;
};

static void
do_set_paused(void *userdata) {
    struct pause_data *data = userdata;
    sc_screen_set_paused(data->screen, data->paused);
    free(data);
}

static void
handle_command(struct sc_terminal_controller *tc, const char *cmd) {
    int offset;
    char *alias = NULL;
    if (!strncmp(cmd, "b ", offset = 2)) {
        if (asprintf(&alias, "bitrate %s", cmd + offset))
            cmd = alias;
    } else if (!strncmp(cmd, "size ", offset = 5)) {
        if (asprintf(&alias, "video size %s", cmd + offset))
            cmd = alias;
    } else if (!strcmp(cmd, "scan")) {
        if (asprintf(&alias, "scan /sdcard/Download"))
            cmd = alias;
    } else if (!strcmp(cmd, "a")) {
        if (asprintf(&alias, "audio on"))
            cmd = alias;
    } else if (!strcmp(cmd, "aa") || !strcmp(cmd, "A")) {
        if (asprintf(&alias, "audio off"))
            cmd = alias;
    } else if (!strcmp(cmd, "v")) {
        if (asprintf(&alias, "video on"))
            cmd = alias;
    } else if (!strcmp(cmd, "vv") || !strcmp(cmd, "V")) {
        if (asprintf(&alias, "video off"))
            cmd = alias;
    }
    if (!strcmp(cmd, "pause") || !strcmp(cmd, "p")) {
        if (tc->screen) {
            struct pause_data *data = malloc(sizeof(*data));
            if (data) {
                data->screen = tc->screen;
                data->paused = true;
                if (!sc_post_to_main_thread(do_set_paused, data)) {
                    free(data);
                }
            }
        }
    } else if (!strcmp(cmd, "unpause") || !strcmp(cmd, "u")) {
        if (tc->screen) {
            struct pause_data *data = malloc(sizeof(*data));
            if (data) {
                data->screen = tc->screen;
                data->paused = false;
                if (!sc_post_to_main_thread(do_set_paused, data)) {
                    free(data);
                }
            }
        }
    } else if (!strncmp(cmd, "fps ", offset = 4)) {
        if (tc->controller) {
            float fps = (float) atof(cmd + offset);
            struct sc_control_msg msg;
            msg.type = SC_CONTROL_MSG_TYPE_SET_MAX_FPS;
            msg.set_max_fps.max_fps = fps;
            if (!sc_controller_push_msg(tc->controller, &msg)) {
                LOGW("Could not push set_max_fps message");
            } else {
                LOGV("Requested max fps: %g", (double) fps);
            }
            msg.type = SC_CONTROL_MSG_TYPE_RESET_VIDEO;
            if (!sc_controller_push_msg(tc->controller, &msg)) {
                LOGW("Could not push reset_video message");
            }
        }
    } else if (!strncmp(cmd, "bitrate ", offset = 8)) {
        if (tc->controller) {
            long bitrate;
            if (!sc_str_parse_integer_with_suffix(cmd + offset, &bitrate)
                    || bitrate <= 0) {
                LOGW("Invalid bitrate: %s", cmd + offset);
            } else {
                struct sc_control_msg msg;
                msg.type = SC_CONTROL_MSG_TYPE_SET_BIT_RATE;
                msg.set_bit_rate.bit_rate = (uint32_t) bitrate;
                if (!sc_controller_push_msg(tc->controller, &msg)) {
                    LOGW("Could not push set_bit_rate message");
                } else {
                    LOGV("Requested bit rate: %ld", bitrate);
                }
                msg.type = SC_CONTROL_MSG_TYPE_RESET_VIDEO;
                if (!sc_controller_push_msg(tc->controller, &msg)) {
                    LOGW("Could not push reset_video message");
                }
            }
        }
    } else if (!strncmp(cmd, "afps ", offset = 5)) {
        if (tc->activity_fps) {
            struct sc_activity_fps_config config;
            if (!sc_activity_fps_parse_config(cmd + offset, &config)
                    || config.fps_idle1 == 0) {
                LOGW("Invalid afps config: %s", cmd + offset);
                LOGI("Usage: afps fps_active,timeout1,fps_idle1,timeout2,fps_idle2");
            } else {
                sc_activity_fps_reconfigure(tc->activity_fps, &config);
                LOGI("Activity FPS reconfigured");
            }
        } else {
            LOGW("Activity FPS not configured (use --max-fps with extended syntax)");
        }
    } else if (!strncmp(cmd, "loglevel ", offset = 9) || !strncmp(cmd, "v ", offset = 2)) {
        enum sc_log_level level;
        if ((level = sc_parse_log_level(cmd + offset)) < 0) {
            LOGW("Invalid log level: %s", cmd + offset);
            LOGI("Valid levels: verbose, debug, info, warn, error");
        } else {
            sc_set_log_level(level);
            LOGV("Client log level set to: %s", cmd + offset);
            if (tc->controller) {
                struct sc_control_msg msg;
                msg.type = SC_CONTROL_MSG_TYPE_SET_LOG_LEVEL;
                msg.set_log_level.level = (uint8_t) level;
                if (!sc_controller_push_msg(tc->controller, &msg)) {
                    LOGW("Could not push set_log_level message");
                }
            }
        }
    } else if (!strcmp(cmd, "listapps")) {
        if (tc->controller) {
            struct sc_control_msg msg;
            msg.type = SC_CONTROL_MSG_TYPE_LIST_APPS;
            if (!sc_controller_push_msg(tc->controller, &msg)) {
                LOGW("Could not push list_apps message");
            }
        }
    } else if (!strncmp(cmd, "start ", offset = 6)) {
        if (tc->controller) {
            char *name = strdup(cmd + offset);
            if (!name) {
                LOG_OOM();
            } else {
                struct sc_control_msg msg;
                msg.type = SC_CONTROL_MSG_TYPE_START_APP;
                msg.start_app.name = name;
                if (!sc_controller_push_msg(tc->controller, &msg)) {
                    LOGW("Could not push start_app message");
                    free(name);
                }
            }
        }
    } else if (!strcmp(cmd, "start")) {
        if (tc->controller) {
            if (!tc->start_app) {
                LOGW("No app configured (use --start-app)");
            } else {
                char *name = strdup(tc->start_app);
                if (!name) {
                    LOG_OOM();
                } else {
                    struct sc_control_msg msg;
                    msg.type = SC_CONTROL_MSG_TYPE_START_APP;
                    msg.start_app.name = name;
                    if (!sc_controller_push_msg(tc->controller, &msg)) {
                        LOGW("Could not push start_app message");
                        free(name);
                    }
                }
            }
        }
    } else if (!strncmp(cmd, "video size ", offset = 11)) {
        if (tc->controller) {
            char *arg = strdup(cmd + offset);
            struct sc_control_msg msg;
            msg.type = SC_CONTROL_MSG_TYPE_SET_MAX_SIZE;
            msg.set_max_size.size_spec = arg;
            if (!sc_controller_push_msg(tc->controller, &msg)) {
                LOGW("Could not push set_max_size message");
            }
            msg.type = SC_CONTROL_MSG_TYPE_RESET_VIDEO;
            if (!sc_controller_push_msg(tc->controller, &msg)) {
                free(arg);
                LOGW("Could not push reset_video message");
            }
        }
    } else if (!strncmp(cmd, "video ", offset = 6) || !strncmp(cmd, "audio ", offset = 6)) {
        if (tc->controller) {
            int off = !strcmp(cmd + offset, "off");
            int on = !strcmp(cmd + offset, "on");
            if (!off && !on) {
                LOGW("Unknown argument: %s", cmd + offset);
                return;
            }
            int video = cmd[0] == 'v';
            struct sc_control_msg msg;
            msg.type = video ? SC_CONTROL_MSG_TYPE_SET_VIDEO_ENABLED :
                               SC_CONTROL_MSG_TYPE_SET_AUDIO_ENABLED;
            msg.set_stream_enabled.value = on;
            // XXX add source parsing too
            if (!sc_controller_push_msg(tc->controller, &msg)) {
                LOGW("Could not push stream enabled message");
            }
        }
    } else if (!strncmp(cmd, "scan ", offset = 5)) {
        if (tc->controller) {
            char *path = strdup(cmd + offset);
            struct sc_control_msg msg;
            msg.type = SC_CONTROL_MSG_TYPE_SCAN_FILE;
            msg.scan_file.path = path;
            if (!sc_controller_push_msg(tc->controller, &msg)) {
                LOGW("Could not push scan_file message");
                free(path);
            }
        }
    } else if (!strcmp(cmd, "quit") || !strcmp(cmd, "q")) {
        sc_push_event(SDL_QUIT);
    } else if (cmd[0] != '\0') {
        if (strcmp(cmd, "help"))
            LOGW("Unknown terminal command: %s", cmd);
        LOGI("Commands: pause, unpause, fps N, bitrate B, afps [...], listapps, start [app], quit, video on/off, audio on/off, video size [ max pixels | n%% ]");
    }
    free(alias);
}

static int
run_terminal_controller(void *data) {
    struct sc_terminal_controller *tc = data;

    LOGV("Terminal control ready.");

    char line[256];
    int pos = 0;

    struct pollfd fds[2];
    fds[0].fd = 0;
    fds[0].events = POLLIN;
    fds[1].fd = tc->cancel_pipe[0];
    fds[1].events = POLLIN;

    for (;;) {
        int r = poll(fds, 2, -1);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (fds[1].revents & POLLIN) {
            break; // cancelled
        }
        if (!(fds[0].revents & POLLIN)) {
            continue;
        }
        char c;
        if (tcgetpgrp(0) != getpgrp())
            continue;
        ssize_t n = read(0, &c, 1);
        if (n < 0) {
            break;
        }
        if (c == '\n') {
            line[pos] = '\0';
            pos = 0;
            handle_command(tc, line);
        } else if (pos < (int) sizeof(line) - 1) {
            line[pos++] = c;
        }
    }

    return 0;
}

bool
sc_terminal_controller_init(struct sc_terminal_controller *tc,
                             struct sc_screen *screen,
                             struct sc_audio_player *ap,
                             struct sc_controller *controller,
                             struct sc_activity_fps *activity_fps,
                             const char *start_app) {
    tc->screen = screen;
    tc->ap = ap;
    tc->controller = controller;
    tc->activity_fps = activity_fps;
    tc->start_app = start_app;
    tc->muted = false;
    if (pipe(tc->cancel_pipe) == -1) {
        LOGE("Could not create cancel pipe for terminal controller");
        return false;
    }
    return true;
}

bool
sc_terminal_controller_start(struct sc_terminal_controller *tc) {
    bool ok = sc_thread_create(&tc->thread, run_terminal_controller,
                               "terminal-ctrl", tc);
    if (!ok) {
        LOGE("Could not start terminal controller thread");
        return false;
    }
    return true;
}

void
sc_terminal_controller_stop(struct sc_terminal_controller *tc) {
    char byte = 0;
    if (write(tc->cancel_pipe[1], &byte, 1) < 0) {
        // goodbye
    }
}

void
sc_terminal_controller_join(struct sc_terminal_controller *tc) {
    sc_thread_join(&tc->thread, NULL);
}

void
sc_terminal_controller_destroy(struct sc_terminal_controller *tc) {
    close(tc->cancel_pipe[0]);
    close(tc->cancel_pipe[1]);
}
