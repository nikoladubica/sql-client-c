#ifndef SQLC_EXPORT_DB_H
#define SQLC_EXPORT_DB_H

#include <stddef.h>
#include <mysql.h>

#include "export.h"

/* Called every few thousand rows from the exporting thread. */
typedef void (*export_progress_fn)(unsigned long long rows, void *data);

/*
 * Export a table (or view) of the current database to path, streaming rows
 * with mysql_use_result. Blocks; run it on a worker thread.
 *
 * The file is written next to path under a temporary name and renamed over
 * it only when complete, so a failed export never leaves a partial file or
 * clobbers an existing one.
 *
 * SQL format: the session time zone is switched to UTC while reading (and
 * restored) so TIMESTAMP values re-import unchanged; generated columns are
 * left out of the INSERTs. A view gets its CREATE VIEW only. A standalone
 * file always: opts->sql_in_dump is ignored.
 *
 * Returns 0, or -1 with *err set to a malloc'd message. *rows is set to the
 * number of rows written either way (may be NULL).
 */
int export_table(MYSQL *conn, const char *table, int is_view, export_format fmt,
                 const export_opts *opts, const char *path,
                 export_progress_fn progress, void *data,
                 unsigned long long *rows, char **err);

/* Progress of export_database, from the exporting thread: at the start of
 * each table or view (rows = 0) and every few thousand rows of a table.
 * index counts from 1 up to count (tables + views). */
typedef void (*export_db_progress_fn)(const char *table, size_t index, size_t count,
                                      unsigned long long rows, void *data);

typedef struct {
    size_t tables;           /* tables written */
    size_t views;            /* views written (none without sql_structure) */
    size_t skipped;          /* sequences and other objects left out */
    unsigned long long rows; /* rows written, all tables */
} export_db_stats;

/*
 * Export the current database (SELECT DATABASE()) to one SQL file at path:
 * a header that saves and sets the session (utf8mb4, UTC, foreign key checks
 * off, so the tables load in any order), every base table by name with its
 * DROP/CREATE TABLE and INSERTs as export_table writes them (per opts:
 * sql_structure, sql_drop, sql_data), then the views (CREATE VIEW only, each
 * after the views it uses, see export_view_order), then the footer restoring
 * the session. Other objects (sequences, ...) are left out with a comment.
 * No CREATE DATABASE / USE: load it into any database with `mysql db < file`.
 *
 * Read-only on the server: SELECT and SHOW only, plus the session time zone
 * as in export_table (restored after each table). Tables are read one after
 * another, not in one transaction, so it is not a consistent snapshot of a
 * database that is being written to. Blocks; run it on a worker thread.
 * Written to a temporary file and renamed over path only when complete.
 *
 * Returns 0, or -1 with *err set to a malloc'd message (no database
 * selected, a server error naming the table, or a write error). *stats
 * (may be NULL) is filled in either way.
 */
int export_database(MYSQL *conn, const export_opts *opts, const char *path,
                    export_db_progress_fn progress, void *data,
                    export_db_stats *stats, char **err);

#endif
