#ifndef KNVR_TRACK_H
#define KNVR_TRACK_H

/*
 * Detections into objects that persist.
 *
 * A detector answers "what is in this frame" and nothing more.  Ask it
 * twice and you get two answers with no relationship between them, which
 * is why a recorder built straight on detections reports the same person
 * forty times and cannot tell you whether the car in the drive arrived or
 * has been there since Tuesday.  A track is the missing noun: the same
 * object, seen repeatedly, with a beginning and an end.
 *
 * Deliberately not a Kalman filter.  Frigate reaches for norfair and
 * scipy; what an NVR actually needs is "is this the same car as last
 * frame", and box overlap plus a centroid gate answers that at a
 * thousandth of the complexity.  Frigate's own centroid tracker is still
 * in its tree as the simpler alternative.  Measure before reaching for
 * anything larger.
 *
 * Time is milliseconds, and every threshold that could have been a frame
 * count is a duration instead.  Detection runs only on frames that had
 * motion, so "five frames ago" can be five seconds or five minutes and a
 * frame-counted age would mean something different on every camera.
 */

#include "knvr_detect.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KNVR_TRACK_MAX 32
#define KNVR_TRACK_PATH 16

typedef struct knvr_point {
    int x;
    int y;
} knvr_point;

typedef struct knvr_track {
    int64_t id;             /* stable for as long as the object is seen */
    int class_id;
    float score;            /* the best this object has ever scored */
    knvr_detection_box box; /* where it was last seen */

    int64_t first_seen;     /* ms, from the caller's clock */
    int64_t last_seen;
    int hits;               /* frames it was matched in */
    int misses;             /* consecutive frames it was not */

    /*
     * True once it has been seen `min_hits` times.  A single frame is not
     * an object: a detector that hallucinates a bird for one frame should
     * not open an event, and this is the whole defence against that.
     */
    bool confirmed;

    /*
     * True when the centroid has stayed put for `stationary_ms`.  A parked
     * car must stop being an event - it is the single largest source of
     * useless recordings in every NVR that lacks this.
     */
    bool stationary;

    int travelled;          /* px the centroid has covered in total */
    knvr_point path[KNVR_TRACK_PATH];  /* newest last */
    size_t path_length;
} knvr_track;

typedef struct knvr_tracker knvr_tracker;

typedef struct knvr_tracker_options {
    /* Box overlap that counts as the same object.  0.2 default: low,
     * because a person walking towards a camera changes size fast and a
     * strict overlap starts a new track every few frames. */
    float iou_min;

    /* Frames before a track is believed.  Default 2. */
    int min_hits;

    /* How long an unseen track is kept before it is forgotten.  Default
     * 5000 ms - long enough to survive somebody walking behind a post,
     * short enough that two people a minute apart are two tracks. */
    int max_gap_ms;

    /* How far the centroid may drift and still count as parked, and for
     * how long.  Defaults 12 px and 30 s. */
    int stationary_px;
    int stationary_ms;
} knvr_tracker_options;

void knvr_tracker_options_init(knvr_tracker_options *options);

bool knvr_tracker_create(
    knvr_tracker **tracker, const knvr_tracker_options *options);
void knvr_tracker_free(knvr_tracker *tracker);

/*
 * One frame of detections in.
 *
 * `at_ms` is the caller's clock, and must not go backwards.  Tracks that
 * have not been seen for longer than `max_gap_ms` are dropped here rather
 * than on a timer, so a camera that stops producing frames does not need
 * a separate reaper.
 */
bool knvr_tracker_update(
    knvr_tracker *tracker, const knvr_detection_box *boxes, size_t count,
    int64_t at_ms);

size_t knvr_tracker_count(const knvr_tracker *tracker);
const knvr_track *knvr_tracker_at(const knvr_tracker *tracker, size_t index);

/*
 * The track a detection from the last update was assigned to, or 0.
 *
 * This is how a caller writes the track id alongside the detection it
 * stores, without the tracker having to know what a store is.
 */
int64_t knvr_tracker_assigned(
    const knvr_tracker *tracker, size_t detection_index);

/* How many objects this tracker has ever created - what makes "eleven
 * people today" a different number from "eleven detections". */
int64_t knvr_tracker_total(const knvr_tracker *tracker);

/*
 * Draw tracks onto a BGRA frame in place: the box in a colour derived
 * from the id, and the trail of where it has been.
 *
 * Identity is the thing being shown, so it is carried by colour rather
 * than by a number nobody can read at 640x360.  A stationary track is
 * drawn dimmed, because "still there" and "just arrived" must not look
 * the same.
 */
void knvr_track_draw(
    uint8_t *rgba, int width, int height, const knvr_tracker *tracker);

#ifdef __cplusplus
}
#endif

#endif /* KNVR_TRACK_H */
