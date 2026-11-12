#ifndef SQLC_DLG_WIN_H
#define SQLC_DLG_WIN_H

/*
 * Modal dialogs built in memory (DLGTEMPLATE), for the prompts and the
 * export options: libui-ng has no dialog with custom controls. Sizes are
 * dialog units; the font is Segoe UI 9.
 */

#include <windows.h>

typedef struct {
    WORD *buf;
    size_t n, cap;
    size_t count_at;  /* where the header's control count is */
    int failed;       /* out of memory: incomplete, dlg_run refuses it */
} dlg_tmpl;

/* Control classes */
#define DLG_BUTTON 0x80   /* push button, check box */
#define DLG_EDIT 0x81
#define DLG_STATIC 0x82
#define DLG_COMBOBOX 0x85

/* Start a dialog cx x cy with the caption "sqlc", centred on its owner. */
void dlg_begin(dlg_tmpl *t, short cx, short cy);

/* Add a control (WS_CHILD | WS_VISIBLE are added to style). */
void dlg_item(dlg_tmpl *t, DWORD style, short x, short y, short cx, short cy, WORD id,
              WORD cls, const WCHAR *text);

/* Run it modally over the app's window; the EndDialog result, or -1. Frees nothing. */
INT_PTR dlg_run(dlg_tmpl *t, DLGPROC proc, LPARAM param);

void dlg_free(dlg_tmpl *t);

/* Lines a message needs at about chars_per_line characters a line (UTF-8). */
int dlg_message_lines(const char *s, int chars_per_line);

#endif
