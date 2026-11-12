/* Windows version of table_menu.h: a right-click menu for a uiTable's list
 * view, the clipboard, text prompts and "are you sure?" alerts. */

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"
#include "ui_windows.h"
#include "table_menu.h"
#include "win_util.h"

/* ---------- context menu ---------- */

static const char *const titles[TABLE_MENU_COUNT] = {
    "Show Structure",
    "Copy Table Name",
    "Rename Table\xE2\x80\xA6",
    "Truncate Table\xE2\x80\xA6",
    "Delete Table\xE2\x80\xA6",
    "Export\xE2\x80\xA6",
};

#define MENU_SUBCLASS_ID 0x536d

typedef struct {
    table_menu_open_fn on_open;
    table_menu_action_fn on_action;
    void *data;
} table_menu;

typedef struct {
    table_menu_action_fn fn;
    table_menu_item item;
    int row;
    void *data;
} pending_action;

static void run_action(void *p) {
    pending_action *a = p;
    a->fn(a->item, a->row, a->data);
    free(a);
}

/* Make row the list view's only selected (and focused) row; the list view
 * tells libui, which updates the table's selection like a click. */
void sqlc_w_select_only_row(HWND lv, int row) {
    LVITEMW it;
    memset(&it, 0, sizeof it);
    it.stateMask = LVIS_SELECTED;
    it.state = 0;
    SendMessageW(lv, LVM_SETITEMSTATE, (WPARAM)-1, (LPARAM)&it);
    it.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    it.state = LVIS_SELECTED | LVIS_FOCUSED;
    SendMessageW(lv, LVM_SETITEMSTATE, (WPARAM)row, (LPARAM)&it);
}

/* The menu for row (selecting it), or NULL for no menu. */
static HMENU build_menu(HWND lv, table_menu *m, int row) {
    int n = (int)SendMessageW(lv, LVM_GETITEMCOUNT, 0, 0);
    if (row < 0 || row >= n) return NULL; /* empty space: no menu */
    sqlc_w_select_only_row(lv, row);
    unsigned enabled = m->on_open ? m->on_open(row, m->data) : TABLE_MENU_ALL;
    if (enabled == 0) return NULL;
    HMENU menu = CreatePopupMenu();
    if (!menu) return NULL;
    for (int i = 0; i < TABLE_MENU_COUNT; i++) {
        if (i == TABLE_MENU_RENAME || i == TABLE_MENU_EXPORT) AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        WCHAR *w = sqlc_w_wide(titles[i]);
        AppendMenuW(menu, MF_STRING | ((enabled & TABLE_MENU_BIT(i)) ? 0 : MF_GRAYED), (UINT_PTR)(i + 1), w ? w : L"");
        free(w);
    }
    return menu;
}

/* The list-view row at a screen point, or -1. */
int sqlc_w_row_at(HWND lv, POINT screen, int *col) {
    LVHITTESTINFO hi;
    memset(&hi, 0, sizeof hi);
    hi.pt = screen;
    ScreenToClient(lv, &hi.pt);
    int row = (int)SendMessageW(lv, LVM_SUBITEMHITTEST, 0, (LPARAM)&hi);
    if (col) *col = hi.iSubItem;
    return row >= 0 && (hi.flags & LVHT_ONITEM) ? hi.iItem : -1;
}

static LRESULT CALLBACK menu_proc(HWND lv, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    table_menu *m = (table_menu *)ref;
    if (msg == WM_CONTEXTMENU) {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int row;
        if (p.x == -1 && p.y == -1) { /* Shift+F10 / menu key: the focused row */
            row = (int)SendMessageW(lv, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_FOCUSED);
            RECT r = { LVIR_BOUNDS, 0, 0, 0 };
            if (row < 0 || !SendMessageW(lv, LVM_GETITEMRECT, (WPARAM)row, (LPARAM)&r)) return 0;
            p.x = r.left;
            p.y = r.bottom;
            ClientToScreen(lv, &p);
        } else {
            row = sqlc_w_row_at(lv, p, NULL);
        }
        HMENU menu = build_menu(lv, m, row);
        if (!menu) return 0;
        int chosen = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                         p.x, p.y, 0, lv, NULL);
        DestroyMenu(menu);
        if (chosen > 0 && m->on_action) {
            /* after the menu and this message are done, so the action may
             * show dialogs or rebuild the table */
            pending_action *a = malloc(sizeof *a);
            if (a) {
                *a = (pending_action){ m->on_action, (table_menu_item)(chosen - 1), row, m->data };
                sqlc_w_post(run_action, a);
            }
        }
        return 0;
    }
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(lv, menu_proc, id);
        free(m);
    }
    return DefSubclassProc(lv, msg, wp, lp);
}

void sqlc_table_set_context_menu(uiTable *t, table_menu_open_fn on_open,
                                 table_menu_action_fn on_action, void *data) {
    HWND lv = (HWND)uiControlHandle(uiControl(t));
    table_menu *m = calloc(1, sizeof *m);
    if (!m) return;
    m->on_open = on_open;
    m->on_action = on_action;
    m->data = data;
    if (!SetWindowSubclass(lv, menu_proc, MENU_SUBCLASS_ID, (DWORD_PTR)m)) free(m);
}

void sqlc_table_menu_describe(uiTable *t, int row, char *buf, size_t size) {
    if (size) buf[0] = '\0';
    HWND lv = (HWND)uiControlHandle(uiControl(t));
    DWORD_PTR ref = 0;
    if (!GetWindowSubclass(lv, menu_proc, MENU_SUBCLASS_ID, &ref) || !ref) return;
    HMENU menu = build_menu(lv, (table_menu *)ref, row);
    if (!menu) return;
    size_t used = 0;
    int n = GetMenuItemCount(menu);
    for (int i = 0; i < n; i++) {
        UINT state = GetMenuState(menu, (UINT)i, MF_BYPOSITION);
        if (state & MF_SEPARATOR) continue;
        UINT id = GetMenuItemID(menu, i);
        if (id < 1 || id > TABLE_MENU_COUNT) continue;
        int w = snprintf(buf + used, size > used ? size - used : 0, "%s%s%s", used ? ", " : "",
                         titles[id - 1], (state & MF_GRAYED) ? "-" : "+");
        if (w > 0) used += (size_t)w;
        if (used >= size) break;
    }
    DestroyMenu(menu);
}

/* ---------- clipboard ---------- */

void sqlc_copy_to_clipboard(const char *text) {
    if (!text) return;
    /* Windows text uses \r\n line ends */
    size_t n = 0, lf = 0;
    for (const char *p = text; *p; p++, n++) if (*p == '\n' && (p == text || p[-1] != '\r')) lf++;
    char *crlf = malloc(n + lf + 1);
    if (!crlf) return;
    char *o = crlf;
    for (const char *p = text; *p; p++) {
        if (*p == '\n' && (p == text || p[-1] != '\r')) *o++ = '\r';
        *o++ = *p;
    }
    *o = '\0';
    WCHAR *w = sqlc_w_wide(crlf);
    free(crlf);
    if (!w) return;
    size_t bytes = (wcslen(w) + 1) * sizeof *w;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem) {
        void *dst = GlobalLock(mem);
        if (dst) {
            memcpy(dst, w, bytes);
            GlobalUnlock(mem);
            if (OpenClipboard(sqlc_w_owner())) {
                EmptyClipboard();
                if (SetClipboardData(CF_UNICODETEXT, mem)) mem = NULL; /* the clipboard owns it now */
                CloseClipboard();
            }
        }
        if (mem) GlobalFree(mem);
    }
    free(w);
}

char *sqlc_clipboard_text(void) {
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(sqlc_w_owner())) return NULL;
    char *out = NULL;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    const WCHAR *w = h ? GlobalLock(h) : NULL;
    if (w) {
        out = sqlc_w_utf8(w);
        GlobalUnlock(h);
    }
    CloseClipboard();
    if (out) { /* \r\n -> \n */
        char *o = out;
        for (const char *p = out; *p; p++)
            if (!(p[0] == '\r' && p[1] == '\n')) *o++ = *p;
        *o = '\0';
    }
    return out;
}

/* ---------- text prompt: a dialog built in memory ---------- */

typedef struct {
    WORD *buf;
    size_t n, cap;
    int failed; /* out of memory: the template is incomplete, don't use it */
} tmpl;

static void t_put(tmpl *t, WORD w) {
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

static void t_dword(tmpl *t, DWORD d) { t_put(t, LOWORD(d)); t_put(t, HIWORD(d)); }
static void t_str(tmpl *t, const WCHAR *s) { do t_put(t, *s); while (*s++); }
static void t_align(tmpl *t) { if (t->n % 2) t_put(t, 0); } /* DWORD boundary */

/* One control: class atom 0x80 button, 0x81 edit, 0x82 static. */
static void t_item(tmpl *t, DWORD style, short x, short y, short cx, short cy, WORD id,
                   WORD cls, const WCHAR *text) {
    t_align(t);
    t_dword(t, style | WS_CHILD | WS_VISIBLE);
    t_dword(t, 0);           /* exStyle */
    t_put(t, (WORD)x); t_put(t, (WORD)y); t_put(t, (WORD)cx); t_put(t, (WORD)cy);
    t_put(t, id);
    t_put(t, 0xFFFF); t_put(t, cls);
    t_str(t, text);
    t_put(t, 0);             /* no creation data */
}

#define ID_TITLE 200
#define ID_MESSAGE 201
#define ID_FIELD 202

typedef struct {
    const WCHAR *initial;
    char *result;
} prompt_state;

static INT_PTR CALLBACK prompt_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    prompt_state *ps = (prompt_state *)GetWindowLongPtrW(dlg, DWLP_USER);
    switch (msg) {
    case WM_INITDIALOG: {
        ps = (prompt_state *)lp;
        SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)ps);
        SendDlgItemMessageW(dlg, ID_TITLE, WM_SETFONT, (WPARAM)sqlc_w_bold_font(), FALSE);
        HWND field = GetDlgItem(dlg, ID_FIELD);
        SetWindowTextW(field, ps->initial);
        SendMessageW(field, EM_SETSEL, 0, -1);
        SetFocus(field);
        return FALSE; /* focus set */
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            HWND field = GetDlgItem(dlg, ID_FIELD);
            int len = GetWindowTextLengthW(field);
            WCHAR *w = calloc((size_t)len + 1, sizeof *w);
            if (w) {
                GetWindowTextW(field, w, len + 1);
                ps->result = sqlc_w_utf8(w);
                SecureZeroMemory(w, ((size_t)len + 1) * sizeof *w); /* may be a key */
                free(w);
            }
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* Lines a message needs at about 60 characters a line. */
static int message_lines(const char *s) {
    int lines = 1, col = 0;
    for (; s && *s; s++) {
        if (*s == '\n') { lines++; col = 0; continue; }
        if ((*s & 0xC0) == 0x80) continue; /* UTF-8 continuation byte */
        if (++col > 60) { lines++; col = 0; }
    }
    return lines;
}

static char *prompt(const char *title, const char *message, const char *initial,
                    const char *ok_label, int secure) {
    WCHAR *wt = sqlc_w_wide(title), *wm = sqlc_w_wide(message), *wi = sqlc_w_wide(initial),
          *wo = sqlc_w_wide(ok_label ? ok_label : "OK");
    char *out = NULL;
    tmpl t = { 0 };
    if (!wt || !wm || !wi || !wo) goto done;

    const short W = 260, M = 7;          /* dialog units */
    short msg_h = (short)(8 * message_lines(message));
    short y = M;
    short title_y = y; y += 10 + 4;
    short msg_y = y; y += msg_h + 6;
    short field_y = y; y += 14 + 10;
    short btn_y = y; y += 14 + M;

    t_dword(&t, DS_MODALFRAME | DS_SETFONT | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU);
    t_dword(&t, 0);
    t_put(&t, 5);                         /* controls */
    t_put(&t, 0); t_put(&t, 0); t_put(&t, (WORD)W); t_put(&t, (WORD)y);
    t_put(&t, 0); t_put(&t, 0);           /* no menu, default class */
    t_str(&t, L"sqlc");
    t_put(&t, 9); t_str(&t, L"Segoe UI");

    t_item(&t, SS_LEFT | SS_NOPREFIX, M, title_y, W - 2 * M, 10, ID_TITLE, 0x82, wt);
    t_item(&t, SS_LEFT | SS_NOPREFIX, M, msg_y, W - 2 * M, msg_h, ID_MESSAGE, 0x82, wm);
    t_item(&t, ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP | (secure ? ES_PASSWORD : 0),
           M, field_y, W - 2 * M, 14, ID_FIELD, 0x81, L"");
    t_item(&t, BS_DEFPUSHBUTTON | WS_TABSTOP, W - M - 50 - 4 - 50, btn_y, 50, 14, IDOK, 0x80, wo);
    t_item(&t, BS_PUSHBUTTON | WS_TABSTOP, W - M - 50, btn_y, 50, 14, IDCANCEL, 0x80, L"Cancel");
    if (t.failed || !t.buf) goto done;

    prompt_state ps = { wi, NULL };
    if (DialogBoxIndirectParamW(GetModuleHandleW(NULL), (LPCDLGTEMPLATEW)t.buf, sqlc_w_owner(),
                                prompt_proc, (LPARAM)&ps) == IDOK)
        out = ps.result;
    else
        free(ps.result);
done:
    free(t.buf);
    free(wt);
    free(wm);
    free(wi);
    free(wo);
    return out;
}

char *sqlc_prompt(const char *title, const char *message, const char *initial,
                  const char *ok_label) {
    return prompt(title, message, initial ? initial : "", ok_label, 0);
}

char *sqlc_prompt_secret(const char *title, const char *message, const char *ok_label) {
    return prompt(title, message, "", ok_label, 1);
}

/* ---------- are you sure? ---------- */

int sqlc_confirm_danger(const char *title, const char *message, const char *ok_label,
                        int destructive) {
    /* Cancel is the default; Escape cancels too */
    const char *labels[] = { ok_label ? ok_label : "OK", "Cancel" };
    return sqlc_w_task_dialog(title, message, destructive, labels, 2, 101, 1) == 100;
}

int sqlc_confirm_keep(const char *title, const char *message, const char *action_label,
                      const char *keep_label) {
    /* keep is the default; Escape does nothing (no cancellation allowed) */
    const char *labels[] = { action_label ? action_label : "OK", keep_label ? keep_label : "Cancel" };
    return sqlc_w_task_dialog(title, message, 1, labels, 2, 101, 0) == 100;
}
