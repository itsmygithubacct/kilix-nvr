/*
 * Zone maps, over kilix-mask.
 *
 * Everything structural here is the mask module's: the file, the painting,
 * the names, the attributes.  What this adds is the reading of that file
 * as policy, and the small amount of memory it takes to say "this object
 * has been in the drive for three frames" rather than "something is in
 * the drive".
 */

#include "knvr_zone.h"

#include "kilix_mask.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ERROR_MAX 160
#define WATCHED_MAX 64

/* Source pixels per stored cell for a new map.  Four keeps a zone map
 * over a main-stream still down to a few kilobytes while staying finer
 * than any boundary a person paints by hand. */
#define ZONE_CELL 4

typedef struct watched {
    int64_t track;
    uint8_t region;
    int hits;
    int64_t since;
    bool used;
} watched;

struct knvr_zones {
    kmask *mask;
    int frame_width;
    int frame_height;
    knvr_zone list[KNVR_ZONE_MAX];
    size_t count;
    watched watching[WATCHED_MAX];
    char error[ERROR_MAX];
};

static int attr_int(const kmask *mask, uint8_t region, const char *key,
                    int fallback)
{
    const char *value = kmask_region_attr(mask, region, key);
    long parsed;
    char *end = NULL;

    if (value == NULL || value[0] == '\0') {
        return fallback;
    }
    parsed = strtol(value, &end, 10);
    if (end == value || parsed < 0 || parsed > 100000) {
        return fallback;
    }
    return (int)parsed;
}

static bool attr_bool(const kmask *mask, uint8_t region, const char *key)
{
    const char *value = kmask_region_attr(mask, region, key);

    return value != NULL &&
           (strcmp(value, "yes") == 0 || strcmp(value, "on") == 0 ||
            strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
}

bool knvr_zones_load(
    knvr_zones **out, const char *path, int frame_width, int frame_height)
{
    knvr_zones *zones;
    size_t counts[256];

    if (out == NULL) {
        return false;
    }
    *out = NULL;
    if (path == NULL || path[0] == '\0' || frame_width <= 0 ||
        frame_height <= 0) {
        return false;
    }
    zones = calloc(1u, sizeof(*zones));
    if (zones == NULL) {
        return false;
    }
    zones->frame_width = frame_width;
    zones->frame_height = frame_height;
    if (!kmask_load(&zones->mask, path)) {
        (void)snprintf(zones->error, sizeof(zones->error),
                       "not a zone map written by kilix-mask");
        free(zones);
        return false;
    }
    kmask_counts(zones->mask, counts);
    for (unsigned region = 1u; region <= KMASK_REGION_MAX; region++) {
        const char *name = kmask_region_name(zones->mask, (uint8_t)region);
        knvr_zone *zone;

        if (counts[region] == 0u && (name == NULL || name[0] == '\0')) {
            continue;
        }
        if (zones->count == KNVR_ZONE_MAX) {
            break;
        }
        zone = &zones->list[zones->count++];
        zone->region = (uint8_t)region;
        /* An unnamed but painted region is still a zone; calling it by
         * its id beats pretending it is not there. */
        if (name != NULL && name[0] != '\0') {
            (void)snprintf(zone->name, sizeof(zone->name), "%s", name);
        } else {
            (void)snprintf(zone->name, sizeof(zone->name), "zone%u", region);
        }
        zone->inertia = attr_int(zones->mask, (uint8_t)region, "inertia", 1);
        if (zone->inertia < 1) {
            zone->inertia = 1;
        }
        zone->preclusive = attr_bool(zones->mask, (uint8_t)region,
                                     "preclusive");
        zone->loiter_seconds = attr_int(zones->mask, (uint8_t)region,
                                        "loiter", 0);
        zone->cells = counts[region];
    }
    *out = zones;
    return true;
}

void knvr_zones_free(knvr_zones *zones)
{
    if (zones == NULL) {
        return;
    }
    kmask_free(zones->mask);
    free(zones);
}

const char *knvr_zones_error(const knvr_zones *zones)
{
    if (zones == NULL || zones->error[0] == '\0') {
        return NULL;
    }
    return zones->error;
}

size_t knvr_zones_count(const knvr_zones *zones)
{
    return zones == NULL ? 0u : zones->count;
}

const knvr_zone *knvr_zones_at(const knvr_zones *zones, size_t index)
{
    if (zones == NULL || index >= zones->count) {
        return NULL;
    }
    return &zones->list[index];
}

const knvr_zone *knvr_zones_find(const knvr_zones *zones, const char *name)
{
    if (zones == NULL || name == NULL) {
        return NULL;
    }
    for (size_t i = 0u; i < zones->count; i++) {
        if (strcmp(zones->list[i].name, name) == 0) {
            return &zones->list[i];
        }
    }
    return NULL;
}

const char *knvr_zone_name(const knvr_zones *zones, uint8_t region)
{
    if (zones == NULL || region == 0u) {
        return NULL;
    }
    for (size_t i = 0u; i < zones->count; i++) {
        if (zones->list[i].region == region) {
            return zones->list[i].name;
        }
    }
    return NULL;
}

uint8_t knvr_zones_at_point(const knvr_zones *zones, int x, int y)
{
    int source_x;
    int source_y;

    if (zones == NULL || zones->mask == NULL) {
        return 0u;
    }
    /* Frame coordinates into the geometry the map was painted at.  These
     * differ far more often than they match: the still someone paints
     * over comes off the main stream and the boxes come off the
     * substream. */
    source_x = (int)(((long)x * (long)kmask_source_width(zones->mask)) /
                     (long)zones->frame_width);
    source_y = (int)(((long)y * (long)kmask_source_height(zones->mask)) /
                     (long)zones->frame_height);
    return kmask_get_at(zones->mask, source_x, source_y);
}

static watched *watch_for(knvr_zones *zones, int64_t track)
{
    watched *free_slot = NULL;

    for (size_t i = 0u; i < WATCHED_MAX; i++) {
        if (zones->watching[i].used && zones->watching[i].track == track) {
            return &zones->watching[i];
        }
        if (!zones->watching[i].used && free_slot == NULL) {
            free_slot = &zones->watching[i];
        }
    }
    if (free_slot != NULL) {
        (void)memset(free_slot, 0, sizeof(*free_slot));
        free_slot->used = true;
        free_slot->track = track;
    }
    return free_slot;
}

bool knvr_zones_track(
    knvr_zones *zones, int64_t track_id, const knvr_detection_box *box,
    int64_t at_ms, knvr_zone_hit *hit)
{
    const knvr_zone *zone = NULL;
    watched *entry;
    uint8_t region;

    if (hit != NULL) {
        (void)memset(hit, 0, sizeof(*hit));
    }
    if (zones == NULL || box == NULL || hit == NULL) {
        return false;
    }
    /* The middle of the bottom edge: where it stands, not where its head
     * is. */
    region = knvr_zones_at_point(zones, box->x + box->w / 2,
                                 box->y + box->h - 1);
    entry = watch_for(zones, track_id);
    if (entry == NULL) {
        /* Out of slots.  Report the raw region without inertia rather
         * than reporting nothing: a zone that stops working silently is
         * worse than one that reacts a frame early. */
        hit->region = region;
        hit->settled = region != 0u;
        hit->since = at_ms;
        return true;
    }
    if (entry->region != region) {
        entry->region = region;
        entry->hits = 0;
        entry->since = at_ms;
    }
    if (entry->hits < 1000000) {
        entry->hits++;
    }
    for (size_t i = 0u; i < zones->count; i++) {
        if (zones->list[i].region == region) {
            zone = &zones->list[i];
            break;
        }
    }
    hit->region = region;
    hit->since = entry->since;
    hit->settled = region != 0u &&
                   (zone == NULL || entry->hits >= zone->inertia);
    hit->loitering = hit->settled && zone != NULL &&
                     zone->loiter_seconds > 0 &&
                     at_ms - entry->since >=
                         (int64_t)zone->loiter_seconds * 1000;
    return true;
}

void knvr_zones_forget(knvr_zones *zones, int64_t track_id)
{
    if (zones == NULL) {
        return;
    }
    for (size_t i = 0u; i < WATCHED_MAX; i++) {
        if (zones->watching[i].used && zones->watching[i].track == track_id) {
            zones->watching[i].used = false;
            return;
        }
    }
}

bool knvr_zone_is_preclusive(const knvr_zones *zones, uint8_t region)
{
    if (zones == NULL || region == 0u) {
        return false;
    }
    for (size_t i = 0u; i < zones->count; i++) {
        if (zones->list[i].region == region) {
            return zones->list[i].preclusive;
        }
    }
    return false;
}

/* ------------------------------- authoring ------------------------------ */

static bool save_attrs(kmask *mask, uint8_t region, int inertia,
                       bool preclusive, int loiter_seconds)
{
    char number[16];

    /* Every attribute is written, every time, including the defaults.
     * Region ids are reused as zones come and go, and an attribute left
     * behind by the last tenant is how a fresh zone turns out to be
     * preclusive. */
    (void)snprintf(number, sizeof(number), "%d", inertia);
    if (!kmask_region_set_attr(mask, region, "inertia", number)) {
        return false;
    }
    if (!kmask_region_set_attr(mask, region, "preclusive",
                               preclusive ? "yes" : "no")) {
        return false;
    }
    (void)snprintf(number, sizeof(number), "%d", loiter_seconds);
    return kmask_region_set_attr(mask, region, "loiter", number);
}

bool knvr_zones_define(
    const char *path, const char *name, int width, int height,
    int inertia, bool preclusive, int loiter_seconds, uint8_t *region,
    const char **reason)
{
    kmask *mask = NULL;
    size_t counts[256];
    uint8_t chosen = 0u;
    bool saved;

    if (reason != NULL) {
        *reason = NULL;
    }
    if (path == NULL || name == NULL || name[0] == '\0' || region == NULL) {
        if (reason != NULL) {
            *reason = "a zone needs a name";
        }
        return false;
    }
    if (strlen(name) >= KNVR_ZONE_NAME_MAX) {
        if (reason != NULL) {
            *reason = "that name is too long";
        }
        return false;
    }
    if (!kmask_load(&mask, path)) {
        if (width <= 0 || height <= 0) {
            if (reason != NULL) {
                *reason = "no zone map yet, and no frame size to make one";
            }
            return false;
        }
        if (!kmask_create(&mask, width, height, ZONE_CELL)) {
            if (reason != NULL) {
                *reason = "cannot create the zone map";
            }
            return false;
        }
    }
    kmask_counts(mask, counts);
    for (unsigned candidate = 1u; candidate <= KMASK_REGION_MAX;
         candidate++) {
        const char *existing = kmask_region_name(mask, (uint8_t)candidate);

        if (existing != NULL && strcmp(existing, name) == 0) {
            kmask_free(mask);
            if (reason != NULL) {
                *reason = "a zone with that name is already there";
            }
            return false;
        }
    }
    for (unsigned candidate = 1u; candidate <= KMASK_REGION_MAX;
         candidate++) {
        const char *existing = kmask_region_name(mask, (uint8_t)candidate);

        if (counts[candidate] == 0u &&
            (existing == NULL || existing[0] == '\0')) {
            chosen = (uint8_t)candidate;
            break;
        }
    }
    if (chosen == 0u) {
        kmask_free(mask);
        if (reason != NULL) {
            *reason = "every region id is taken";
        }
        return false;
    }
    if (!kmask_region_set_name(mask, chosen, name) ||
        !save_attrs(mask, chosen, inertia, preclusive, loiter_seconds)) {
        kmask_free(mask);
        if (reason != NULL) {
            *reason = "cannot record the zone";
        }
        return false;
    }
    saved = kmask_save(mask, path);
    kmask_free(mask);
    if (!saved) {
        if (reason != NULL) {
            *reason = "cannot write the zone map";
        }
        return false;
    }
    *region = chosen;
    return true;
}

bool knvr_zones_undefine(const char *path, const char *name,
                         const char **reason)
{
    kmask *mask = NULL;
    uint8_t found = 0u;
    bool saved;

    if (reason != NULL) {
        *reason = NULL;
    }
    if (path == NULL || name == NULL) {
        return false;
    }
    if (!kmask_load(&mask, path)) {
        if (reason != NULL) {
            *reason = "no zone map for that camera";
        }
        return false;
    }
    for (unsigned region = 1u; region <= KMASK_REGION_MAX; region++) {
        const char *existing = kmask_region_name(mask, (uint8_t)region);

        if (existing != NULL && strcmp(existing, name) == 0) {
            found = (uint8_t)region;
            break;
        }
    }
    if (found == 0u) {
        kmask_free(mask);
        if (reason != NULL) {
            *reason = "no zone with that name";
        }
        return false;
    }
    kmask_clear_region(mask, found);
    /* NULL clears; the empty string is refused as a name, so passing ""
     * would leave the zone named and the removal silently undone. */
    if (!kmask_region_set_name(mask, found, NULL)) {
        kmask_free(mask);
        if (reason != NULL) {
            *reason = "cannot clear the zone's name";
        }
        return false;
    }
    saved = kmask_save(mask, path);
    kmask_free(mask);
    if (!saved && reason != NULL) {
        *reason = "cannot write the zone map";
    }
    return saved;
}
