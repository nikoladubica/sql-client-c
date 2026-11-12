/* Windows version of export_dialog.h. Phase 1 of the Windows port: the
 * options dialog and save panel are not there yet, so exporting says so
 * and counts as cancelled (see README, "Windows"). */

#include <windows.h>

#include "export_dialog.h"
#include "win_util.h"

static void not_yet(void) {
    const char *labels[] = { "OK" };
    sqlc_w_task_dialog("Export isn't available on Windows yet",
                       "Exporting tables and databases comes in a later version of the Windows app.",
                       0, labels, 1, 100, 1);
}

int sqlc_export_dialog(const char *table, int is_view,
                       export_format *fmt, export_opts *opts, char **path) {
    (void)table; (void)is_view; (void)fmt; (void)opts;
    *path = NULL;
    not_yet();
    return 0;
}

int sqlc_export_database_dialog(const char *db, size_t nselected, export_opts *opts,
                                char **path) {
    (void)db; (void)nselected; (void)opts;
    *path = NULL;
    not_yet();
    return 0;
}
