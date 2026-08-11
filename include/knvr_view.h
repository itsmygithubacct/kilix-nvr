#ifndef KNVR_VIEW_H
#define KNVR_VIEW_H

/*
 * Watching cameras, and what the models make of them.
 *
 * One program for the two things a person actually does with a recorder:
 * look at what is happening now, and go back through what happened.  Both
 * on the same screen furniture, because switching between "live" and
 * "history" should be a keypress rather than a different program.
 *
 * The frames come from kilix-rtsp, which is the only decoder in this
 * family and the one the recorder uses.  Today the viewer opens its own
 * source per camera it is showing; kilix-rtsp's cross-process ring
 * (krtsp_frame_attach) is what will let it read the recorder's frames
 * instead once something supervises the recorder, and that is the version
 * where watching a camera costs nothing extra.  Until then a viewer that
 * cannot show anything unless a daemon is running would be a viewer
 * nobody could open.
 *
 * What is drawn under every camera is the point:
 *
 *   motion  ▁▁▂▁▁▁▅█▇▄▂▁▁▁▁▂▁▁▁▁
 *   sound   ▁▁▁▁▁▁▂▃█▆▂▁▁▁▁▁▁▁▁▁
 *
 * Two signals a person can read at a glance, from numbers that were
 * already being computed on every frame and thrown away.
 */

#include "knvr_config.h"
#include "knvr_store.h"

#include <stdbool.h>

typedef struct knvr_view_options {
    /* One camera, or NULL for a grid of everything configured. */
    const char *camera;

    /*
     * Compose one frame to a PPM and exit, with no terminal involved.
     *
     * The only way to see what a full-screen program draws on a machine
     * that cannot show it, and the house pattern for every graphical tool
     * here.
     */
    const char *render;

    /* Stop after this long; 0 runs until quit. */
    int seconds;

    /* Start on the newest event's footage rather than on live. */
    bool replay;

    /*
     * Run the object detector on the camera being watched.  Default on
     * for one camera, off in a grid: a detector is hundreds of megabytes
     * and tens of milliseconds a frame, and nine of them is the machine.
     * The strips need neither - motion and level are arithmetic.
     */
    bool detect;
} knvr_view_options;

void knvr_view_options_init(knvr_view_options *options);

int knvr_view(
    knvr_config *config, knvr_store *store,
    const knvr_view_options *options);

#endif /* KNVR_VIEW_H */
