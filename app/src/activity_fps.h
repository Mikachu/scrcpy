#ifndef SC_ACTIVITY_FPS_H
#define SC_ACTIVITY_FPS_H

#include "common.h"

#include <stdbool.h>
#include <stdint.h>

#include "controller.h"
#include "util/thread.h"
#include "util/tick.h"

struct sc_activity_fps {
    struct sc_controller *controller;

    float fps_active;
    float fps_idle1;
    uint32_t timeout1;  // seconds until idle1
    float fps_idle2;    // 0 = disabled
    uint32_t timeout2;  // seconds until idle2

    sc_thread thread;
    sc_mutex mutex;
    sc_cond cond;
    sc_tick deadline;
    int state;   // 0=active, 1=idle1, 2=idle2
    bool stopped;
};

bool
sc_activity_fps_init(struct sc_activity_fps *af,
                     struct sc_controller *controller,
                     float fps_active,
                     float fps_idle1, uint32_t timeout1,
                     float fps_idle2, uint32_t timeout2);

bool
sc_activity_fps_start(struct sc_activity_fps *af);

// Thread-safe; may be called from any thread (e.g. the SDL event loop)
void
sc_activity_fps_notify_activity(void *userdata);

void
sc_activity_fps_stop(struct sc_activity_fps *af);

void
sc_activity_fps_join(struct sc_activity_fps *af);

void
sc_activity_fps_destroy(struct sc_activity_fps *af);

#endif
