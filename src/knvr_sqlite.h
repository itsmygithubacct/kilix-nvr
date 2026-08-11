#ifndef KNVR_SQLITE_H
#define KNVR_SQLITE_H

/*
 * The one thing both sqlite-backed stores need and neither should own.
 *
 * Two stores here keep schemas that grow: the policy store and the event
 * store.  Both are opened against databases that already exist and hold
 * data nobody wants to lose, so both need "add this column if it is not
 * there yet" - which sqlite has no syntax for, since ALTER TABLE ADD
 * COLUMN is an error when the column exists.
 */

#include <sqlite3.h>

#include <stdbool.h>

/*
 * True when the column is there afterwards, whether or not it was there
 * before.  `declaration` is the type and constraints, and must carry a
 * default: sqlite cannot add a NOT NULL column to a populated table
 * without one.
 */
bool knvr_sqlite_add_column(
    sqlite3 *db, const char *table, const char *column,
    const char *declaration);

#endif /* KNVR_SQLITE_H */
