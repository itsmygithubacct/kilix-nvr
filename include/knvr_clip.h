#ifndef KNVR_CLIP_H
#define KNVR_CLIP_H

/*
 * Events cut out of the segments they span.
 *
 * `record=clips` is this: capture continuously, because pre-roll cannot
 * be invented after the fact, cut each event out when it closes, and let
 * the segments behind it go early while the clips keep the camera's
 * retention.  That is the arrangement every NVR converges on, and the
 * reason is that the ten seconds before somebody walks into frame is the
 * part you actually want and the only way to have it is to have been
 * recording already.
 *
 * The cut is `-c copy`, so it costs I/O and no CPU and the clip is
 * bit-identical to the archive.  It also means the clip starts at the
 * keyframe at or before the moment asked for, which is right for footage
 * and wrong for anything counting frames.
 *
 * Two ways to run it, deliberately.  The recorder cannot wait - it has
 * cameras to decode - so it starts one and collects it later, the same
 * shape as the detector.  A person typing `kilix-nvr clip 12` has nothing
 * else to do and gets the blocking one.
 */

#include "knvr_config.h"
#include "knvr_store.h"

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* The segment file containing a moment, or false when nothing covers it. */
bool knvr_clip_segment_covering(
    const char *camera, int64_t at, char *out, size_t size);

/*
 * Cut an event and index it, waiting for ffmpeg.
 *
 * `out`, when not NULL, receives the path written.  False means there was
 * no footage covering the event, or the cut failed.
 */
bool knvr_clip_cut(
    knvr_store *store, const knvr_event *event, char *out, size_t size);

/*
 * The same cut, started and left running.
 *
 * Returns the child's pid, or -1.  `out` receives the path it is writing,
 * which the caller keeps until knvr_clip_finish() succeeds - ffmpeg is
 * told the name once and nothing else knows it.
 */
pid_t knvr_clip_start(const knvr_event *event, char *out, size_t size);

/*
 * Whether the footage for an event is finished being written.
 *
 * Cutting the instant an event closes reads a segment ffmpeg still has
 * open, so the tail of the clip is whatever happened to be flushed - the
 * last seconds, which are the ones somebody is going to watch.  A cut is
 * ready once a later segment exists, because that means the one holding
 * the event was closed and completed.
 *
 * `patience` seconds after the event ended it is called ready anyway: a
 * camera that stops producing frames would otherwise hold its last event
 * unclipped for ever.
 */
bool knvr_clip_ready(const knvr_event *event, int64_t now, int patience);

/*
 * Collect a started cut without blocking.
 *
 * Returns false while it is still running.  When it has finished, the
 * media row is added if it succeeded, `*ok` says which, and the caller's
 * pid is spent.
 */
bool knvr_clip_finish(
    knvr_store *store, pid_t child, int64_t event_id, const char *path,
    bool *ok);

/*
 * Delete segments nothing needs any more.
 *
 * Segments are the one thing retention never touched: they are written by
 * ffmpeg without telling the store, so nothing indexed them and nothing
 * deleted them.  A camera on `continuous` therefore filled the disk with
 * a prune timer running and reporting success, which is the worst shape a
 * retention bug can take.
 *
 * The rule follows the camera's mode.  `continuous` keeps segments for
 * its own retain_days, because the segments *are* the archive.  `clips`
 * keeps them only long enough to cut from - the clips are the archive
 * there, and holding both would be paying for continuous twice.
 */
bool knvr_clip_prune_segments(
    knvr_config *config, int64_t now, bool dry_run,
    knvr_prune_result *result);

#endif /* KNVR_CLIP_H */
