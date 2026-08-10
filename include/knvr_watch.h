#ifndef KNVR_WATCH_H
#define KNVR_WATCH_H

/*
 * One camera, decoded once, with motion boxes over it.
 *
 * This is the spine of the recorder and the first thing worth looking at:
 * frames arrive from a supervised ffmpeg through kilix-rtsp, motion runs
 * over a downscaled copy through kilix-motion-detect, and what it found is
 * drawn on the frame.  Everything later - events, detection, recording -
 * hangs off the same loop.
 *
 * Motion is the gate the whole design sits behind, so it is made visible
 * before anything depends on it.  A gate whose false-positive rate nobody
 * has watched is a gate that wakes a detector all night.
 */

#include "knvr_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KNVR_MOTION_BOX_MAX 32

typedef struct knvr_box {
    int x;
    int y;
    int w;
    int h;
} knvr_box;

typedef struct knvr_watch knvr_watch;

typedef struct knvr_watch_options {
    /* Decoded size.  Defaults to the camera's substream shape: measured,
     * because 320 wide loses subjects under about 50 px. */
    int width;
    int height;

    int fps_cap;

    /* Mask file painted with kilix-mask, or NULL.  Its painted region is
     * what motion IGNORES - kmask_expand_exclude() already produces the
     * polarity kmd_detector_set_mask() wants, so neither side inverts. */
    const char *mask_path;

    /*
     * Continuous recording target, or NULL for none.
     *
     * When `record_url` is also set, the archive is written by its own
     * ffmpeg reading the camera's MAIN stream while motion keeps
     * differencing the substream.  That second process never decodes -
     * it copies the camera's own bitstream to disk - so it costs I/O and
     * nothing else, and it is the reason the archive is full quality
     * while the differencing stays cheap.
     *
     * With `record_url` NULL the archive comes off the same stream this
     * decodes, which is one process but substream quality.
     */
    const char *record_url;
    const char *record_dir;
    int segment_seconds;
    bool record_audio;

    /* Where ffmpeg's stderr goes.  NULL discards it, and it must never be
     * the terminal: one warning printed into the alternate screen corrupts
     * the display, and a flaky camera produces plenty. */
    const char *log_path;
} knvr_watch_options;

typedef struct knvr_watch_stats {
    uint64_t frames;         /* borrowed from the source */
    uint64_t motion_frames;  /* ...that had any motion at all */
    uint64_t boxes;          /* total boxes reported */
    int last_age_ms;         /* age of the newest frame; a wedged camera
                              * keeps one available forever, so this is the
                              * only thing that distinguishes it from a
                              * still scene */
} knvr_watch_stats;

void knvr_watch_options_init(knvr_watch_options *options);

/*
 * Start supervising one camera.  `url` is its stream; the caller resolves
 * it from kilix-rtsp's config so this never parses a config file and never
 * holds a credential longer than the call.
 */
bool knvr_watch_start(
    knvr_watch **watch, const char *url, const knvr_watch_options *options);
void knvr_watch_stop(knvr_watch *watch);

/* The last failure as a short phrase, or NULL.  Never contains a URL. */
const char *knvr_watch_error(const knvr_watch *watch);

int knvr_watch_width(const knvr_watch *watch);
int knvr_watch_height(const knvr_watch *watch);

/*
 * Take the newest frame and run motion over it.
 *
 * Returns false when no frame has arrived yet, which is ordinary during
 * startup rather than an error.  On success `rgba` points at the frame for
 * the caller to draw or present - borrowed until the next call, never
 * freed by the caller - and `boxes` receives the motion regions in frame
 * coordinates.
 */
bool knvr_watch_step(
    knvr_watch *watch, const uint8_t **rgba, knvr_box *boxes,
    size_t capacity, size_t *count);

void knvr_watch_get_stats(const knvr_watch *watch, knvr_watch_stats *out);

/*
 * Draw motion boxes onto a frame in place.
 *
 * Here rather than in the caller because both the terminal view and the
 * headless render want the same picture, and a debug overlay that differs
 * between the two is a debug overlay that lies about one of them.
 */
void knvr_watch_draw_boxes(
    uint8_t *rgba, int width, int height, const knvr_box *boxes,
    size_t count);

#ifdef __cplusplus
}
#endif

#endif /* KNVR_WATCH_H */
