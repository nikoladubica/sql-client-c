#ifndef SQLC_WIN_UTIL_H
#define SQLC_WIN_UTIL_H

/*
 * Shared helpers for the Windows versions of the GUI shims (src/win/):
 * UTF-8 <-> UTF-16, the owner window for dialogs, small settings in the
 * registry (HKCU\Software\sqlc) and running code after the current event.
 * Main thread only.
 */

#include <windows.h>

/* UTF-8 -> UTF-16 / back, newly allocated (free()); NULL in -> L"" / "". NULL on failure. */
WCHAR *sqlc_w_wide(const char *s);
char *sqlc_w_utf8(const WCHAR *w);

/* The window dialogs belong to: the app's active window, or NULL. */
HWND sqlc_w_owner(void);

/* An integer setting under HKCU\Software\sqlc; *out unchanged if missing. 1 if read. */
int sqlc_w_get_int(const char *name, int *out);
void sqlc_w_set_int(const char *name, int value);

/* Call f(data) once the current message (a menu, a click) has been handled. */
void sqlc_w_post(void (*f)(void *data), void *data);

/* Dialog units / DPI: a length in 96-dpi pixels scaled for hwnd's monitor. */
int sqlc_w_scale(HWND hwnd, int px);

/* The UI font (Segoe UI, from libui), and a bold copy (made once, never freed). */
HFONT sqlc_w_font(void);
HFONT sqlc_w_bold_font(void);

/*
 * A task dialog (menu_win.c): buttons labels[0..n-1] with ids 100, 101, ...,
 * default_id the default (Return); warning shows the warning icon. Returns
 * the id clicked, or IDCANCEL (2) when closed with Escape or the close box,
 * which only works when allow_cancel.
 */
int sqlc_w_task_dialog(const char *title, const char *message, int warning,
                       const char *const *labels, int n, int default_id, int allow_cancel);

/* List views (table_menu_win.c), for uiTable's HWND: */
/* make row the only selected and focused row (libui sees it as a click) */
void sqlc_w_select_only_row(HWND lv, int row);
/* the row at a point on the screen, or -1; *col (if col) its column */
int sqlc_w_row_at(HWND lv, POINT screen, int *col);

#endif
