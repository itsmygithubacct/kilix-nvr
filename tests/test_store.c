#include "knvr_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sqlite3.h>

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            (void)fprintf(stderr, "%s:%d: check failed: %s\n",                \
                          __FILE__, __LINE__, #condition);                    \
            return false;                                                     \
        }                                                                     \
    } while (false)

static const char *STORE = "build/test-store.db";
static const int64_t DAY = 86400;
static const int64_t NOW = 1760000000;

static bool fresh(knvr_store **store)
{
    (void)remove(STORE);
    return knvr_store_open(store, STORE);
}

static bool write_file(const char *path, size_t bytes)
{
    FILE *file = fopen(path, "wb");

    if (file == NULL) {
        return false;
    }
    for (size_t i = 0u; i < bytes; i++) {
        (void)fputc('x', file);
    }
    return fclose(file) == 0;
}

/*
 * The property the caller's loop depends on: saying "something moved" on
 * every frame must not produce an event per frame.
 */
static bool
test_an_event_opens_once(void)
{
    knvr_store *store = NULL;
    int64_t first = 0;
    int64_t again = 0;
    knvr_event events[4];
    size_t count = 0u;

    CHECK(fresh(&store));
    CHECK(knvr_store_event_open(store, "yard", KNVR_TRIGGER_MOTION, NOW,
                                &first));
    for (int i = 0; i < 20; i++) {
        CHECK(knvr_store_event_open(store, "yard", KNVR_TRIGGER_MOTION,
                                    NOW + i, &again));
        CHECK(again == first);
        CHECK(knvr_store_event_touch(store, first));
    }
    CHECK(knvr_store_events(store, NULL, events, 4u, &count));
    CHECK(count == 1u);
    CHECK(events[0].motion_frames == 21);   /* the open plus 20 touches */
    CHECK(events[0].ended == 0);            /* still open */

    /* A different camera gets its own event. */
    CHECK(knvr_store_event_open(store, "drive", KNVR_TRIGGER_MOTION, NOW,
                                &again));
    CHECK(again != first);

    CHECK(knvr_store_event_close(store, first, NOW + 30));
    /* Closing twice is not an error, and must not rewrite the end time:
     * the quiet timer and a shutdown can both reach for it. */
    CHECK(knvr_store_event_close(store, first, NOW + 999));
    CHECK(knvr_store_events(store, NULL, events, 4u, &count));
    for (size_t i = 0u; i < count; i++) {
        if (events[i].id == first) {
            CHECK(events[i].ended == NOW + 30);
        }
    }

    /* And after the close, movement starts a new one. */
    CHECK(knvr_store_event_open(store, "yard", KNVR_TRIGGER_MOTION,
                                NOW + 40, &again));
    CHECK(again != first);

    knvr_store_close(store);
    return true;
}

/* A restart must not leave an event open forever: one with no end time
 * never appears in a query bounded by when it finished. */
static bool
test_stale_events_are_closed(void)
{
    knvr_store *store = NULL;
    int64_t id = 0;
    knvr_event events[8];
    size_t count = 0u;

    CHECK(fresh(&store));
    CHECK(knvr_store_event_open(store, "a", KNVR_TRIGGER_MOTION, NOW, &id));
    CHECK(knvr_store_event_open(store, "b", KNVR_TRIGGER_MOTION, NOW, &id));
    CHECK(knvr_store_close_stale(store, "", NOW + 5));
    CHECK(knvr_store_events(store, NULL, events, 8u, &count));
    CHECK(count == 2u);
    for (size_t i = 0u; i < count; i++) {
        CHECK(events[i].ended == NOW + 5);
    }
    knvr_store_close(store);
    return true;
}

/* The best score rides on the event so the common question - "what was
 * probably a person" - reads one table.  It only ever goes up. */
static bool
test_best_detection_rides_on_the_event(void)
{
    knvr_store *store = NULL;
    knvr_detection detection;
    knvr_event events[2];
    knvr_detection found[8];
    int64_t id = 0;
    size_t count = 0u;

    CHECK(fresh(&store));
    CHECK(knvr_store_event_open(store, "yard", KNVR_TRIGGER_MOTION, NOW,
                                &id));

    (void)memset(&detection, 0, sizeof(detection));
    detection.event = id;
    detection.at = NOW;
    detection.score = 0.31;
    (void)snprintf(detection.label, sizeof(detection.label), "%s", "cat");
    detection.w = 20;
    detection.h = 30;
    CHECK(knvr_store_add_detection(store, &detection));

    detection.score = 0.88;
    (void)snprintf(detection.label, sizeof(detection.label), "%s", "person");
    CHECK(knvr_store_add_detection(store, &detection));

    /* A worse one afterwards must not lower it. */
    detection.score = 0.12;
    (void)snprintf(detection.label, sizeof(detection.label), "%s", "bird");
    CHECK(knvr_store_add_detection(store, &detection));

    CHECK(knvr_store_events(store, NULL, events, 2u, &count));
    CHECK(count == 1u);
    CHECK(events[0].best_score > 0.87 && events[0].best_score < 0.89);
    CHECK(strcmp(events[0].best_label, "person") == 0);

    CHECK(knvr_store_detections(store, id, found, 8u, &count));
    CHECK(count == 3u);
    knvr_store_close(store);
    return true;
}

static bool
test_queries_filter(void)
{
    knvr_store *store = NULL;
    knvr_query query;
    knvr_event events[16];
    knvr_detection detection;
    int64_t id = 0;
    size_t count = 0u;

    CHECK(fresh(&store));
    for (int i = 0; i < 5; i++) {
        CHECK(knvr_store_event_open(store, i % 2 == 0 ? "yard" : "drive",
                                    KNVR_TRIGGER_MOTION,
                                    NOW + (int64_t)i * DAY, &id));
        CHECK(knvr_store_event_close(store, id, NOW + (int64_t)i * DAY + 10));
        if (i == 4) {
            (void)memset(&detection, 0, sizeof(detection));
            detection.event = id;
            detection.at = NOW;
            detection.score = 0.9;
            (void)snprintf(detection.label, sizeof(detection.label), "%s",
                           "person");
            CHECK(knvr_store_add_detection(store, &detection));
        }
    }

    knvr_query_init(&query);
    CHECK(knvr_store_events(store, &query, events, 16u, &count));
    CHECK(count == 5u);
    /* Newest first, which is the order a person reviewing wants. */
    CHECK(events[0].started > events[1].started);

    knvr_query_init(&query);
    (void)snprintf(query.camera, sizeof(query.camera), "%s", "yard");
    CHECK(knvr_store_events(store, &query, events, 16u, &count));
    CHECK(count == 3u);

    knvr_query_init(&query);
    query.since = NOW + 3 * DAY;
    CHECK(knvr_store_events(store, &query, events, 16u, &count));
    CHECK(count == 2u);

    knvr_query_init(&query);
    query.min_score = 0.5;
    CHECK(knvr_store_events(store, &query, events, 16u, &count));
    CHECK(count == 1u);
    CHECK(strcmp(events[0].best_label, "person") == 0);

    /* Reports what exists, not what fitted. */
    knvr_query_init(&query);
    CHECK(knvr_store_events(store, &query, events, 2u, &count));
    CHECK(count == 5u);
    knvr_store_close(store);
    return true;
}

/*
 * Retention that leaves the footage on disk is not retention.  This is
 * the check that would have caught it.
 */
static bool
test_age_retention_deletes_the_files(void)
{
    knvr_store *store = NULL;
    knvr_retention rule;
    knvr_prune_result result;
    int64_t old_event = 0;
    int64_t new_event = 0;
    knvr_event events[8];
    size_t count = 0u;
    const char *old_media = "build/test-store-old.bin";
    const char *new_media = "build/test-store-new.bin";

    CHECK(fresh(&store));
    CHECK(write_file(old_media, 1000u));
    CHECK(write_file(new_media, 500u));

    CHECK(knvr_store_event_open(store, "yard", KNVR_TRIGGER_MOTION,
                                NOW - 30 * DAY, &old_event));
    CHECK(knvr_store_add_media(store, old_event, "still", old_media));
    CHECK(knvr_store_event_close(store, old_event, NOW - 30 * DAY + 5));

    CHECK(knvr_store_event_open(store, "yard", KNVR_TRIGGER_MOTION,
                                NOW - DAY, &new_event));
    CHECK(knvr_store_add_media(store, new_event, "still", new_media));
    CHECK(knvr_store_event_close(store, new_event, NOW - DAY + 5));

    (void)memset(&rule, 0, sizeof(rule));
    rule.days = 7;
    (void)snprintf(rule.camera, sizeof(rule.camera), "%s", "yard");

    /* A dry run says what would go and touches nothing. */
    CHECK(knvr_store_prune_age(store, &rule, NOW, true, &result));
    CHECK(result.events_removed == 1u);
    CHECK(result.bytes_freed == 1000u);
    CHECK(access(old_media, F_OK) == 0);

    CHECK(knvr_store_prune_age(store, &rule, NOW, false, &result));
    CHECK(result.events_removed == 1u);
    CHECK(result.media_removed == 1u);
    CHECK(result.bytes_freed == 1000u);
    /* The file is gone, not merely forgotten. */
    CHECK(access(old_media, F_OK) != 0);
    CHECK(access(new_media, F_OK) == 0);

    CHECK(knvr_store_events(store, NULL, events, 8u, &count));
    CHECK(count == 1u);
    CHECK(events[0].id == new_event);

    /* days = 0 is "no age rule", not "delete everything". */
    rule.days = 0;
    CHECK(knvr_store_prune_age(store, &rule, NOW, false, &result));
    CHECK(result.events_removed == 0u);
    CHECK(knvr_store_events(store, NULL, events, 8u, &count));
    CHECK(count == 1u);

    (void)remove(new_media);
    knvr_store_close(store);
    return true;
}

/* The backstop stops the moment it is under the cap: everything removed
 * past that point is footage nobody asked to lose. */
static bool
test_size_cap_stops_at_the_limit(void)
{
    knvr_store *store = NULL;
    knvr_prune_result result;
    knvr_event events[16];
    size_t count = 0u;
    char path[64];

    CHECK(fresh(&store));
    for (int i = 0; i < 5; i++) {
        int64_t id = 0;

        (void)snprintf(path, sizeof(path), "build/test-store-%d.bin", i);
        CHECK(write_file(path, 1000u));
        CHECK(knvr_store_event_open(store, "yard", KNVR_TRIGGER_MOTION,
                                    NOW + (int64_t)i * DAY, &id));
        CHECK(knvr_store_add_media(store, id, "still", path));
        CHECK(knvr_store_event_close(store, id, NOW + (int64_t)i * DAY + 1));
    }

    /* 5000 bytes stored, capped at 2500: three must go, not five. */
    CHECK(knvr_store_prune_size(store, 2500u, false, &result));
    CHECK(result.events_removed == 3u);
    CHECK(result.bytes_freed == 3000u);

    CHECK(knvr_store_events(store, NULL, events, 16u, &count));
    CHECK(count == 2u);
    /* Oldest first, so what survives is the newest. */
    CHECK(events[0].started == NOW + 4 * DAY);

    /* Already under the cap: nothing happens. */
    CHECK(knvr_store_prune_size(store, 1000000u, false, &result));
    CHECK(result.events_removed == 0u);

    for (int i = 0; i < 5; i++) {
        (void)snprintf(path, sizeof(path), "build/test-store-%d.bin", i);
        (void)remove(path);
    }
    knvr_store_close(store);
    return true;
}

static bool
test_rejections(void)
{
    knvr_store *store = NULL;
    knvr_prune_result result;
    int64_t id = 0;

    CHECK(!knvr_store_open(NULL, STORE));
    CHECK(!knvr_store_open(&store, "/definitely/not/here/events.db"));

    CHECK(fresh(&store));
    CHECK(!knvr_store_event_open(store, NULL, KNVR_TRIGGER_MOTION, NOW, &id));
    CHECK(!knvr_store_event_open(store, "", KNVR_TRIGGER_MOTION, NOW, &id));
    CHECK(!knvr_store_add_detection(store, NULL));
    CHECK(!knvr_store_add_media(store, 1, NULL, "x"));
    CHECK(!knvr_store_events(NULL, NULL, NULL, 0u, NULL));
    CHECK(!knvr_store_prune_age(store, NULL, NOW, true, &result));
    CHECK(!knvr_store_prune_size(NULL, 0u, true, &result));
    CHECK(knvr_store_error(NULL) == NULL);
    knvr_store_close(store);
    knvr_store_close(NULL);
    return true;
}


/* -------------------------- objects and zones ---------------------------- */

static bool test_objects_are_one_row_per_thing(void)
{
    knvr_store *store = NULL;
    knvr_object object;
    knvr_object read[4];
    size_t count = 0u;
    int64_t event = 0;

    CHECK(fresh(&store));
    CHECK(knvr_store_event_open(store, "drive", KNVR_TRIGGER_MOTION, NOW,
                                &event));
    (void)memset(&object, 0, sizeof(object));
    object.event = event;
    object.track = 4;
    (void)snprintf(object.camera, sizeof(object.camera), "drive");
    (void)snprintf(object.label, sizeof(object.label), "person");
    object.score = 0.60;
    object.first_seen = NOW;
    object.last_seen = NOW;
    object.travelled = 10;
    CHECK(knvr_store_put_object(store, &object));

    /* Seen again: the same row, a better score, further travelled. */
    object.score = 0.90;
    object.last_seen = NOW + 6;
    object.travelled = 130;
    object.stationary = true;
    CHECK(knvr_store_put_object(store, &object));

    /* A lower score afterwards must not undo the best one. */
    object.score = 0.30;
    CHECK(knvr_store_put_object(store, &object));

    CHECK(knvr_store_objects(store, event, read, 4u, &count));
    CHECK(count == 1u);
    CHECK(read[0].track == 4);
    CHECK(read[0].score > 0.89 && read[0].score < 0.91);
    CHECK(read[0].last_seen == NOW + 6);
    CHECK(read[0].travelled == 130);
    CHECK(read[0].stationary);
    knvr_store_close(store);
    return true;
}

/*
 * An object that crosses two zones has been in both, and "what came up
 * the drive" has to find it after it has walked on to the porch.
 */
static bool test_zones_accumulate(void)
{
    knvr_store *store = NULL;
    knvr_object object;
    knvr_object read[2];
    knvr_query query;
    knvr_event events[4];
    size_t count = 0u;
    int64_t event = 0;

    CHECK(fresh(&store));
    CHECK(knvr_store_event_open(store, "drive", KNVR_TRIGGER_MOTION, NOW,
                                &event));
    (void)memset(&object, 0, sizeof(object));
    object.event = event;
    object.track = 1;
    (void)snprintf(object.camera, sizeof(object.camera), "drive");
    (void)snprintf(object.label, sizeof(object.label), "person");
    object.first_seen = NOW;
    object.last_seen = NOW;
    (void)snprintf(object.zone, sizeof(object.zone), "driveway");
    CHECK(knvr_store_put_object(store, &object));
    /* The same zone again must not be listed twice. */
    CHECK(knvr_store_put_object(store, &object));
    (void)snprintf(object.zone, sizeof(object.zone), "porch");
    CHECK(knvr_store_put_object(store, &object));
    /* And leaving every zone must not erase where it has been. */
    object.zone[0] = '\0';
    CHECK(knvr_store_put_object(store, &object));

    CHECK(knvr_store_objects(store, event, read, 2u, &count));
    CHECK(count == 1u);
    CHECK(strcmp(read[0].zone, "driveway,porch") == 0);

    knvr_query_init(&query);
    (void)snprintf(query.zone, sizeof(query.zone), "porch");
    CHECK(knvr_store_events(store, &query, events, 4u, &count));
    CHECK(count == 1u);
    (void)snprintf(query.zone, sizeof(query.zone), "driveway");
    CHECK(knvr_store_events(store, &query, events, 4u, &count));
    CHECK(count == 1u);
    /* A name that is only a substring of one it has been in is not a
     * match. */
    (void)snprintf(query.zone, sizeof(query.zone), "drive");
    CHECK(knvr_store_events(store, &query, events, 4u, &count));
    CHECK(count == 0u);
    (void)snprintf(query.zone, sizeof(query.zone), "lawn");
    CHECK(knvr_store_events(store, &query, events, 4u, &count));
    CHECK(count == 0u);
    knvr_store_close(store);
    return true;
}

static bool test_detections_carry_their_track(void)
{
    knvr_store *store = NULL;
    knvr_detection detection;
    knvr_detection read[4];
    size_t count = 0u;
    int64_t event = 0;

    CHECK(fresh(&store));
    CHECK(knvr_store_event_open(store, "drive", KNVR_TRIGGER_MOTION, NOW,
                                &event));
    (void)memset(&detection, 0, sizeof(detection));
    detection.event = event;
    detection.at = NOW;
    (void)snprintf(detection.label, sizeof(detection.label), "person");
    detection.score = 0.8;
    detection.track = 12;
    (void)snprintf(detection.zone, sizeof(detection.zone), "driveway");
    CHECK(knvr_store_add_detection(store, &detection));
    CHECK(knvr_store_detections(store, event, read, 4u, &count));
    CHECK(count == 1u);
    CHECK(read[0].track == 12);
    CHECK(strcmp(read[0].zone, "driveway") == 0);
    knvr_store_close(store);
    return true;
}

/*
 * A store written before tracking existed has to keep working, with its
 * events intact.  That case cannot be reached by creating a fresh store,
 * so the old schema is written out by hand here.
 */
static bool test_an_older_store_is_migrated(void)
{
    sqlite3 *db = NULL;
    knvr_store *store = NULL;
    knvr_detection read[4];
    knvr_event events[4];
    size_t count = 0u;

    (void)remove(STORE);
    CHECK(sqlite3_open(STORE, &db) == SQLITE_OK);
    CHECK(sqlite3_exec(db,
                       "CREATE TABLE event ("
                       "  id INTEGER PRIMARY KEY, camera TEXT NOT NULL,"
                       "  started INTEGER NOT NULL,"
                       "  ended INTEGER NOT NULL DEFAULT 0,"
                       "  trigger INTEGER NOT NULL DEFAULT 0,"
                       "  motion_frames INTEGER NOT NULL DEFAULT 0,"
                       "  best_score REAL NOT NULL DEFAULT 0,"
                       "  best_label TEXT NOT NULL DEFAULT '');"
                       "CREATE TABLE detection ("
                       "  event INTEGER NOT NULL, at INTEGER NOT NULL,"
                       "  label TEXT NOT NULL, score REAL NOT NULL,"
                       "  x INTEGER NOT NULL, y INTEGER NOT NULL,"
                       "  w INTEGER NOT NULL, h INTEGER NOT NULL);"
                       "CREATE TABLE media ("
                       "  event INTEGER NOT NULL, kind TEXT NOT NULL,"
                       "  path TEXT NOT NULL,"
                       "  bytes INTEGER NOT NULL DEFAULT 0);"
                       "INSERT INTO event (id, camera, started, ended) "
                       "VALUES (7, 'garage', 1759000000, 1759000030);"
                       "INSERT INTO detection VALUES "
                       "(7, 1759000010, 'person', 0.75, 1, 2, 3, 4);",
                       NULL, NULL, NULL) == SQLITE_OK);
    CHECK(sqlite3_close(db) == SQLITE_OK);

    CHECK(knvr_store_open(&store, STORE));
    CHECK(knvr_store_events(store, NULL, events, 4u, &count));
    CHECK(count == 1u);
    CHECK(events[0].id == 7);
    CHECK(knvr_store_detections(store, 7, read, 4u, &count));
    CHECK(count == 1u);
    CHECK(strcmp(read[0].label, "person") == 0);
    /* The columns that did not exist read as their defaults. */
    CHECK(read[0].track == 0);
    CHECK(read[0].zone[0] == '\0');
    /* And the new table is there to be written to. */
    {
        knvr_object object;

        (void)memset(&object, 0, sizeof(object));
        object.event = 7;
        object.track = 1;
        (void)snprintf(object.camera, sizeof(object.camera), "garage");
        (void)snprintf(object.label, sizeof(object.label), "person");
        object.first_seen = 1759000010;
        object.last_seen = 1759000012;
        CHECK(knvr_store_put_object(store, &object));
    }
    knvr_store_close(store);
    return true;
}

typedef bool (*test_function)(void);

typedef struct test_case {
    const char *name;
    test_function function;
} test_case;

int
main(void)
{
    static const test_case tests[] = {
        {"an event opens once", test_an_event_opens_once},
        {"stale events are closed", test_stale_events_are_closed},
        {"the best detection rides on the event",
         test_best_detection_rides_on_the_event},
        {"queries filter", test_queries_filter},
        {"age retention deletes the files",
         test_age_retention_deletes_the_files},
        {"the size cap stops at the limit",
         test_size_cap_stops_at_the_limit},
        {"rejections", test_rejections},
        {"objects are one row per thing",
         test_objects_are_one_row_per_thing},
        {"zones accumulate", test_zones_accumulate},
        {"detections carry their track",
         test_detections_carry_their_track},
        {"an older store is migrated", test_an_older_store_is_migrated}
    };
    size_t passed = 0u;

    for (size_t index = 0u; index < sizeof(tests) / sizeof(tests[0]); ++index) {
        const bool ok = tests[index].function();

        (void)printf("%s %s\n", ok ? "ok" : "not ok", tests[index].name);
        if (!ok) {
            return 1;
        }
        ++passed;
    }
    (void)remove(STORE);
    (void)printf("%zu tests passed\n", passed);
    return 0;
}
