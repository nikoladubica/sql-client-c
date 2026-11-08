#ifndef SQLC_EXPORT_DB_H
#define SQLC_EXPORT_DB_H

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
 * left out of the INSERTs. A view gets its CREATE VIEW only.
 *
 * Returns 0, or -1 with *err set to a malloc'd message. *rows is set to the
 * number of rows written either way (may be NULL).
 */
int export_table(MYSQL *conn, const char *table, int is_view, export_format fmt,
                 const export_opts *opts, const char *path,
                 export_progress_fn progress, void *data,
                 unsigned long long *rows, char **err);

#endif
