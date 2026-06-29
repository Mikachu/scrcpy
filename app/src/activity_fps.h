#ifndef SC_ACTIVITY_FPS_H
#define SC_ACTIVITY_FPS_H

#include "common.h"

#include <stdbool.h>
#include <stdint.h>

#include "controller.h"
#include "util/thread.h"
#include "util/tick.h"

struct sc_activity_fps_config {
    float fps_active;
    float fps_idle1;   // 0 = disabled
    uint32_t timeout1; // seconds until idle1
    float fps_idle2;   // 0 = disabled
    uint32_t timeout2; // seconds until idle2
};

struct sc_activity_fps {
    struct sc_controller *controller;

    struct sc_activity_fps_config config;

    uint32_t bitrate_active;
    uint32_t bitrate_idle1;
    uint32_t bitrate_idle2;

    sc_thread thread;
    sc_mutex mutex;
    sc_cond cond;
    sc_tick deadline;
    int state;   // 0=active, 1=idle1, 2=idle2
    bool stopped;
};

// Parse "fps_active,timeout1,fps_idle1,timeout2,fps_idle2" or plain "fps".
// On success, fills *config and returns true.
bool
sc_activity_fps_parse_config(const char *s,
                             struct sc_activity_fps_config *config);

bool
sc_activity_fps_init(struct sc_activity_fps *af,
                     struct sc_controller *controller,
                     const struct sc_activity_fps_config *config,
                     uint32_t bitrate_active);

// Thread-safe; reconfigure at runtime (e.g. from terminal controller)
void
sc_activity_fps_reconfigure(struct sc_activity_fps *af,
                             const struct sc_activity_fps_config *config);

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
