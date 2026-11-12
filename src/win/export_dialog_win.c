/* Windows version of export_dialog.h: the export options as a dialog built
 * in memory (format, SQL and CSV options, the same rules as
 * export_dialog.m), then the standard Save As dialog. The last choices are
 * remembered for the session. */

#include <windows.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "export_dialog.h"
#include "dlg_win.h"
#include "win_util.h"

/* remembered for the session */
static int have_last;
static export_format last_fmt;
static export_opts last_opts;
static int have_last_db;
static export_opts last_db_opts;

enum {
    ID_TITLE = 200, ID_MESSAGE, ID_FORMAT_LABEL, ID_FORMAT,
    ID_SQL_HEADING, ID_STRUCTURE, ID_DROP, ID_DATA,
    ID_CSV_HEADING, ID_NULL_N, ID_BOM
};

typedef struct {
    int database;          /* SQL only: no format or CSV controls */
    int is_view;
    export_format fmt;
    export_opts opts;      /* in: starting values; out: the choices */
} export_state;

static int checked(HWND dlg, int id) { return IsDlgButtonChecked(dlg, id) == BST_CHECKED; }

/* Which options apply to the format, and whether Export… can be pressed. */
static void update(HWND dlg, export_state *st) {
    int f = st->database ? EXPORT_SQL : (int)SendDlgItemMessageW(dlg, ID_FORMAT, CB_GETCURSEL, 0, 0);
    int sql = f == EXPORT_SQL, csv = f == EXPORT_CSV;
    int structure = checked(dlg, ID_STRUCTURE);
    EnableWindow(GetDlgItem(dlg, ID_STRUCTURE), sql);
    EnableWindow(GetDlgItem(dlg, ID_DROP), sql && structure);
    EnableWindow(GetDlgItem(dlg, ID_DATA), sql && !st->is_view);
    if (!st->database) {
        EnableWindow(GetDlgItem(dlg, ID_NULL_N), csv);
        EnableWindow(GetDlgItem(dlg, ID_BOM), csv);
    }
    /* SQL needs at least one of structure / data */
    int data = !st->is_view && checked(dlg, ID_DATA);
    EnableWindow(GetDlgItem(dlg, IDOK), !sql || structure || data);
}

static void check(HWND dlg, int id, int on) { CheckDlgButton(dlg, id, on ? BST_CHECKED : BST_UNCHECKED); }

static INT_PTR CALLBACK export_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    export_state *st = (export_state *)GetWindowLongPtrW(dlg, DWLP_USER);
    switch (msg) {
    case WM_INITDIALOG: {
        st = (export_state *)lp;
        SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)st);
        HFONT bold = sqlc_w_bold_font();
        SendDlgItemMessageW(dlg, ID_TITLE, WM_SETFONT, (WPARAM)bold, FALSE);
        SendDlgItemMessageW(dlg, ID_SQL_HEADING, WM_SETFONT, (WPARAM)bold, FALSE);
        if (!st->database) {
            SendDlgItemMessageW(dlg, ID_CSV_HEADING, WM_SETFONT, (WPARAM)bold, FALSE);
            HWND combo = GetDlgItem(dlg, ID_FORMAT);
            SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"SQL (CREATE + INSERT)");
            SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"CSV");
            SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"XML");
            SendMessageW(combo, CB_SETCURSEL, (WPARAM)st->fmt, 0);
            check(dlg, ID_NULL_N, st->opts.csv_null_as_n);
            check(dlg, ID_BOM, st->opts.csv_bom);
        }
        check(dlg, ID_STRUCTURE, st->opts.sql_structure);
        check(dlg, ID_DROP, st->opts.sql_drop);
        check(dlg, ID_DATA, st->opts.sql_data && !st->is_view);
        update(dlg, st);
        return TRUE; /* focus on the first control */
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_FORMAT:
            if (HIWORD(wp) == CBN_SELCHANGE) update(dlg, st);
            return TRUE;
        case ID_STRUCTURE: case ID_DROP: case ID_DATA: case ID_NULL_N: case ID_BOM:
            update(dlg, st);
            return TRUE;
        case IDOK:
            if (!IsWindowEnabled(GetDlgItem(dlg, IDOK))) return TRUE; /* Return while disabled */
            if (!st->database)
                st->fmt = (export_format)SendDlgItemMessageW(dlg, ID_FORMAT, CB_GETCURSEL, 0, 0);
            st->opts.sql_structure = checked(dlg, ID_STRUCTURE);
            st->opts.sql_drop = checked(dlg, ID_DROP);
            st->opts.sql_data = !st->is_view && checked(dlg, ID_DATA);
            if (!st->database) {
                st->opts.csv_null_as_n = checked(dlg, ID_NULL_N);
                st->opts.csv_bom = checked(dlg, ID_BOM);
            }
            EndDialog(dlg, IDOK);
            return TRUE;
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* The options dialog; 1 if Export… was chosen (st filled in). */
static int run_options(export_state *st, const char *title, const char *message) {
    WCHAR *wt = sqlc_w_wide(title), *wm = sqlc_w_wide(message);
    int ok = 0;
    dlg_tmpl t = { 0 };
    if (!wt || !wm) goto done;

    const short W = 240, M = 7, CW = W - 2 * M, CH = 10, ROW = 13;
    short msg_h = (short)(8 * dlg_message_lines(message, 54));
    short y = M;
    short title_y = y; y += 10 + 3;
    short msg_y = y; y += msg_h + 6;
    short format_y = y;
    if (!st->database) y += 14 + 8;
    short sql_y = y; y += 10 + 2;
    short structure_y = y; y += ROW;
    short drop_y = y; y += ROW;
    short data_y = y; y += ROW + 6;
    short csv_y = y, null_y = 0, bom_y = 0;
    if (!st->database) { y += 10 + 2; null_y = y; y += ROW; bom_y = y; y += ROW + 6; }
    short btn_y = y; y += 14 + M;

    const char *views = st->is_view ? "VIEW" : "TABLE";
    WCHAR structure[64], drop[64];
    if (st->database) {
        lstrcpyW(structure, L"Structure (CREATE TABLE and CREATE VIEW)");
        lstrcpyW(drop, L"Add DROP TABLE / DROP VIEW IF EXISTS");
    } else {
        swprintf(structure, 64, L"Structure (CREATE %hs)", views);
        swprintf(drop, 64, L"Add DROP %hs IF EXISTS", views);
    }
    const WCHAR *data = st->database ? L"Data (INSERT statements, tables only)"
                      : st->is_view ? L"Data (not exported for views)" : L"Data (INSERT statements)";

    dlg_begin(&t, W, y);
    dlg_item(&t, SS_LEFT | SS_NOPREFIX, M, title_y, CW, 10, ID_TITLE, DLG_STATIC, wt);
    dlg_item(&t, SS_LEFT | SS_NOPREFIX, M, msg_y, CW, msg_h, ID_MESSAGE, DLG_STATIC, wm);
    if (!st->database) {
        dlg_item(&t, SS_LEFT, M, format_y + 2, 30, CH, ID_FORMAT_LABEL, DLG_STATIC, L"Format:");
        dlg_item(&t, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, M + 32, format_y, 120, 60,
                 ID_FORMAT, DLG_COMBOBOX, L"");
    }
    dlg_item(&t, SS_LEFT, M, sql_y, CW, 10, ID_SQL_HEADING, DLG_STATIC, L"SQL");
    dlg_item(&t, BS_AUTOCHECKBOX | WS_TABSTOP, M, structure_y, CW, CH, ID_STRUCTURE, DLG_BUTTON, structure);
    dlg_item(&t, BS_AUTOCHECKBOX | WS_TABSTOP, M + 10, drop_y, CW - 10, CH, ID_DROP, DLG_BUTTON, drop);
    dlg_item(&t, BS_AUTOCHECKBOX | WS_TABSTOP, M, data_y, CW, CH, ID_DATA, DLG_BUTTON, data);
    if (!st->database) {
        dlg_item(&t, SS_LEFT, M, csv_y, CW, 10, ID_CSV_HEADING, DLG_STATIC, L"CSV");
        dlg_item(&t, BS_AUTOCHECKBOX | WS_TABSTOP, M, null_y, CW, CH, ID_NULL_N, DLG_BUTTON,
                 L"Write NULL as \\N (otherwise an empty field)");
        dlg_item(&t, BS_AUTOCHECKBOX | WS_TABSTOP, M, bom_y, CW, CH, ID_BOM, DLG_BUTTON,
                 L"Start with a UTF-8 BOM (helps Excel)");
    }
    dlg_item(&t, BS_DEFPUSHBUTTON | WS_TABSTOP, W - M - 50 - 4 - 50, btn_y, 50, 14, IDOK, DLG_BUTTON,
             L"Export\x2026");
    dlg_item(&t, BS_PUSHBUTTON | WS_TABSTOP, W - M - 50, btn_y, 50, 14, IDCANCEL, DLG_BUTTON, L"Cancel");

    ok = dlg_run(&t, export_proc, (LPARAM)st) == IDOK;
done:
    dlg_free(&t);
    free(wt);
    free(wm);
    return ok;
}

/* Save As suggesting "<name>.<ext>"; 1 with *path (malloc'd, UTF-8) or 0. */
static int save_dialog(const char *name, export_format fmt, char **path) {
    const char *ext = export_extension(fmt);
    char base[512], suggested[600];
    export_safe_filename(base, sizeof base, name);
    snprintf(suggested, sizeof suggested, "%s.%s", base, ext);

    enum { BUF = 32768 };
    WCHAR *file = calloc(BUF, sizeof *file);
    WCHAR *wsug = sqlc_w_wide(suggested), *wext = sqlc_w_wide(ext);
    int ok = 0;
    if (file && wsug && wext) {
        lstrcpynW(file, wsug, BUF);
        /* "SQL files (*.sql)\0*.sql\0All files (*.*)\0*.*\0\0" */
        WCHAR filter[128];
        const WCHAR *kind = fmt == EXPORT_SQL ? L"SQL" : fmt == EXPORT_CSV ? L"CSV" : L"XML";
        int n = swprintf(filter, 100, L"%ls files (*.%ls)", kind, wext);
        n += 1 + swprintf(filter + n + 1, 20, L"*.%ls", wext);
        memcpy(filter + n + 1, L"All files (*.*)\0*.*\0", sizeof L"All files (*.*)\0*.*\0");

        OPENFILENAMEW ofn;
        memset(&ofn, 0, sizeof ofn);
        ofn.lStructSize = sizeof ofn;
        ofn.hwndOwner = sqlc_w_owner();
        ofn.lpstrFilter = filter;
        ofn.lpstrFile = file;
        ofn.nMaxFile = BUF;
        ofn.lpstrDefExt = wext;
        ofn.lpstrTitle = L"Export";
        ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                    OFN_ENABLESIZING | OFN_HIDEREADONLY;
        if (GetSaveFileNameW(&ofn)) {
            *path = sqlc_w_utf8(file);
            ok = *path != NULL;
        }
    }
    free(file);
    free(wsug);
    free(wext);
    return ok;
}

int sqlc_export_dialog(const char *table, int is_view,
                       export_format *fmt, export_opts *opts, char **path) {
    if (!have_last) {
        last_fmt = EXPORT_SQL;
        last_opts = export_default_opts();
        have_last = 1;
    }
    *path = NULL;
    const char *name = table && *table ? table : "table";
    char title[600];
    snprintf(title, sizeof title, "Export %s \xE2\x80\x9C%s\xE2\x80\x9D", is_view ? "view" : "table", name);
    export_state st = { 0, is_view, last_fmt, last_opts };
    if (!run_options(&st, title, "Choose a format, then where to save the file.")) return 0;
    last_fmt = st.fmt;
    if (!is_view) last_opts = st.opts; /* a view's forced "no data" shouldn't stick */
    else { int d = last_opts.sql_data; last_opts = st.opts; last_opts.sql_data = d; }
    if (!save_dialog(name, st.fmt, path)) return 0;
    *fmt = st.fmt;
    *opts = st.opts;
    return 1;
}

int sqlc_export_database_dialog(const char *db, size_t nselected, export_opts *opts,
                                char **path) {
    if (!have_last_db) {
        last_db_opts = export_default_opts();
        have_last_db = 1;
    }
    *path = NULL;
    const char *name = db && *db ? db : "database";
    char base[512], title[600];
    export_dump_basename(base, sizeof base, db, nselected);
    const char *message;
    if (nselected) {
        snprintf(title, sizeof title, "Export %zu tables from \xE2\x80\x9C%s\xE2\x80\x9D", nselected, name);
        message = "One SQL file with the selected tables, then the selected views. "
                  "Choose what to include, then where to save it.";
    } else {
        snprintf(title, sizeof title, "Export database \xE2\x80\x9C%s\xE2\x80\x9D", name);
        message = "One SQL file with every table, then the views. "
                  "Choose what to include, then where to save it.";
    }
    export_state st = { 1, 0, EXPORT_SQL, export_default_opts() };
    st.opts.sql_structure = last_db_opts.sql_structure;
    st.opts.sql_drop = last_db_opts.sql_drop;
    st.opts.sql_data = last_db_opts.sql_data;
    if (!run_options(&st, title, message)) return 0;
    last_db_opts = st.opts;
    if (!save_dialog(base, EXPORT_SQL, path)) return 0;
    *opts = st.opts;
    return 1;
}
