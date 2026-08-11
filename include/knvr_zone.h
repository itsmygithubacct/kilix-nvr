#ifndef KNVR_ZONE_H
#define KNVR_ZONE_H

/*
 * Where in the picture something happened, and whether that matters.
 *
 * Motion tells you the scene changed and the detector tells you what
 * changed it.  Neither can tell you that a person on the pavement is
 * traffic while the same person six feet to the left is in the drive, and
 * that distinction is most of what makes a recorder worth reviewing.
 *
 * A zone map is a kilix-mask file: one named region per zone, painted
 * over a frame from the camera with `kilix mask`.  That is deliberate
 * reuse rather than a private polygon format - the painter, the file
 * format, the names and the free-form attributes all already exist, and
 * the map opens in any image viewer.  The cost is that regions cannot
 * overlap: a point is in exactly one zone or none.  Frigate and
 * ZoneMinder both allow overlapping polygons; painting says instead that
 * every pixel of the view belongs to one place, which is easier to reason
 * about and impossible to get subtly wrong.
 *
 * Policy rides in the region's attributes, so a zone map is self
 * describing and nothing has to be kept in step with it:
 *
 *   inertia=3       frames inside before the object counts as arrived
 *   preclusive=yes  activity here SUPPRESSES the event instead of raising
 *                   it - ZoneMinder's term and its semantics
 *   loiter=30       seconds before dwelling here is itself interesting
 */

#include "knvr_detect.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KNVR_ZONE_MAX 32
#define KNVR_ZONE_NAME_MAX 48

typedef struct knvr_zone {
    uint8_t region;      /* the id painted in the mask, 1..255 */
    char name[KNVR_ZONE_NAME_MAX];
    int inertia;         /* consecutive frames inside; default 1 */
    bool preclusive;     /* suppresses rather than raises */
    int loiter_seconds;  /* 0 disables */
    size_t cells;        /* painted cells, so `zones` can report coverage */
} knvr_zone;

typedef struct knvr_zones knvr_zones;

typedef struct knvr_zone_hit {
    uint8_t region;   /* 0 when the object is outside every zone */
    bool settled;     /* the region's inertia has been satisfied */
    bool loitering;   /* it has been here longer than `loiter` */
    int64_t since;    /* ms when it entered this region */
} knvr_zone_hit;

/* ------------------------------- lifetime ------------------------------- */

/*
 * Load a camera's zone map.
 *
 * `frame_width`/`frame_height` are the geometry the boxes will arrive in.
 * The map records the size it was painted at, so this is where the two
 * are reconciled - a map painted over a 2304x1296 still and consulted
 * against 640x360 motion boxes is the ordinary case, not the exception,
 * and getting it wrong puts every zone in the wrong place while looking
 * entirely plausible.
 */
bool knvr_zones_load(
    knvr_zones **zones, const char *path, int frame_width, int frame_height);
void knvr_zones_free(knvr_zones *zones);
const char *knvr_zones_error(const knvr_zones *zones);

size_t knvr_zones_count(const knvr_zones *zones);
const knvr_zone *knvr_zones_at(const knvr_zones *zones, size_t index);
const knvr_zone *knvr_zones_find(const knvr_zones *zones, const char *name);
const char *knvr_zone_name(const knvr_zones *zones, uint8_t region);

/* --------------------------------- tests -------------------------------- */

/*
 * Which zone a point in frame coordinates falls in, ignoring inertia.
 */
uint8_t knvr_zones_at_point(const knvr_zones *zones, int x, int y);

/*
 * Which zone an object is in, applying inertia and loitering.
 *
 * The point tested is the middle of the box's bottom edge - where the
 * thing touches the ground.  A centroid puts a tall person in the zone
 * their chest is over, which for a camera looking down a drive is
 * routinely the wrong one, and box overlap puts a lorry in four zones at
 * once.
 *
 * `track_id` is what makes inertia possible: the count belongs to the
 * object, not to the zone.  Call knvr_zones_forget() when a track ends,
 * or the table fills with objects that left.
 */
bool knvr_zones_track(
    knvr_zones *zones, int64_t track_id, const knvr_detection_box *box,
    int64_t at_ms, knvr_zone_hit *hit);
void knvr_zones_forget(knvr_zones *zones, int64_t track_id);

/*
 * True when a settled hit belongs to a preclusive zone, which the caller
 * treats as "do not raise this".
 */
bool knvr_zone_is_preclusive(const knvr_zones *zones, uint8_t region);

/* ------------------------------- authoring ------------------------------ */

/*
 * Create or extend a zone map: add a named region and save.
 *
 * Painting is `kilix mask`'s job; this exists because the editor paints
 * ids and a zone needs a name and a policy.  The region id is the lowest
 * free one, which is also the digit the operator presses in the editor.
 * Returns the id through `region`.
 *
 * `width`/`height` size a map that does not exist yet.  Adding to one
 * that does leaves its geometry alone.
 */
bool knvr_zones_define(
    const char *path, const char *name, int width, int height,
    int inertia, bool preclusive, int loiter_seconds, uint8_t *region,
    const char **reason);

/* Remove a named zone and everything painted with it. */
bool knvr_zones_undefine(
    const char *path, const char *name, const char **reason);

#ifdef __cplusplus
}
#endif

#endif /* KNVR_ZONE_H */
