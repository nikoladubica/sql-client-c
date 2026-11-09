#ifndef SQLC_EXPORT_DIALOG_H
#define SQLC_EXPORT_DIALOG_H

#include <stddef.h>

#include "export.h"

/*
 * Native, app-modal export flow for a table: an options dialog (format and
 * per-format options), then a save panel defaulting to "<table>.<ext>".
 * The last choices are remembered for the rest of the session.
 *
 * Returns 1 with *fmt, *opts and *path (malloc'd) filled in, or 0 if the
 * user cancelled.
 */
int sqlc_export_dialog(const char *table, int is_view,
                       export_format *fmt, export_opts *opts, char **path);

/*
 * The same for a whole database (export_database, nselected 0) or nselected
 * of its tables and views (export_tables): SQL only, with the structure /
 * DROP / data options, then a save panel defaulting to "<db>.sql" or
 * "<db>-<n>-tables.sql" (export_dump_basename). Remembered separately from
 * the table choices.
 *
 * Returns 1 with *opts and *path (malloc'd) filled in, or 0 if cancelled.
 */
int sqlc_export_database_dialog(const char *db, size_t nselected, export_opts *opts,
                                char **path);

#endif
