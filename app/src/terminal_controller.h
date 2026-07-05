#ifndef SC_TERMINAL_CONTROLLER_H
#define SC_TERMINAL_CONTROLLER_H

#include "common.h"

#include <stdbool.h>

#include "audio_player.h"
#include "controller.h"
#include "screen.h"
#include "util/thread.h"

struct sc_terminal_controller {
    struct sc_screen *screen;       // NULL if no window
    struct sc_audio_player *ap;     // NULL if no audio playback
    struct sc_controller *controller; // NULL if no control
    struct sc_activity_fps *activity_fps; // NULL if not configured
    const char *start_app;

    sc_thread thread;
    int cancel_pipe[2]; // [0]=read end, [1]=write end
    bool muted;
};

bool
sc_terminal_controller_init(struct sc_terminal_controller *tc,
                             struct sc_screen *screen,
                             struct sc_audio_player *ap,
                             struct sc_controller *controller,
                             struct sc_activity_fps *activity_fps,
                             const char *start_app);

bool
sc_terminal_controller_start(struct sc_terminal_controller *tc);

void
sc_terminal_controller_stop(struct sc_terminal_controller *tc);

void
sc_terminal_controller_join(struct sc_terminal_controller *tc);

void
sc_terminal_controller_destroy(struct sc_terminal_controller *tc);

#endif
