#ifndef SQLC_EXPORT_DB_H
#define SQLC_EXPORT_DB_H

#include <stddef.h>
#include <mysql.h>

#include "export.h"

/*
 * Progress, from the exporting thread: once at the start of each table or
 * view (rows = 0) and every few thousand rows of a table. See
 * export_progress in export.h (p->table and p are only valid during the
 * call). Return 0 to go on, or nonzero to stop the export: it then ends at
 * that point with -2 ("Export cancelled"), its temporary file is removed
 * and the target file is not created or touched.
 */
typedef int (*export_progress_fn)(const export_progress *p, void *data);

/* What export_table / export_database / export_tables return when the
 * progress callback asked to stop. */
#define EXPORT_CANCELLED (-2)

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
 * With a progress callback, the table's row estimate is read from
 * information_schema first (not for a view), and progress is called once
 * before the first row (index 1, count 1).
 *
 * Returns 0, -1 with *err set to a malloc'd message, or EXPORT_CANCELLED
 * with *err set to "Export cancelled". *rows is set to the number of rows
 * written either way (may be NULL). The connection stays usable after a
 * cancel: the rest of the table's result is read and dropped first, which
 * can take a moment on a big table.
 */
int export_table(MYSQL *conn, const char *table, int is_view, export_format fmt,
                 const export_opts *opts, const char *path,
                 export_progress_fn progress, void *data,
                 unsigned long long *rows, char **err);

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
 * Progress: index counts from 1 up to count (tables + views written), with
 * each table's row estimate from information_schema (views: 0).
 *
 * Returns 0, or -1 with *err set to a malloc'd message (no database
 * selected, a server error naming the table, or a write error), or
 * EXPORT_CANCELLED as export_table. *stats (may be NULL) is filled in
 * either way.
 */
int export_database(MYSQL *conn, const export_opts *opts, const char *path,
                    export_progress_fn progress, void *data,
                    export_db_stats *stats, char **err);

/*
 * The same for some objects of the current database: the n tables and views
 * named in names (n >= 1), written exactly as export_database writes them -
 * tables in name order, then the selected views, each after the selected
 * views it uses (views it uses that are not selected are not added). Same
 * header (saying it is a selection, see export_sql_dump_header), footer and
 * options. A selected sequence or other object is left out with a comment.
 *
 * A name that is not in the database (dropped or renamed since the list was
 * loaded) fails the export before the file is created. Read-only on the
 * server, as export_database. Returns 0, -1 with *err set (malloc'd), or
 * EXPORT_CANCELLED.
 */
int export_tables(MYSQL *conn, const char *const *names, size_t n,
                  const export_opts *opts, const char *path,
                  export_progress_fn progress, void *data,
                  export_db_stats *stats, char **err);

#endif
