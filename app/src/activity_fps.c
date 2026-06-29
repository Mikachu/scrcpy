#include "activity_fps.h"

#include "control_msg.h"
#include "util/log.h"

#include <stdlib.h>

bool
sc_activity_fps_parse_config(const char *s,
                             struct sc_activity_fps_config *config) {
    char *end;
    float fps_active = (float) strtod(s, &end);
    if (end == s) {
        LOGE("Invalid fps in activity fps config: '%s'", s);
        return false;
    }

    if (*end == '\0') {
        config->fps_active = fps_active;
        config->fps_idle1 = 0;
        config->timeout1 = 0;
        config->fps_idle2 = 0;
        config->timeout2 = 0;
        return true;
    }

    if (*end != ',') {
        LOGE("Invalid activity fps config: '%s'", s);
        return false;
    }

    int commas = 0;
    for (const char *p = s; *p; ++p) {
        if (*p == ',') ++commas;
    }
    if (commas != 4) {
        LOGE("Invalid activity fps config: expected "
             "'fps_active,timeout1,fps_idle1,timeout2,fps_idle2'");
        return false;
    }

    const char *p = end + 1;

    long timeout1 = strtol(p, &end, 10);
    if (end == p || *end != ',' || timeout1 <= 0) {
        LOGE("Invalid timeout1 in activity fps config: '%s'", s);
        return false;
    }
    p = end + 1;
    float fps_idle1 = (float) strtod(p, &end);
    if (end == p || *end != ',') {
        LOGE("Invalid fps_idle1 in activity fps config: '%s'", s);
        return false;
    }
    p = end + 1;
    long timeout2 = strtol(p, &end, 10);
    if (end == p || *end != ',' || timeout2 <= 0) {
        LOGE("Invalid timeout2 in activity fps config: '%s'", s);
        return false;
    }
    p = end + 1;
    float fps_idle2 = (float) strtod(p, &end);
    if (end == p || *end != '\0') {
        LOGE("Invalid fps_idle2 in activity fps config: '%s'", s);
        return false;
    }

    config->fps_active = fps_active;
    config->timeout1 = (uint32_t) timeout1;
    config->fps_idle1 = fps_idle1;
    config->timeout2 = (uint32_t) timeout2;
    config->fps_idle2 = fps_idle2;
    return true;
}

static void
send_fps(struct sc_controller *controller, float fps, uint32_t bitrate) {
    struct sc_control_msg msg;

    if (bitrate != 0) {
        msg.type = SC_CONTROL_MSG_TYPE_SET_BIT_RATE;
        msg.set_bit_rate.bit_rate = bitrate;
        if (!sc_controller_push_msg(controller, &msg)) {
            LOGW("Could not push set_bit_rate message");
        } else {
            LOGV("Activity FPS: set bit rate to %" PRIu32, bitrate);
        }
    }

    msg.type = SC_CONTROL_MSG_TYPE_SET_MAX_FPS;
    msg.set_max_fps.max_fps = fps;
    if (!sc_controller_push_msg(controller, &msg)) {
        LOGW("Could not push set_max_fps message");
    } else {
        LOGV("Activity FPS: set max fps to %g", (double) fps);
    }

    msg.type = SC_CONTROL_MSG_TYPE_RESET_VIDEO;
    if (!sc_controller_push_msg(controller, &msg)) {
        LOGW("Could not push reset_video message");
    }
}

static int
run_activity_fps(void *data) {
    struct sc_activity_fps *af = data;

    sc_mutex_lock(&af->mutex);
    while (!af->stopped) {
        bool timed_out;
        if (af->state == 2) {
            sc_cond_wait(&af->cond, &af->mutex);
            timed_out = false;
        } else {
            timed_out = !sc_cond_timedwait(&af->cond, &af->mutex,
                                           af->deadline);
        }
        if (af->stopped) {
            break;
        }

        if (timed_out) {
            if (af->state == 0) {
                send_fps(af->controller, af->config.fps_idle1, af->bitrate_idle1);
                af->state = 1;
                if (af->config.fps_idle2 != 0) {
                    af->deadline = sc_tick_now()
                                 + SC_TICK_FROM_SEC(af->config.timeout2);
                } else {
                    af->state = 2;
                }
            } else if (af->state == 1) {
                send_fps(af->controller, af->config.fps_idle2, af->bitrate_idle2);
                af->state = 2;
            }
        }
        // If signaled (not timed out), deadline was already updated by
        // notify_activity; just loop and re-wait.
    }
    sc_mutex_unlock(&af->mutex);

    return 0;
}

static void
apply_config(struct sc_activity_fps *af,
               const struct sc_activity_fps_config *config) {
    af->config = *config;
    af->bitrate_idle1 = (uint32_t)(af->bitrate_active * af->config.fps_idle1 /
                                   (af->config.fps_active ?: 1));
    af->bitrate_idle2 = (uint32_t)(af->bitrate_active * af->config.fps_idle2 /
                                   (af->config.fps_active ?: 1));
}

bool
sc_activity_fps_init(struct sc_activity_fps *af,
                     struct sc_controller *controller,
                     const struct sc_activity_fps_config *config,
                     uint32_t bitrate_active) {
    af->controller = controller;
    af->bitrate_active = bitrate_active;
    af->state = 0;
    af->stopped = false;

    apply_config(af, config);

    bool ok = sc_mutex_init(&af->mutex);
    if (!ok) {
        return false;
    }

    ok = sc_cond_init(&af->cond);
    if (!ok) {
        sc_mutex_destroy(&af->mutex);
        return false;
    }

    return true;
}

bool
sc_activity_fps_start(struct sc_activity_fps *af) {
    af->deadline = sc_tick_now() + SC_TICK_FROM_SEC(af->config.timeout1);
    bool ok = sc_thread_create(&af->thread, run_activity_fps,
                               "scrcpy-afps", af);
    if (!ok) {
        LOGE("Could not start activity fps thread");
        return false;
    }
    return true;
}

void
sc_activity_fps_notify_activity(void *userdata) {
    struct sc_activity_fps *af = userdata;

    sc_mutex_lock(&af->mutex);
    if (af->state > 0) {
        // We've previously reduced fps; restore active fps.
        send_fps(af->controller, af->config.fps_active, af->bitrate_active);
        af->state = 0;
    }
    af->deadline = sc_tick_now() + SC_TICK_FROM_SEC(af->config.timeout1);
    sc_cond_signal(&af->cond);
    sc_mutex_unlock(&af->mutex);
}

void
sc_activity_fps_reconfigure(struct sc_activity_fps *af,
                             const struct sc_activity_fps_config *config) {
    sc_mutex_lock(&af->mutex);
    apply_config(af, config);
    af->state = 0;
    send_fps(af->controller, af->config.fps_active, af->bitrate_active);
    af->deadline = sc_tick_now() + SC_TICK_FROM_SEC(af->config.timeout1);
    sc_cond_signal(&af->cond);
    sc_mutex_unlock(&af->mutex);
}

void
sc_activity_fps_stop(struct sc_activity_fps *af) {
    sc_mutex_lock(&af->mutex);
    af->stopped = true;
    sc_cond_signal(&af->cond);
    sc_mutex_unlock(&af->mutex);
}

void
sc_activity_fps_join(struct sc_activity_fps *af) {
    sc_thread_join(&af->thread, NULL);
}

void
sc_activity_fps_destroy(struct sc_activity_fps *af) {
    sc_cond_destroy(&af->cond);
    sc_mutex_destroy(&af->mutex);
}
