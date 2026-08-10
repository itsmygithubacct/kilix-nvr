#ifndef KNVR_CONFIG_H
#define KNVR_CONFIG_H

/*
 * What each camera is allowed to do.
 *
 * The cameras themselves - names, URLs, groups - already live in
 * kilix-rtsp's cameras.conf, and are read from there with
 * krtsp_config_load().  This is the other half: the per-camera policy that
 * says whether a camera records, whether it looks for motion, whether it
 * runs a detector and when.  Two files rather than one because they have
 * different owners: the URLs are the operator's description of the
 * hardware, and this is the operator's description of what to do with it.
 *
 * Everything defaults to off.  A camera that has just been added views and
 * nothing else, and that is the safety property the rest of the design
 * leans on: no recording starts, no detector runs, and no disk fills
 * because somebody typed a URL.  Turning a capability on is always a
 * deliberate act.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KNVR_NAME_MAX 64
#define KNVR_CAMERA_MAX 64

/*
 * How a camera records.  These are modes rather than flags because they
 * are mutually exclusive: a camera writing continuous segments is not also
 * writing event clips of the same footage.
 */
typedef enum knvr_record_mode {
    KNVR_RECORD_OFF = 0,
    KNVR_RECORD_STILLS,      /* a frame per event */
    KNVR_RECORD_CLIPS,       /* re-encoded event clips; the later phase */
    KNVR_RECORD_CONTINUOUS   /* segments, always, with pre-roll for free */
} knvr_record_mode;

/*
 * When the object detector runs.  `on-view` is the interesting one: it
 * detects only while somebody is watching, which is cheap and honest about
 * being blind the rest of the time - a fact `cameras` reports and nothing
 * else does.
 */
typedef enum knvr_detect_policy {
    KNVR_DETECT_OFF = 0,
    KNVR_DETECT_ALWAYS,
    KNVR_DETECT_ON_VIEW
} knvr_detect_policy;

typedef struct knvr_camera {
    char name[KNVR_NAME_MAX];

    knvr_record_mode record;
    knvr_detect_policy detect;
    bool motion;
    bool audio;          /* record sound alongside video */
    bool sound_events;   /* and run the sound-event detector */

    /* Days to keep this camera's footage.  0 means the global size cap is
     * the only limit, which is a choice rather than an oversight: a camera
     * whose retention nobody set should not quietly delete evidence. */
    int retain_days;

    /* Mask painted with kilix-mask, or empty for none.  Relative to the
     * config directory, so a data directory stays movable. */
    char mask[KNVR_NAME_MAX * 2];
} knvr_camera;

typedef struct knvr_config knvr_config;

/* ------------------------------- lifetime ------------------------------- */

/*
 * Open the policy store, creating it if this is the first run.  `path` is
 * the sqlite file; NULL uses the conventional location under the state
 * directory.
 */
bool knvr_config_open(knvr_config **config, const char *path);
void knvr_config_close(knvr_config *config);

/* The last failure, as a short phrase for an error message, or NULL. */
const char *knvr_config_error(const knvr_config *config);

/* -------------------------------- cameras ------------------------------- */

/*
 * Add a camera with every capability off.  Fails if the name is already
 * known - changing one is what `set` is for, and silently overwriting a
 * camera's policy because its name was typed twice is how a recording
 * stops without anybody noticing.
 */
bool knvr_config_add(knvr_config *config, const char *name);
bool knvr_config_remove(knvr_config *config, const char *name);

/* Read one camera's policy.  Returns false when the name is unknown. */
bool knvr_config_get(
    const knvr_config *config, const char *name, knvr_camera *out);

/* Write it back.  The name field selects the row. */
bool knvr_config_put(knvr_config *config, const knvr_camera *camera);

/*
 * Every camera with a policy, ordered by name.  Writes up to `capacity`
 * and reports the number available through `count`, so a caller can tell a
 * full buffer from an exact fit.
 */
bool knvr_config_list(
    const knvr_config *config, knvr_camera *out, size_t capacity,
    size_t *count);

/* ------------------------------- settings ------------------------------- */

/*
 * Apply one `key=value` to a camera in memory.
 *
 * Parsing lives here rather than in the command so that the command, a
 * future config file and any test all reject the same strings.  Returns
 * false with a reason for an unknown key or an unparseable value; an
 * out-of-range number is a refusal, not a clamp, because a silently
 * clamped retention is a promise the operator did not make.
 */
bool knvr_camera_set(
    knvr_camera *camera, const char *assignment, const char **reason);

/* The spellings `set` accepts and `cameras` prints, so the two cannot
 * drift apart. */
const char *knvr_record_mode_name(knvr_record_mode mode);
const char *knvr_detect_policy_name(knvr_detect_policy policy);

/*
 * True when this camera is configured to detect but will not be looking
 * right now.  Surfaced by `cameras` and deliberately nowhere else: it is a
 * fact about the policy, not an error, and a warning printed on every
 * frame would train people to ignore it.
 */
bool knvr_camera_is_blind(const knvr_camera *camera, bool viewer_attached);

#ifdef __cplusplus
}
#endif

#endif /* KNVR_CONFIG_H */
