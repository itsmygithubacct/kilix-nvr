/*
 * The per-camera capability store.
 *
 * sqlite rather than a config file of our own, for one reason that
 * outweighs the rest: the event store is already sqlite, and a system with
 * two persistence mechanisms has two things to get right on every upgrade.
 * The schema is small enough that this costs nothing.
 *
 * Every capability defaults off, and the defaults live in the schema so a
 * row inserted by any route - this code, a migration, somebody with the
 * sqlite shell - lands in the same safe state.
 */

#include "knvr_config.h"
#include "knvr_paths.h"
#include "knvr_sqlite.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#define ERROR_MAX 160

struct knvr_config {
    sqlite3 *db;
    char error[ERROR_MAX];
};

static bool fail(knvr_config *config, const char *reason)
{
    if (config != NULL) {
        (void)snprintf(config->error, sizeof(config->error), "%s", reason);
    }
    return false;
}

static bool fail_sqlite(knvr_config *config, const char *what)
{
    if (config != NULL) {
        (void)snprintf(config->error, sizeof(config->error), "%s: %s", what,
                       sqlite3_errmsg(config->db));
    }
    return false;
}

const char *knvr_config_error(const knvr_config *config)
{
    if (config == NULL || config->error[0] == '\0') {
        return NULL;
    }
    return config->error;
}

/*
 * The defaults are in the DDL, not in the insert.  A row created by a
 * migration or by hand then lands in the same off-by-default state as one
 * created here, which is the property that makes "a new camera does
 * nothing" true rather than merely usual.
 */
static const char SCHEMA[] =
    "PRAGMA journal_mode=WAL;"
    "CREATE TABLE IF NOT EXISTS camera ("
    "  name TEXT PRIMARY KEY NOT NULL,"
    "  record INTEGER NOT NULL DEFAULT 0,"
    "  detect INTEGER NOT NULL DEFAULT 0,"
    "  motion INTEGER NOT NULL DEFAULT 0,"
    "  audio INTEGER NOT NULL DEFAULT 0,"
    "  sound_events INTEGER NOT NULL DEFAULT 0,"
    "  retain_days INTEGER NOT NULL DEFAULT 0,"
    "  mask TEXT NOT NULL DEFAULT '',"
    "  zones TEXT NOT NULL DEFAULT ''"
    ");";

/* Added after the first release, so it has to reach databases that
 * already describe five cameras. */
static bool migrate(sqlite3 *db)
{
    return knvr_sqlite_add_column(db, "camera", "zones",
                                  "TEXT NOT NULL DEFAULT ''");
}

bool knvr_config_open(knvr_config **out, const char *path)
{
    knvr_config *config;
    char resolved[KNVR_PATH_MAX];
    char *message = NULL;

    if (out == NULL) {
        return false;
    }
    *out = NULL;
    config = calloc(1u, sizeof(*config));
    if (config == NULL) {
        return false;
    }
    if (path == NULL) {
        if (!knvr_paths_state_file(resolved, sizeof(resolved),
                                   "cameras.db")) {
            free(config);
            return false;
        }
        path = resolved;
    }
    if (sqlite3_open(path, &config->db) != SQLITE_OK) {
        (void)fail_sqlite(config, "cannot open the policy store");
        sqlite3_close(config->db);
        free(config);
        return false;
    }
    if (sqlite3_exec(config->db, SCHEMA, NULL, NULL, &message) != SQLITE_OK) {
        (void)snprintf(config->error, sizeof(config->error),
                       "cannot prepare the policy store: %s",
                       message != NULL ? message : "unknown");
        sqlite3_free(message);
        sqlite3_close(config->db);
        free(config);
        return false;
    }
    if (!migrate(config->db)) {
        (void)snprintf(config->error, sizeof(config->error),
                       "cannot bring the policy store up to date");
        sqlite3_close(config->db);
        free(config);
        return false;
    }
    *out = config;
    return true;
}

void knvr_config_close(knvr_config *config)
{
    if (config == NULL) {
        return;
    }
    sqlite3_close(config->db);
    free(config);
}

/* ------------------------------- cameras -------------------------------- */

static bool name_is_safe(const char *name)
{
    size_t length;

    if (name == NULL) {
        return false;
    }
    length = strlen(name);
    if (length == 0u || length >= KNVR_NAME_MAX) {
        return false;
    }
    for (size_t i = 0u; i < length; i++) {
        const char c = name[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';

        if (!ok) {
            /* Names become path components for masks and media, so the
             * set is restricted here rather than escaped everywhere. */
            return false;
        }
    }
    return true;
}

bool knvr_config_add(knvr_config *config, const char *name)
{
    sqlite3_stmt *statement = NULL;
    int status;

    if (config == NULL) {
        return false;
    }
    if (!name_is_safe(name)) {
        return fail(config,
                    "a camera name is 1-63 characters of letters, digits, "
                    "'-' or '_'");
    }
    if (sqlite3_prepare_v2(config->db,
                           "INSERT INTO camera (name) VALUES (?1);", -1,
                           &statement, NULL) != SQLITE_OK) {
        return fail_sqlite(config, "cannot add");
    }
    (void)sqlite3_bind_text(statement, 1, name, -1, SQLITE_TRANSIENT);
    status = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (status == SQLITE_CONSTRAINT) {
        return fail(config, "that camera already has a policy; use `set`");
    }
    if (status != SQLITE_DONE) {
        return fail_sqlite(config, "cannot add");
    }
    return true;
}

bool knvr_config_remove(knvr_config *config, const char *name)
{
    sqlite3_stmt *statement = NULL;
    int status;

    if (config == NULL || !name_is_safe(name)) {
        return false;
    }
    if (sqlite3_prepare_v2(config->db, "DELETE FROM camera WHERE name = ?1;",
                           -1, &statement, NULL) != SQLITE_OK) {
        return fail_sqlite(config, "cannot remove");
    }
    (void)sqlite3_bind_text(statement, 1, name, -1, SQLITE_TRANSIENT);
    status = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (status != SQLITE_DONE) {
        return fail_sqlite(config, "cannot remove");
    }
    return sqlite3_changes(config->db) > 0;
}

static void read_row(sqlite3_stmt *statement, knvr_camera *out)
{
    const unsigned char *name = sqlite3_column_text(statement, 0);
    const unsigned char *mask = sqlite3_column_text(statement, 7);
    const unsigned char *zones = sqlite3_column_text(statement, 8);

    (void)memset(out, 0, sizeof(*out));
    (void)snprintf(out->name, sizeof(out->name), "%s",
                   name != NULL ? (const char *)name : "");
    out->record = (knvr_record_mode)sqlite3_column_int(statement, 1);
    out->detect = (knvr_detect_policy)sqlite3_column_int(statement, 2);
    out->motion = sqlite3_column_int(statement, 3) != 0;
    out->audio = sqlite3_column_int(statement, 4) != 0;
    out->sound_events = sqlite3_column_int(statement, 5) != 0;
    out->retain_days = sqlite3_column_int(statement, 6);
    (void)snprintf(out->mask, sizeof(out->mask), "%s",
                   mask != NULL ? (const char *)mask : "");
    (void)snprintf(out->zones, sizeof(out->zones), "%s",
                   zones != NULL ? (const char *)zones : "");
}

static const char SELECT_COLUMNS[] =
    "SELECT name, record, detect, motion, audio, sound_events, retain_days, "
    "mask, zones FROM camera";

bool knvr_config_get(
    const knvr_config *config, const char *name, knvr_camera *out)
{
    knvr_config *mutable_config = (knvr_config *)config;
    sqlite3_stmt *statement = NULL;
    char query[sizeof(SELECT_COLUMNS) + 32];
    bool found = false;

    if (config == NULL || out == NULL || !name_is_safe(name)) {
        return false;
    }
    (void)snprintf(query, sizeof(query), "%s WHERE name = ?1;",
                   SELECT_COLUMNS);
    if (sqlite3_prepare_v2(config->db, query, -1, &statement, NULL) !=
        SQLITE_OK) {
        return fail_sqlite(mutable_config, "cannot read");
    }
    (void)sqlite3_bind_text(statement, 1, name, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(statement) == SQLITE_ROW) {
        read_row(statement, out);
        found = true;
    }
    sqlite3_finalize(statement);
    return found;
}

bool knvr_config_put(knvr_config *config, const knvr_camera *camera)
{
    sqlite3_stmt *statement = NULL;
    int status;

    if (config == NULL || camera == NULL || !name_is_safe(camera->name)) {
        return false;
    }
    if (sqlite3_prepare_v2(
            config->db,
            "UPDATE camera SET record = ?2, detect = ?3, motion = ?4, "
            "audio = ?5, sound_events = ?6, retain_days = ?7, mask = ?8, "
            "zones = ?9 WHERE name = ?1;",
            -1, &statement, NULL) != SQLITE_OK) {
        return fail_sqlite(config, "cannot write");
    }
    (void)sqlite3_bind_text(statement, 1, camera->name, -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_int(statement, 2, (int)camera->record);
    (void)sqlite3_bind_int(statement, 3, (int)camera->detect);
    (void)sqlite3_bind_int(statement, 4, camera->motion ? 1 : 0);
    (void)sqlite3_bind_int(statement, 5, camera->audio ? 1 : 0);
    (void)sqlite3_bind_int(statement, 6, camera->sound_events ? 1 : 0);
    (void)sqlite3_bind_int(statement, 7, camera->retain_days);
    (void)sqlite3_bind_text(statement, 8, camera->mask, -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(statement, 9, camera->zones, -1,
                            SQLITE_TRANSIENT);
    status = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (status != SQLITE_DONE) {
        return fail_sqlite(config, "cannot write");
    }
    if (sqlite3_changes(config->db) == 0) {
        return fail(config, "no such camera");
    }
    return true;
}

bool knvr_config_list(
    const knvr_config *config, knvr_camera *out, size_t capacity,
    size_t *count)
{
    knvr_config *mutable_config = (knvr_config *)config;
    sqlite3_stmt *statement = NULL;
    char query[sizeof(SELECT_COLUMNS) + 32];
    size_t total = 0u;

    if (count != NULL) {
        *count = 0u;
    }
    if (config == NULL) {
        return false;
    }
    (void)snprintf(query, sizeof(query), "%s ORDER BY name;", SELECT_COLUMNS);
    if (sqlite3_prepare_v2(config->db, query, -1, &statement, NULL) !=
        SQLITE_OK) {
        return fail_sqlite(mutable_config, "cannot list");
    }
    while (sqlite3_step(statement) == SQLITE_ROW) {
        if (out != NULL && total < capacity) {
            read_row(statement, &out[total]);
        }
        total++;
    }
    sqlite3_finalize(statement);
    if (count != NULL) {
        *count = total;
    }
    return true;
}

/* ------------------------------- settings ------------------------------- */

const char *knvr_record_mode_name(knvr_record_mode mode)
{
    switch (mode) {
    case KNVR_RECORD_OFF:        return "off";
    case KNVR_RECORD_STILLS:     return "stills";
    case KNVR_RECORD_CLIPS:      return "clips";
    case KNVR_RECORD_CONTINUOUS: return "continuous";
    default:                     return "?";
    }
}

const char *knvr_detect_policy_name(knvr_detect_policy policy)
{
    switch (policy) {
    case KNVR_DETECT_OFF:     return "off";
    case KNVR_DETECT_ALWAYS:  return "always";
    case KNVR_DETECT_ON_VIEW: return "on-view";
    default:                  return "?";
    }
}

static bool parse_bool(const char *text, bool *out)
{
    static const char *const yes[] = {"on", "yes", "true", "1"};
    static const char *const no[] = {"off", "no", "false", "0"};

    for (size_t i = 0u; i < sizeof(yes) / sizeof(yes[0]); i++) {
        if (strcmp(text, yes[i]) == 0) {
            *out = true;
            return true;
        }
        if (strcmp(text, no[i]) == 0) {
            *out = false;
            return true;
        }
    }
    return false;
}

bool knvr_camera_set(
    knvr_camera *camera, const char *assignment, const char **reason)
{
    const char *equals;
    char key[32];
    const char *value;
    size_t key_length;

    if (reason != NULL) {
        *reason = NULL;
    }
    if (camera == NULL || assignment == NULL) {
        return false;
    }
    equals = strchr(assignment, '=');
    if (equals == NULL || equals == assignment) {
        if (reason != NULL) {
            *reason = "settings look like key=value";
        }
        return false;
    }
    key_length = (size_t)(equals - assignment);
    if (key_length >= sizeof(key)) {
        if (reason != NULL) {
            *reason = "unknown setting";
        }
        return false;
    }
    (void)memcpy(key, assignment, key_length);
    key[key_length] = '\0';
    value = equals + 1;

    if (strcmp(key, "record") == 0) {
        for (int mode = KNVR_RECORD_OFF; mode <= KNVR_RECORD_CONTINUOUS;
             mode++) {
            if (strcmp(value, knvr_record_mode_name(
                                  (knvr_record_mode)mode)) != 0) {
                continue;
            }
            /*
             * `clips` is a name with nothing behind it.
             *
             * It is still spelled by knvr_record_mode_name(), because a
             * store written before this check can hold it and a camera
             * has to be describable.  What it must not be is settable:
             * accepting it meant a camera reporting a recording mode it
             * did not have, which behaved exactly like `stills` while
             * claiming otherwise.  Refusing is the honest state until
             * event clips are actually cut.
             */
            if (mode == KNVR_RECORD_CLIPS) {
                if (reason != NULL) {
                    *reason = "clips is not implemented; use continuous and "
                              "cut events with `kilix-nvr clip`";
                }
                return false;
            }
            camera->record = (knvr_record_mode)mode;
            return true;
        }
        if (reason != NULL) {
            *reason = "record is off, stills or continuous";
        }
        return false;
    }
    if (strcmp(key, "detect") == 0) {
        for (int policy = KNVR_DETECT_OFF; policy <= KNVR_DETECT_ON_VIEW;
             policy++) {
            if (strcmp(value, knvr_detect_policy_name(
                                  (knvr_detect_policy)policy)) == 0) {
                camera->detect = (knvr_detect_policy)policy;
                return true;
            }
        }
        if (reason != NULL) {
            *reason = "detect is off, always or on-view";
        }
        return false;
    }
    if (strcmp(key, "motion") == 0) {
        if (parse_bool(value, &camera->motion)) {
            return true;
        }
        if (reason != NULL) {
            *reason = "motion is on or off";
        }
        return false;
    }
    if (strcmp(key, "audio") == 0) {
        if (parse_bool(value, &camera->audio)) {
            return true;
        }
        if (reason != NULL) {
            *reason = "audio is on or off";
        }
        return false;
    }
    if (strcmp(key, "sound_events") == 0) {
        if (parse_bool(value, &camera->sound_events)) {
            return true;
        }
        if (reason != NULL) {
            *reason = "sound_events is on or off";
        }
        return false;
    }
    if (strcmp(key, "retain_days") == 0) {
        char *end = NULL;
        long days = strtol(value, &end, 10);

        /* Refused rather than clamped: a retention silently reduced to
         * something the operator did not ask for is a promise about how
         * long footage lasts that nobody made. */
        if (end == value || *end != '\0' || days < 0 || days > 3650) {
            if (reason != NULL) {
                *reason = "retain_days is 0 (no limit) to 3650";
            }
            return false;
        }
        camera->retain_days = (int)days;
        return true;
    }
    if (strcmp(key, "mask") == 0) {
        if (strlen(value) >= sizeof(camera->mask)) {
            if (reason != NULL) {
                *reason = "that mask path is too long";
            }
            return false;
        }
        (void)snprintf(camera->mask, sizeof(camera->mask), "%s", value);
        return true;
    }
    if (strcmp(key, "zones") == 0) {
        if (strlen(value) >= sizeof(camera->zones)) {
            if (reason != NULL) {
                *reason = "that zone map path is too long";
            }
            return false;
        }
        (void)snprintf(camera->zones, sizeof(camera->zones), "%s", value);
        return true;
    }
    if (reason != NULL) {
        *reason = "unknown setting";
    }
    return false;
}

bool knvr_camera_is_blind(const knvr_camera *camera, bool viewer_attached)
{
    if (camera == NULL) {
        return false;
    }
    return camera->detect == KNVR_DETECT_ON_VIEW && !viewer_attached;
}
