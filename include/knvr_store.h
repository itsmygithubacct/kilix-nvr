#ifndef KNVR_STORE_H
#define KNVR_STORE_H

/*
 * What happened, and when.
 *
 * An event opens when a camera starts moving and closes when it stops.
 * Detections and media hang off it, so a person reviewing the day reads a
 * list of things that happened rather than a list of frames.
 *
 * Times are Unix seconds, UTC, everywhere.  That is not a style choice:
 * the archive this design was measured against is stamped UTC while the
 * machine is UTC-7, and a "night" sample taken by local hour turned out
 * to be an afternoon one.  Local time is a presentation concern and is
 * converted at the edge, once.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KNVR_STORE_NAME_MAX 64
#define KNVR_STORE_LABEL_MAX 32
#define KNVR_STORE_PATH_MAX 512
#define KNVR_STORE_ZONES_MAX 128

typedef struct knvr_store knvr_store;

/* Why an event opened.  Motion is the gate; a detection is what the
 * detector made of it afterwards. */
typedef enum knvr_trigger {
    KNVR_TRIGGER_MOTION = 0,
    KNVR_TRIGGER_SOUND
} knvr_trigger;

typedef struct knvr_event {
    int64_t id;
    char camera[KNVR_STORE_NAME_MAX];
    int64_t started;      /* Unix seconds, UTC */
    int64_t ended;        /* 0 while still open */
    knvr_trigger trigger;
    int64_t motion_frames;
    /* Best detection score on this event, 0 when nothing was detected.
     * Stored rather than derived so the common query - "show me events
     * that were probably a person" - does not join every detection. */
    double best_score;
    char best_label[KNVR_STORE_LABEL_MAX];
} knvr_event;

typedef struct knvr_detection {
    int64_t event;
    int64_t at;
    char label[KNVR_STORE_LABEL_MAX];
    double score;
    int x;
    int y;
    int w;
    int h;
    /* The tracked object this detection belongs to, or 0 when nothing was
     * tracking - a sound event, or a detection from before tracking
     * existed. */
    int64_t track;
    /* The zone it was standing in, or empty. */
    char zone[KNVR_STORE_LABEL_MAX];
} knvr_detection;

/*
 * A tracked object: one row for one thing that was there, however many
 * frames it appeared in.
 *
 * `track` is the tracker's id, unique only within the run that produced
 * it, so identity in the store is the (event, track) pair.
 */
typedef struct knvr_object {
    int64_t id;
    int64_t event;
    int64_t track;
    char camera[KNVR_STORE_NAME_MAX];
    char label[KNVR_STORE_LABEL_MAX];
    double score;
    int64_t first_seen;
    int64_t last_seen;
    int travelled;      /* px the centroid covered */
    bool stationary;
    /*
     * On write: the zone it is in now, or empty.  On read: every zone it
     * has been in, comma-separated.  Asymmetric on purpose - the caller
     * knows where the thing is, and only the store can know where it has
     * been.
     */
    char zone[KNVR_STORE_ZONES_MAX];
} knvr_object;

/* ------------------------------- lifetime ------------------------------- */

/* `path` may be NULL for the conventional location under the state
 * directory. */
bool knvr_store_open(knvr_store **store, const char *path);
void knvr_store_close(knvr_store *store);
const char *knvr_store_error(const knvr_store *store);

/* -------------------------------- events -------------------------------- */

/*
 * Open an event, or return the one already open for this camera.
 *
 * Reopening rather than duplicating is what makes the caller's loop
 * simple: it can say "something moved" on every frame without tracking
 * whether it has said so already.
 */
bool knvr_store_event_open(
    knvr_store *store, const char *camera, knvr_trigger trigger,
    int64_t at, int64_t *event_id);

/* Note another frame of movement against the open event. */
bool knvr_store_event_touch(knvr_store *store, int64_t event_id);

/* Close it.  Closing an already-closed event is not an error: the quiet
 * timer and a shutdown can both reach for it. */
bool knvr_store_event_close(
    knvr_store *store, int64_t event_id, int64_t at);

/* Close every event a camera has left open - what a restart needs, since
 * an event open forever is an event that never appears in a query with an
 * end time. */
bool knvr_store_close_stale(knvr_store *store, const char *camera,
                            int64_t at);

bool knvr_store_add_detection(
    knvr_store *store, const knvr_detection *detection);

/*
 * Record a tracked object, or update the one already recorded.
 *
 * Called on every frame the object is seen: the row is keyed on
 * (event, track), the score only ever rises, and the zone is added to the
 * list of zones it has been in rather than replacing it.
 */
bool knvr_store_put_object(knvr_store *store, const knvr_object *object);

bool knvr_store_objects(
    const knvr_store *store, int64_t event_id, knvr_object *out,
    size_t capacity, size_t *count);

/* A still, a clip or a segment belonging to an event.  `kind` is the
 * caller's word for it; the store does not interpret it. */
bool knvr_store_add_media(
    knvr_store *store, int64_t event_id, const char *kind,
    const char *path);

/* -------------------------------- queries ------------------------------- */

typedef struct knvr_query {
    /* Unix seconds; 0 means unbounded. */
    int64_t since;
    int64_t until;
    /* Empty means every camera. */
    char camera[KNVR_STORE_NAME_MAX];
    /* Only events whose best detection scored at least this. */
    double min_score;
    /* Only events with an object that was in this zone.  Empty means any,
     * and a name no zone has means none. */
    char zone[KNVR_STORE_LABEL_MAX];
    /* 0 means the store's default. */
    int limit;
} knvr_query;

void knvr_query_init(knvr_query *query);

/*
 * Newest first, which is the order a person reviewing wants.  `count`
 * receives how many were written; a caller wanting more asks with a
 * bigger buffer rather than paging, because the limit is the point.
 */
bool knvr_store_events(
    const knvr_store *store, const knvr_query *query, knvr_event *out,
    size_t capacity, size_t *count);

typedef struct knvr_media {
    char kind[KNVR_STORE_LABEL_MAX];
    char path[KNVR_STORE_PATH_MAX];
    int64_t bytes;
} knvr_media;

/* The files belonging to an event, for review and for playback. */
bool knvr_store_media(
    const knvr_store *store, int64_t event_id, knvr_media *out,
    size_t capacity, size_t *count);

bool knvr_store_detections(
    const knvr_store *store, int64_t event_id, knvr_detection *out,
    size_t capacity, size_t *count);

/*
 * What a camera has seen lately, newest first.
 *
 * For a live viewer attached to a recorder's frames: it knows the camera
 * on screen and has no reason to know which event is open, and asking
 * the recorder's answer beats loading a second copy of the model to
 * re-derive it.
 */
bool knvr_store_recent_detections(
    const knvr_store *store, const char *camera, int64_t since,
    knvr_detection *out, size_t capacity, size_t *count);

/* -------------------------------- the pulse ------------------------------ */

/*
 * How much a camera was moving and how loud it was, once a second.
 *
 * Both numbers already exist on every frame - kmd_result.motion_fraction
 * and ksd_level() - and both were thrown away.  Kept, they turn a camera
 * into something you can read at a glance over an hour: a bark with
 * nothing moving is a different fact from a bark with a shape at the
 * gate, and no list of events shows that.
 *
 * Peak rather than mean, deliberately: a loud instant in a quiet second
 * is the thing worth seeing, and averaging is how it disappears.
 */
typedef struct knvr_pulse {
    int64_t at;      /* Unix seconds, UTC */
    float motion;    /* 0..1, the second's peak */
    float audio;     /* 0..1, the second's peak */
} knvr_pulse;

/*
 * Record one second.  Calling it repeatedly within the same second keeps
 * the larger value, so a caller can hand over every frame without
 * tracking which second it is in.
 */
bool knvr_store_pulse(
    knvr_store *store, const char *camera, int64_t at, float motion,
    float audio);

/*
 * The series between two times, resampled into `count` buckets.
 *
 * Resampled here rather than in the drawing, because the width of a
 * strip in pixels is what decides how many buckets there are, and a
 * caller asking for a day at 900 pixels wants 900 numbers rather than
 * 86,400.  Empty buckets read as zero, which is the truth: nothing was
 * recorded then.
 */
bool knvr_store_pulse_series(
    const knvr_store *store, const char *camera, int64_t from, int64_t to,
    knvr_pulse *out, size_t count);

/* ------------------------------- retention ------------------------------ */

typedef struct knvr_retention {
    /* Per-camera days; 0 disables the age rule for that camera. */
    int days;
    char camera[KNVR_STORE_NAME_MAX];
} knvr_retention;

typedef struct knvr_prune_result {
    size_t events_removed;
    size_t media_removed;
    uint64_t bytes_freed;
} knvr_prune_result;

/*
 * Apply one camera's age rule.
 *
 * Media files are unlinked, not merely forgotten: a retention policy that
 * leaves the footage on disk is not a retention policy.  `dry_run`
 * reports what would go without touching anything, because the first
 * question about a delete is always "what exactly".
 */
bool knvr_store_prune_age(
    knvr_store *store, const knvr_retention *rule, int64_t now,
    bool dry_run, knvr_prune_result *result);

/*
 * The backstop: delete oldest-first across every camera until the whole
 * store is under `limit_bytes`.
 *
 * Behind the per-camera rule rather than instead of it, because a size cap
 * alone silently shortens the retention of whichever camera happens to be
 * busiest.
 */
bool knvr_store_prune_size(
    knvr_store *store, uint64_t limit_bytes, bool dry_run,
    knvr_prune_result *result);

#ifdef __cplusplus
}
#endif

#endif /* KNVR_STORE_H */
