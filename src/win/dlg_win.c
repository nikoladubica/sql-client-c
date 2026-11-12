/* Modal dialogs built in memory (see dlg_win.h). */

#include "dlg_win.h"

#include <stdlib.h>

#include "win_util.h"

static void put(dlg_tmpl *t, WORD w) {
    if (t->failed) return;
    if (t->n == t->cap) {
        size_t ncap = t->cap ? t->cap * 2 : 512;
        WORD *b = realloc(t->buf, ncap * sizeof *b);
        if (!b) { t->failed = 1; return; }
        t->buf = b;
        t->cap = ncap;
    }
    t->buf[t->n++] = w;
}

static void put_dword(dlg_tmpl *t, DWORD d) { put(t, LOWORD(d)); put(t, HIWORD(d)); }
static void put_str(dlg_tmpl *t, const WCHAR *s) { do put(t, *s); while (*s++); }
static void align(dlg_tmpl *t) { if (t->n % 2) put(t, 0); } /* DWORD boundary */

void dlg_begin(dlg_tmpl *t, short cx, short cy) {
    t->buf = NULL;
    t->n = t->cap = 0;
    t->failed = 0;
    put_dword(t, DS_MODALFRAME | DS_SETFONT | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU);
    put_dword(t, 0);          /* exStyle */
    t->count_at = t->n;
    put(t, 0);                /* controls, counted by dlg_item */
    put(t, 0); put(t, 0); put(t, (WORD)cx); put(t, (WORD)cy);
    put(t, 0); put(t, 0);     /* no menu, default class */
    put_str(t, L"sqlc");
    put(t, 9); put_str(t, L"Segoe UI");
}

void dlg_item(dlg_tmpl *t, DWORD style, short x, short y, short cx, short cy, WORD id,
              WORD cls, const WCHAR *text) {
    align(t);
    put_dword(t, style | WS_CHILD | WS_VISIBLE);
    put_dword(t, 0);
    put(t, (WORD)x); put(t, (WORD)y); put(t, (WORD)cx); put(t, (WORD)cy);
    put(t, id);
    put(t, 0xFFFF); put(t, cls);
    put_str(t, text ? text : L"");
    put(t, 0);                /* no creation data */
    if (!t->failed) t->buf[t->count_at]++;
}

INT_PTR dlg_run(dlg_tmpl *t, DLGPROC proc, LPARAM param) {
    if (t->failed || !t->buf) return -1;
    return DialogBoxIndirectParamW(GetModuleHandleW(NULL), (LPCDLGTEMPLATEW)t->buf,
                                   sqlc_w_owner(), proc, param);
}

void dlg_free(dlg_tmpl *t) {
    free(t->buf);
    t->buf = NULL;
    t->n = t->cap = 0;
}

int dlg_message_lines(const char *s, int chars_per_line) {
    int lines = 1, col = 0;
    for (; s && *s; s++) {
        if (*s == '\n') { lines++; col = 0; continue; }
        if ((*s & 0xC0) == 0x80) continue; /* UTF-8 continuation byte */
        if (++col > chars_per_line) { lines++; col = 0; }
    }
    return lines;
}
