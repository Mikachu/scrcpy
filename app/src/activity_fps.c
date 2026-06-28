#include "activity_fps.h"

#include "control_msg.h"
#include "util/log.h"

static void
send_fps(struct sc_controller *controller, float fps) {
    struct sc_control_msg msg;
    msg.type = SC_CONTROL_MSG_TYPE_SET_MAX_FPS;
    msg.set_max_fps.max_fps = fps;
    if (!sc_controller_push_msg(controller, &msg)) {
        LOGW("Could not push set_max_fps message");
    } else {
        LOGI("Activity FPS: set max fps to %g", (double) fps);
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
                send_fps(af->controller, af->fps_idle1);
                af->state = 1;
                if (af->fps_idle2 != 0) {
                    af->deadline = sc_tick_now()
                                 + SC_TICK_FROM_SEC(af->timeout2);
                } else {
                    af->state = 2;
                }
            } else if (af->state == 1) {
                send_fps(af->controller, af->fps_idle2);
                af->state = 2;
            }
        }
        // If signaled (not timed out), deadline was already updated by
        // notify_activity; just loop and re-wait.
    }
    sc_mutex_unlock(&af->mutex);

    return 0;
}

bool
sc_activity_fps_init(struct sc_activity_fps *af,
                     struct sc_controller *controller,
                     float fps_active,
                     float fps_idle1, uint32_t timeout1,
                     float fps_idle2, uint32_t timeout2) {
    af->controller = controller;
    af->fps_active = fps_active;
    af->fps_idle1 = fps_idle1;
    af->timeout1 = timeout1;
    af->fps_idle2 = fps_idle2;
    af->timeout2 = timeout2;
    af->state = 0;
    af->stopped = false;

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
    af->deadline = sc_tick_now() + SC_TICK_FROM_SEC(af->timeout1);
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
        // send_fps acquires controller->mutex; that's fine because
        // controller never acquires af->mutex (no circular dependency).
        send_fps(af->controller, af->fps_active);
        af->state = 0;
    }
    af->deadline = sc_tick_now() + SC_TICK_FROM_SEC(af->timeout1);
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
