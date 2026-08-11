#ifndef KNVR_RUN_H
#define KNVR_RUN_H

/*
 * Every camera at once, in one process, until told to stop.
 *
 * This is the shape a recorder is actually deployed in, and its absence
 * was the largest gap in this program: `watch` runs one camera in the
 * foreground, so a machine with five cameras needed five terminals and
 * nothing survived a logout.  `run` is the same pipeline over a list, and
 * `watch` is now this with one camera and the per-frame commentary turned
 * on - one implementation, because two recorders that drift apart is the
 * failure this whole family is arranged to avoid.
 *
 * It also publishes.  Each camera's decoded frames go into a named shared
 * ring, so a viewer on the same machine reads the frames this process
 * already decoded rather than opening a second session to the camera.
 * That is the payoff kilix-rtsp's cross-process ring was built for, and
 * it needed a publisher to exist before it could be used.
 */

#include "knvr_config.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct knvr_run_options {
    /* Names to run, or NULL for every camera with a policy. */
    const char *const *cameras;
    size_t count;

    /* Stop after this long; 0 runs until SIGINT or SIGTERM. */
    int seconds;

    /*
     * Print what each camera is doing, frame by frame.
     *
     * Right for one camera being watched by a person, wrong for five
     * running as a service: a line per motion frame per camera is a log
     * nobody reads and a disk nobody expected to fill.
     */
    bool verbose;

    /* Write the first frame that had motion here, boxes drawn on it. */
    const char *render;

    /*
     * Share each camera's frames for a viewer to attach to.  Default on.
     *
     * The object is named for the camera and forced to mode 0600: frames
     * are not authenticated, so anything able to open it can see the
     * camera.  Off for a run that should leave nothing behind.
     */
    bool publish;
} knvr_run_options;

void knvr_run_options_init(knvr_run_options *options);

/*
 * The name of a camera's shared ring, so a viewer can find it.  Composed
 * rather than guessed at by two programs.
 */
bool knvr_run_ring_name(const char *camera, char *out, size_t size);

int knvr_run(knvr_config *config, const knvr_run_options *options);

#endif /* KNVR_RUN_H */
