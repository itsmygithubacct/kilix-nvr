/*
 * The event store.
 *
 * One open event per camera at a time, which is what lets the caller say
 * "something moved" on every frame without tracking whether it has said so
 * already.  Retention unlinks the media as well as the rows, because a
 * retention policy that leaves the footage on disk is not one.
 */

#include "knvr_store.h"
#include "knvr_paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sqlite3.h>

#define ERROR_MAX 200
#define DEFAULT_LIMIT 200

struct knvr_store {
    sqlite3 *db;
    char error[ERROR_MAX];
};

static bool fail_sqlite(knvr_store *store, const char *what)
{
    if (store != NULL) {
        (void)snprintf(store->error, sizeof(store->error), "%s: %s", what,
                       sqlite3_errmsg(store->db));
    }
    return false;
}

const char *knvr_store_error(const knvr_store *store)
{
    if (store == NULL || store->error[0] == '\0') {
        return NULL;
    }
    return store->error;
}

/*
 * `ended = 0` marks an open event, and the partial index makes "is this
 * camera already recording an event" a lookup rather than a scan - the
 * question every frame asks.
 */
static const char SCHEMA[] =
    "PRAGMA journal_mode=WAL;"
    "PRAGMA foreign_keys=ON;"
    "CREATE TABLE IF NOT EXISTS event ("
    "  id INTEGER PRIMARY KEY,"
    "  camera TEXT NOT NULL,"
    "  started INTEGER NOT NULL,"
    "  ended INTEGER NOT NULL DEFAULT 0,"
    "  trigger INTEGER NOT NULL DEFAULT 0,"
    "  motion_frames INTEGER NOT NULL DEFAULT 0,"
    "  best_score REAL NOT NULL DEFAULT 0,"
    "  best_label TEXT NOT NULL DEFAULT ''"
    ");"
    "CREATE INDEX IF NOT EXISTS event_by_time ON event(started DESC);"
    "CREATE INDEX IF NOT EXISTS event_open ON event(camera) WHERE ended = 0;"
    "CREATE TABLE IF NOT EXISTS detection ("
    "  event INTEGER NOT NULL REFERENCES event(id) ON DELETE CASCADE,"
    "  at INTEGER NOT NULL,"
    "  label TEXT NOT NULL,"
    "  score REAL NOT NULL,"
    "  x INTEGER NOT NULL, y INTEGER NOT NULL,"
    "  w INTEGER NOT NULL, h INTEGER NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS detection_by_event ON detection(event);"
    "CREATE TABLE IF NOT EXISTS media ("
    "  event INTEGER NOT NULL REFERENCES event(id) ON DELETE CASCADE,"
    "  kind TEXT NOT NULL,"
    "  path TEXT NOT NULL,"
    "  bytes INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE INDEX IF NOT EXISTS media_by_event ON media(event);";

bool knvr_store_open(knvr_store **out, const char *path)
{
    knvr_store *store;
    char resolved[KNVR_PATH_MAX];
    char *message = NULL;

    if (out == NULL) {
        return false;
    }
    *out = NULL;
    store = calloc(1u, sizeof(*store));
    if (store == NULL) {
        return false;
    }
    if (path == NULL) {
        if (!knvr_paths_state_file(resolved, sizeof(resolved), "events.db")) {
            free(store);
            return false;
        }
        path = resolved;
    }
    if (sqlite3_open(path, &store->db) != SQLITE_OK) {
        (void)fail_sqlite(store, "cannot open the event store");
        sqlite3_close(store->db);
        free(store);
        return false;
    }
    if (sqlite3_exec(store->db, SCHEMA, NULL, NULL, &message) != SQLITE_OK) {
        (void)snprintf(store->error, sizeof(store->error),
                       "cannot prepare the event store: %s",
                       message != NULL ? message : "unknown");
        sqlite3_free(message);
        sqlite3_close(store->db);
        free(store);
        return false;
    }
    *out = store;
    return true;
}

void knvr_store_close(knvr_store *store)
{
    if (store == NULL) {
        return;
    }
    sqlite3_close(store->db);
    free(store);
}

/* -------------------------------- events -------------------------------- */

static bool find_open(knvr_store *store, const char *camera, int64_t *id)
{
    sqlite3_stmt *statement = NULL;
    bool found = false;

    if (sqlite3_prepare_v2(
            store->db,
            "SELECT id FROM event WHERE camera = ?1 AND ended = 0 "
            "ORDER BY started DESC LIMIT 1;",
            -1, &statement, NULL) != SQLITE_OK) {
        return false;
    }
    (void)sqlite3_bind_text(statement, 1, camera, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(statement) == SQLITE_ROW) {
        *id = sqlite3_column_int64(statement, 0);
        found = true;
    }
    sqlite3_finalize(statement);
    return found;
}

bool knvr_store_event_open(
    knvr_store *store, const char *camera, knvr_trigger trigger,
    int64_t at, int64_t *event_id)
{
    sqlite3_stmt *statement = NULL;
    int64_t existing = 0;

    if (store == NULL || camera == NULL || camera[0] == '\0') {
        return false;
    }
    /* Already open: hand back the same event rather than starting a
     * second one, so a caller can report movement every frame. */
    if (find_open(store, camera, &existing)) {
        if (event_id != NULL) {
            *event_id = existing;
        }
        return true;
    }
    if (sqlite3_prepare_v2(
            store->db,
            "INSERT INTO event (camera, started, trigger, motion_frames) "
            "VALUES (?1, ?2, ?3, 1);",
            -1, &statement, NULL) != SQLITE_OK) {
        return fail_sqlite(store, "cannot open an event");
    }
    (void)sqlite3_bind_text(statement, 1, camera, -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_int64(statement, 2, at);
    (void)sqlite3_bind_int(statement, 3, (int)trigger);
    if (sqlite3_step(statement) != SQLITE_DONE) {
        sqlite3_finalize(statement);
        return fail_sqlite(store, "cannot open an event");
    }
    sqlite3_finalize(statement);
    if (event_id != NULL) {
        *event_id = sqlite3_last_insert_rowid(store->db);
    }
    return true;
}

bool knvr_store_event_touch(knvr_store *store, int64_t event_id)
{
    sqlite3_stmt *statement = NULL;
    bool ok;

    if (store == NULL) {
        return false;
    }
    if (sqlite3_prepare_v2(
            store->db,
            "UPDATE event SET motion_frames = motion_frames + 1 "
            "WHERE id = ?1 AND ended = 0;",
            -1, &statement, NULL) != SQLITE_OK) {
        return fail_sqlite(store, "cannot update an event");
    }
    (void)sqlite3_bind_int64(statement, 1, event_id);
    ok = sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    return ok;
}

bool knvr_store_event_close(knvr_store *store, int64_t event_id, int64_t at)
{
    sqlite3_stmt *statement = NULL;
    bool ok;

    if (store == NULL) {
        return false;
    }
    /* Only an open event is closed, so a second close is a no-op rather
     * than a rewrite of the end time: the quiet timer and a shutdown can
     * both reach for the same event. */
    if (sqlite3_prepare_v2(
            store->db, "UPDATE event SET ended = ?2 WHERE id = ?1 AND ended = 0;",
            -1, &statement, NULL) != SQLITE_OK) {
        return fail_sqlite(store, "cannot close an event");
    }
    (void)sqlite3_bind_int64(statement, 1, event_id);
    (void)sqlite3_bind_int64(statement, 2, at);
    ok = sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    return ok;
}

bool knvr_store_close_stale(knvr_store *store, const char *camera, int64_t at)
{
    sqlite3_stmt *statement = NULL;
    bool ok;

    if (store == NULL) {
        return false;
    }
    if (sqlite3_prepare_v2(
            store->db,
            "UPDATE event SET ended = ?2 WHERE ended = 0 AND "
            "(?1 = '' OR camera = ?1);",
            -1, &statement, NULL) != SQLITE_OK) {
        return fail_sqlite(store, "cannot close stale events");
    }
    (void)sqlite3_bind_text(statement, 1, camera != NULL ? camera : "", -1,
                            SQLITE_TRANSIENT);
    (void)sqlite3_bind_int64(statement, 2, at);
    ok = sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    return ok;
}

bool knvr_store_add_detection(
    knvr_store *store, const knvr_detection *detection)
{
    sqlite3_stmt *statement = NULL;
    bool ok;

    if (store == NULL || detection == NULL) {
        return false;
    }
    if (sqlite3_prepare_v2(
            store->db,
            "INSERT INTO detection (event, at, label, score, x, y, w, h) "
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8);",
            -1, &statement, NULL) != SQLITE_OK) {
        return fail_sqlite(store, "cannot record a detection");
    }
    (void)sqlite3_bind_int64(statement, 1, detection->event);
    (void)sqlite3_bind_int64(statement, 2, detection->at);
    (void)sqlite3_bind_text(statement, 3, detection->label, -1,
                            SQLITE_TRANSIENT);
    (void)sqlite3_bind_double(statement, 4, detection->score);
    (void)sqlite3_bind_int(statement, 5, detection->x);
    (void)sqlite3_bind_int(statement, 6, detection->y);
    (void)sqlite3_bind_int(statement, 7, detection->w);
    (void)sqlite3_bind_int(statement, 8, detection->h);
    ok = sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    if (!ok) {
        return fail_sqlite(store, "cannot record a detection");
    }

    /* Carried on the event so the common query - "what was probably a
     * person" - reads one table.  Only ever raised. */
    if (sqlite3_prepare_v2(
            store->db,
            "UPDATE event SET best_score = ?2, best_label = ?3 "
            "WHERE id = ?1 AND best_score < ?2;",
            -1, &statement, NULL) != SQLITE_OK) {
        return fail_sqlite(store, "cannot record a detection");
    }
    (void)sqlite3_bind_int64(statement, 1, detection->event);
    (void)sqlite3_bind_double(statement, 2, detection->score);
    (void)sqlite3_bind_text(statement, 3, detection->label, -1,
                            SQLITE_TRANSIENT);
    ok = sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    return ok;
}

bool knvr_store_add_media(
    knvr_store *store, int64_t event_id, const char *kind, const char *path)
{
    sqlite3_stmt *statement = NULL;
    struct stat info;
    int64_t bytes = 0;
    bool ok;

    if (store == NULL || kind == NULL || path == NULL) {
        return false;
    }
    /* Size recorded now, while the file is certainly there: the size cap
     * has to know what it is freeing, and stat-ing at prune time would
     * miss anything already deleted from underneath us. */
    if (stat(path, &info) == 0) {
        bytes = (int64_t)info.st_size;
    }
    if (sqlite3_prepare_v2(
            store->db,
            "INSERT INTO media (event, kind, path, bytes) "
            "VALUES (?1, ?2, ?3, ?4);",
            -1, &statement, NULL) != SQLITE_OK) {
        return fail_sqlite(store, "cannot record media");
    }
    (void)sqlite3_bind_int64(statement, 1, event_id);
    (void)sqlite3_bind_text(statement, 2, kind, -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(statement, 3, path, -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_int64(statement, 4, bytes);
    ok = sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    return ok;
}

/* -------------------------------- queries ------------------------------- */

void knvr_query_init(knvr_query *query)
{
    if (query != NULL) {
        (void)memset(query, 0, sizeof(*query));
        query->limit = DEFAULT_LIMIT;
    }
}

static void read_event(sqlite3_stmt *statement, knvr_event *out)
{
    const unsigned char *camera = sqlite3_column_text(statement, 1);
    const unsigned char *label = sqlite3_column_text(statement, 7);

    (void)memset(out, 0, sizeof(*out));
    out->id = sqlite3_column_int64(statement, 0);
    (void)snprintf(out->camera, sizeof(out->camera), "%s",
                   camera != NULL ? (const char *)camera : "");
    out->started = sqlite3_column_int64(statement, 2);
    out->ended = sqlite3_column_int64(statement, 3);
    out->trigger = (knvr_trigger)sqlite3_column_int(statement, 4);
    out->motion_frames = sqlite3_column_int64(statement, 5);
    out->best_score = sqlite3_column_double(statement, 6);
    (void)snprintf(out->best_label, sizeof(out->best_label), "%s",
                   label != NULL ? (const char *)label : "");
}

bool knvr_store_events(
    const knvr_store *store, const knvr_query *query, knvr_event *out,
    size_t capacity, size_t *count)
{
    knvr_store *mutable_store = (knvr_store *)store;
    knvr_query defaults;
    sqlite3_stmt *statement = NULL;
    size_t total = 0u;

    if (count != NULL) {
        *count = 0u;
    }
    if (store == NULL) {
        return false;
    }
    if (query == NULL) {
        knvr_query_init(&defaults);
        query = &defaults;
    }
    if (sqlite3_prepare_v2(
            store->db,
            "SELECT id, camera, started, ended, trigger, motion_frames, "
            "best_score, best_label FROM event WHERE "
            "(?1 = 0 OR started >= ?1) AND (?2 = 0 OR started <= ?2) AND "
            "(?3 = '' OR camera = ?3) AND best_score >= ?4 "
            "ORDER BY started DESC LIMIT ?5;",
            -1, &statement, NULL) != SQLITE_OK) {
        return fail_sqlite(mutable_store, "cannot query events");
    }
    (void)sqlite3_bind_int64(statement, 1, query->since);
    (void)sqlite3_bind_int64(statement, 2, query->until);
    (void)sqlite3_bind_text(statement, 3, query->camera, -1,
                            SQLITE_TRANSIENT);
    (void)sqlite3_bind_double(statement, 4, query->min_score);
    (void)sqlite3_bind_int(statement, 5,
                           query->limit > 0 ? query->limit : DEFAULT_LIMIT);
    while (sqlite3_step(statement) == SQLITE_ROW) {
        if (out != NULL && total < capacity) {
            read_event(statement, &out[total]);
        }
        total++;
    }
    sqlite3_finalize(statement);
    if (count != NULL) {
        *count = total;
    }
    return true;
}

bool knvr_store_detections(
    const knvr_store *store, int64_t event_id, knvr_detection *out,
    size_t capacity, size_t *count)
{
    knvr_store *mutable_store = (knvr_store *)store;
    sqlite3_stmt *statement = NULL;
    size_t total = 0u;

    if (count != NULL) {
        *count = 0u;
    }
    if (store == NULL) {
        return false;
    }
    if (sqlite3_prepare_v2(
            store->db,
            "SELECT event, at, label, score, x, y, w, h FROM detection "
            "WHERE event = ?1 ORDER BY at;",
            -1, &statement, NULL) != SQLITE_OK) {
        return fail_sqlite(mutable_store, "cannot query detections");
    }
    (void)sqlite3_bind_int64(statement, 1, event_id);
    while (sqlite3_step(statement) == SQLITE_ROW) {
        if (out != NULL && total < capacity) {
            const unsigned char *label = sqlite3_column_text(statement, 2);

            (void)memset(&out[total], 0, sizeof(out[total]));
            out[total].event = sqlite3_column_int64(statement, 0);
            out[total].at = sqlite3_column_int64(statement, 1);
            (void)snprintf(out[total].label, sizeof(out[total].label), "%s",
                           label != NULL ? (const char *)label : "");
            out[total].score = sqlite3_column_double(statement, 3);
            out[total].x = sqlite3_column_int(statement, 4);
            out[total].y = sqlite3_column_int(statement, 5);
            out[total].w = sqlite3_column_int(statement, 6);
            out[total].h = sqlite3_column_int(statement, 7);
        }
        total++;
    }
    sqlite3_finalize(statement);
    if (count != NULL) {
        *count = total;
    }
    return true;
}

/* ------------------------------- retention ------------------------------ */

/*
 * Unlink one event's media and drop the row.  The files go first: a row
 * removed before its files leaves footage nothing points at, which no
 * later prune will ever find.
 */
static bool remove_event(
    knvr_store *store, int64_t event_id, bool dry_run,
    knvr_prune_result *result)
{
    sqlite3_stmt *statement = NULL;

    if (sqlite3_prepare_v2(store->db,
                           "SELECT path, bytes FROM media WHERE event = ?1;",
                           -1, &statement, NULL) != SQLITE_OK) {
        return fail_sqlite(store, "cannot read media");
    }
    (void)sqlite3_bind_int64(statement, 1, event_id);
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const unsigned char *path = sqlite3_column_text(statement, 0);
        const int64_t bytes = sqlite3_column_int64(statement, 1);

        if (path != NULL && !dry_run) {
            (void)unlink((const char *)path);
        }
        if (result != NULL) {
            result->media_removed++;
            result->bytes_freed += (uint64_t)(bytes > 0 ? bytes : 0);
        }
    }
    sqlite3_finalize(statement);

    if (!dry_run) {
        /* detection and media cascade from the foreign key. */
        if (sqlite3_prepare_v2(store->db, "DELETE FROM event WHERE id = ?1;",
                               -1, &statement, NULL) != SQLITE_OK) {
            return fail_sqlite(store, "cannot remove an event");
        }
        (void)sqlite3_bind_int64(statement, 1, event_id);
        if (sqlite3_step(statement) != SQLITE_DONE) {
            sqlite3_finalize(statement);
            return fail_sqlite(store, "cannot remove an event");
        }
        sqlite3_finalize(statement);
    }
    if (result != NULL) {
        result->events_removed++;
    }
    return true;
}

bool knvr_store_prune_age(
    knvr_store *store, const knvr_retention *rule, int64_t now,
    bool dry_run, knvr_prune_result *result)
{
    sqlite3_stmt *statement = NULL;
    int64_t cutoff;
    int64_t doomed[512];
    size_t count = 0u;

    if (result != NULL) {
        (void)memset(result, 0, sizeof(*result));
    }
    if (store == NULL || rule == NULL) {
        return false;
    }
    if (rule->days <= 0) {
        return true;   /* no age rule for this camera; not an error */
    }
    cutoff = now - (int64_t)rule->days * 86400;

    /* Collected before deleting: stepping a SELECT while DELETEing rows it
     * is walking is the kind of thing that works until it does not. */
    if (sqlite3_prepare_v2(
            store->db,
            "SELECT id FROM event WHERE started < ?1 AND "
            "(?2 = '' OR camera = ?2) ORDER BY started LIMIT ?3;",
            -1, &statement, NULL) != SQLITE_OK) {
        return fail_sqlite(store, "cannot select expired events");
    }
    (void)sqlite3_bind_int64(statement, 1, cutoff);
    (void)sqlite3_bind_text(statement, 2, rule->camera, -1,
                            SQLITE_TRANSIENT);
    (void)sqlite3_bind_int(statement, 3,
                           (int)(sizeof(doomed) / sizeof(doomed[0])));
    while (sqlite3_step(statement) == SQLITE_ROW &&
           count < sizeof(doomed) / sizeof(doomed[0])) {
        doomed[count++] = sqlite3_column_int64(statement, 0);
    }
    sqlite3_finalize(statement);

    for (size_t i = 0u; i < count; i++) {
        if (!remove_event(store, doomed[i], dry_run, result)) {
            return false;
        }
    }
    return true;
}

bool knvr_store_prune_size(
    knvr_store *store, uint64_t limit_bytes, bool dry_run,
    knvr_prune_result *result)
{
    sqlite3_stmt *statement = NULL;
    uint64_t total = 0u;

    if (result != NULL) {
        (void)memset(result, 0, sizeof(*result));
    }
    if (store == NULL) {
        return false;
    }
    if (sqlite3_prepare_v2(store->db, "SELECT COALESCE(SUM(bytes), 0) FROM media;",
                           -1, &statement, NULL) != SQLITE_OK) {
        return fail_sqlite(store, "cannot measure the store");
    }
    if (sqlite3_step(statement) == SQLITE_ROW) {
        total = (uint64_t)sqlite3_column_int64(statement, 0);
    }
    sqlite3_finalize(statement);
    if (total <= limit_bytes) {
        return true;
    }

    /* Oldest first, one event at a time, stopping the moment the store is
     * under the cap.  Deleting a batch and re-measuring would overshoot,
     * and the footage removed past the cap is not recoverable. */
    while (total > limit_bytes) {
        int64_t oldest = 0;
        uint64_t before;

        if (sqlite3_prepare_v2(
                store->db, "SELECT id FROM event ORDER BY started LIMIT 1;",
                -1, &statement, NULL) != SQLITE_OK) {
            return fail_sqlite(store, "cannot select the oldest event");
        }
        if (sqlite3_step(statement) != SQLITE_ROW) {
            sqlite3_finalize(statement);
            break;   /* nothing left to remove */
        }
        oldest = sqlite3_column_int64(statement, 0);
        sqlite3_finalize(statement);

        before = result != NULL ? result->bytes_freed : 0u;
        if (!remove_event(store, oldest, dry_run, result)) {
            return false;
        }
        if (result != NULL) {
            const uint64_t freed = result->bytes_freed - before;

            total = freed >= total ? 0u : total - freed;
        }
        if (dry_run) {
            /* A dry run cannot shrink anything, so without this it would
             * walk the whole store reporting every event as doomed. */
            if (result == NULL || result->bytes_freed == before) {
                break;
            }
        }
    }
    return true;
}
