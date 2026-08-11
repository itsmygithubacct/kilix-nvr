#include "knvr_sqlite.h"

#include <stdio.h>

bool knvr_sqlite_add_column(
    sqlite3 *db, const char *table, const char *column,
    const char *declaration)
{
    sqlite3_stmt *statement = NULL;
    char sql[256];
    bool present;

    if (db == NULL || table == NULL || column == NULL ||
        declaration == NULL) {
        return false;
    }
    /* pragma_table_info as a table-valued function, so the check is a
     * bound query rather than string-built sql against a name. */
    if (sqlite3_prepare_v2(db,
                           "SELECT 1 FROM pragma_table_info(?1) "
                           "WHERE name = ?2;",
                           -1, &statement, NULL) != SQLITE_OK) {
        return false;
    }
    (void)sqlite3_bind_text(statement, 1, table, -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(statement, 2, column, -1, SQLITE_TRANSIENT);
    present = sqlite3_step(statement) == SQLITE_ROW;
    sqlite3_finalize(statement);
    if (present) {
        return true;
    }
    /* The table and column names here are literals from the callers, not
     * anything a user typed; ALTER TABLE takes no parameters. */
    if (snprintf(sql, sizeof(sql), "ALTER TABLE %s ADD COLUMN %s %s;", table,
                 column, declaration) < 0) {
        return false;
    }
    return sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK;
}
