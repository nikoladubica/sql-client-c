/* Windows version of grid_edit.h: an edit control laid over the
 * double-clicked cell of a uiTable's list view, with a month calendar (and
 * for date-and-time columns a time picker) in a popup under it for date
 * columns, and a cell context menu. */

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"
#include "ui_windows.h"
#include "grid_edit.h"
#include "row_edit.h"
#include "win_util.h"

#define GRID_SUBCLASS_ID 0x5367
#define FIELD_SUBCLASS_ID 0x5366

/*
 * One per table. refs: 1 for the list view (dropped when it is destroyed)
 * plus 1 per callback still queued, so a queued callback never reads freed
 * memory; dead (set by sqlc_grid_detach) drops those callbacks.
 */
typedef struct grid_editor {
    HWND lv;
    grid_edit_callbacks cb;
    int refs;
    int dead;
    HWND field;          /* open editor, or NULL */
    int row, col;
    int finishing;
    grid_editor_kind kind;
    HWND popup;          /* calendar popup (date columns), or NULL */
    HWND cal, clock;     /* its month calendar and, for datetime, time picker */
    int syncing;         /* field <-> calendar update in progress */
} grid_editor;

static void unref(grid_editor *e) {
    if (--e->refs == 0) free(e);
}

/* ---------- queued callbacks ---------- */

typedef enum { CALL_DOUBLE, CALL_COMMIT, CALL_CANCEL, CALL_MENU } call_kind;

typedef struct {
    grid_editor *e;
    call_kind kind;
    int row, col;
    char *text;
    grid_end how;
    grid_menu_item item;
} queued_call;

static void run_call(void *p) {
    queued_call *q = p;
    grid_editor *e = q->e;
    if (!e->dead) {
        const grid_edit_callbacks *cb = &e->cb;
        switch (q->kind) {
        case CALL_DOUBLE: if (cb->double_clicked) cb->double_clicked(q->row, q->col, cb->data); break;
        case CALL_COMMIT: if (cb->committed) cb->committed(q->row, q->col, q->text, q->how, cb->data); break;
        case CALL_CANCEL: if (cb->cancelled) cb->cancelled(q->row, q->col, cb->data); break;
        case CALL_MENU: if (cb->menu_action) cb->menu_action(q->item, q->row, q->col, cb->data); break;
        }
    }
    free(q->text);
    free(q);
    unref(e);
}

/* Queue a callback to run once the current message is done. Takes text. */
static void queue(grid_editor *e, call_kind kind, int row, int col, char *text, grid_end how,
                  grid_menu_item item) {
    queued_call *q = calloc(1, sizeof *q);
    if (!q) { free(text); return; }
    *q = (queued_call){ e, kind, row, col, text, how, item };
    e->refs++;
    sqlc_w_post(run_call, q);
}

/* ---------- the editor ---------- */

static char *field_text(grid_editor *e) {
    int len = GetWindowTextLengthW(e->field);
    WCHAR *w = calloc((size_t)len + 1, sizeof *w);
    if (!w) return NULL;
    GetWindowTextW(e->field, w, len + 1);
    char *s = sqlc_w_utf8(w);
    free(w);
    return s;
}

/* ---------- the calendar popup ---------- */

#define POPUP_CLASS L"sqlc_calendar_popup"
#define POPUP_PAD 8 /* points */

static void field_from_calendar(grid_editor *e);
static int in_popup(grid_editor *e, HWND hwnd);
static void finish(grid_editor *e, grid_end how);

/* After the popup lost activation: if the keyboard went neither to the
 * field nor into the popup, the user clicked elsewhere: end the edit. */
static void check_blur(void *p) {
    grid_editor *e = p;
    if (!e->dead && e->field && !e->finishing) {
        HWND f = GetFocus();
        if (f != e->field && !in_popup(e, f)) finish(e, GRID_END_BLUR);
    }
    unref(e);
}

static LRESULT CALLBACK popup_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    grid_editor *e = (grid_editor *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE; /* the field keeps the keyboard */
    case WM_ACTIVATE:
        if (e && LOWORD(wp) == WA_INACTIVE) { /* a control in it had taken the focus */
            e->refs++;
            sqlc_w_post(check_blur, e);
        }
        break;
    case WM_NOTIFY: {
        const NMHDR *nm = (const NMHDR *)lp;
        if (e && ((nm->hwndFrom == e->cal && nm->code == MCN_SELECT) ||
                  (nm->hwndFrom == e->clock && nm->code == DTN_DATETIMECHANGE)))
            field_from_calendar(e);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int register_popup(void) {
    static ATOM atom;
    if (!atom) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = popup_proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = POPUP_CLASS;
        atom = RegisterClassW(&wc);
    }
    return atom != 0;
}

/* 1 if hwnd is the popup or inside it. */
static int in_popup(grid_editor *e, HWND hwnd) {
    return e->popup && hwnd && (hwnd == e->popup || IsChild(e->popup, hwnd));
}

/* Under the field (above it when there's no room below), on its screen. */
static void place_popup(grid_editor *e) {
    if (!e->popup || !e->field) return;
    RECT f, p;
    GetWindowRect(e->field, &f);
    GetWindowRect(e->popup, &p);
    int w = p.right - p.left, h = p.bottom - p.top;
    MONITORINFO mi;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromWindow(e->field, MONITOR_DEFAULTTONEAREST), &mi);
    int x = f.left, y = f.bottom + 2;
    if (y + h > mi.rcWork.bottom && f.top - 2 - h >= mi.rcWork.top) y = f.top - 2 - h;
    if (x + w > mi.rcWork.right) x = mi.rcWork.right - w;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    SetWindowPos(e->popup, HWND_TOP, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}

/* Show the typed date (if it is one) in the calendar and clock. */
static void calendar_from_field(grid_editor *e) {
    if (!e->popup || e->syncing) return;
    char *text = field_text(e);
    int y, mo, d, h, mi, sec;
    if (text && row_edit_calendar_value(text, &y, &mo, &d, &h, &mi, &sec)) {
        SYSTEMTIME st = { (WORD)y, (WORD)mo, 0, (WORD)d, (WORD)h, (WORD)mi, (WORD)sec, 0 };
        e->syncing = 1;
        SendMessageW(e->cal, MCM_SETCURSEL, 0, (LPARAM)&st);
        if (e->clock) SendMessageW(e->clock, DTM_SETSYSTEMTIME, GDT_VALID, (LPARAM)&st);
        e->syncing = 0;
    }
    free(text);
}

/* The picked date (and time) into the field, and the keyboard back to it
 * so Return saves and Escape cancels. */
static void field_from_calendar(grid_editor *e) {
    if (!e->field || e->syncing) return;
    SYSTEMTIME day, time;
    memset(&time, 0, sizeof time);
    SendMessageW(e->cal, MCM_GETCURSEL, 0, (LPARAM)&day);
    if (e->clock) SendMessageW(e->clock, DTM_GETSYSTEMTIME, 0, (LPARAM)&time);
    WCHAR buf[32];
    if (e->kind == GRID_EDITOR_DATE)
        swprintf(buf, 32, L"%04u-%02u-%02u", day.wYear, day.wMonth, day.wDay);
    else
        swprintf(buf, 32, L"%04u-%02u-%02u %02u:%02u:%02u", day.wYear, day.wMonth, day.wDay,
                 time.wHour, time.wMinute, time.wSecond);
    e->syncing = 1;
    SetWindowTextW(e->field, buf);
    e->syncing = 0;
    HWND root = GetAncestor(e->lv, GA_ROOT);
    if (GetActiveWindow() != root) SetActiveWindow(root);
    if (GetFocus() != e->field) {
        e->finishing = 1; /* our own focus change */
        SetFocus(e->field);
        e->finishing = 0;
    }
    int len = GetWindowTextLengthW(e->field);
    SendMessageW(e->field, EM_SETSEL, (WPARAM)len, (LPARAM)len);
}

static void show_calendar(grid_editor *e) {
    if (!register_popup()) return;
    HWND root = GetAncestor(e->lv, GA_ROOT);
    e->popup = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_CONTROLPARENT, POPUP_CLASS, L"",
                               WS_POPUP | WS_BORDER, 0, 0, 10, 10, root, NULL, GetModuleHandleW(NULL), NULL);
    if (!e->popup) return;
    SetWindowLongPtrW(e->popup, GWLP_USERDATA, (LONG_PTR)e);
    int pad = sqlc_w_scale(e->lv, POPUP_PAD);
    e->cal = CreateWindowExW(0, MONTHCAL_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                             pad, pad, 10, 10, e->popup, NULL, GetModuleHandleW(NULL), NULL);
    RECT need = { 0 };
    if (e->cal) MonthCal_GetMinReqRect(e->cal, &need);
    int cw = need.right - need.left, ch = need.bottom - need.top;
    if (e->cal) SetWindowPos(e->cal, NULL, pad, pad, cw, ch, SWP_NOZORDER | SWP_NOACTIVATE);
    int h = pad + ch + pad;
    if (e->kind == GRID_EDITOR_DATETIME) {
        int clock_h = sqlc_w_scale(e->lv, 24);
        e->clock = CreateWindowExW(0, DATETIMEPICK_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                   DTS_TIMEFORMAT | DTS_UPDOWN, pad, h, cw, clock_h, e->popup, NULL,
                                   GetModuleHandleW(NULL), NULL);
        if (e->clock) {
            SendMessageW(e->clock, WM_SETFONT, SendMessageW(e->lv, WM_GETFONT, 0, 0), FALSE);
            SendMessageW(e->clock, DTM_SETFORMATW, 0, (LPARAM)L"HH':'mm':'ss");
        }
        h += clock_h + pad;
    }
    /* start at the typed date, or today */
    SYSTEMTIME today;
    GetLocalTime(&today);
    today.wHour = today.wMinute = today.wSecond = today.wMilliseconds = 0;
    e->syncing = 1;
    if (e->cal) SendMessageW(e->cal, MCM_SETCURSEL, 0, (LPARAM)&today);
    if (e->clock) SendMessageW(e->clock, DTM_SETSYSTEMTIME, GDT_VALID, (LPARAM)&today);
    e->syncing = 0;
    calendar_from_field(e);
    RECT wr = { 0, 0, pad + cw + pad, h };
    AdjustWindowRectEx(&wr, WS_POPUP | WS_BORDER, FALSE, WS_EX_TOOLWINDOW);
    SetWindowPos(e->popup, HWND_TOP, 0, 0, wr.right - wr.left, wr.bottom - wr.top, SWP_NOMOVE | SWP_NOACTIVATE);
    place_popup(e);
    ShowWindow(e->popup, SW_SHOWNOACTIVATE);
}

static void close_quietly(grid_editor *e) {
    if (e->popup) {
        HWND p = e->popup;
        e->popup = e->cal = e->clock = NULL;
        DestroyWindow(p);
    }
    if (!e->field) return;
    HWND f = e->field;
    e->field = NULL;
    int had_focus = GetFocus() == f;
    DestroyWindow(f); /* its WM_NCDESTROY removes field_proc */
    if (had_focus && IsWindow(e->lv)) SetFocus(e->lv);
}

/* Where the editor goes: the cell, at least 140 x 22 points. */
static RECT cell_rect(grid_editor *e, int row, int col) {
    RECT r = { 0 };
    r.top = col;
    r.left = col == 0 ? LVIR_LABEL : LVIR_BOUNDS;
    SendMessageW(e->lv, LVM_GETSUBITEMRECT, (WPARAM)row, (LPARAM)&r);
    int minw = sqlc_w_scale(e->lv, 140), minh = sqlc_w_scale(e->lv, 22);
    if (r.right - r.left < minw) r.right = r.left + minw;
    if (r.bottom - r.top < minh) {
        int grow = minh - (r.bottom - r.top);
        r.top -= grow / 2;
        r.bottom = r.top + minh;
    }
    return r;
}

static void place_field(grid_editor *e) {
    if (!e->field) return;
    RECT r = cell_rect(e, e->row, e->col);
    SetWindowPos(e->field, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE);
    place_popup(e);
}

static void finish(grid_editor *e, grid_end how) {
    if (!e->field || e->finishing) return;
    e->finishing = 1;
    char *text = field_text(e);
    int r = e->row, c = e->col;
    close_quietly(e);
    e->finishing = 0;
    if (text) queue(e, CALL_COMMIT, r, c, text, how, 0);
}

static void cancel(grid_editor *e) {
    if (!e->field) return;
    int r = e->row, c = e->col;
    e->finishing = 1;
    close_quietly(e);
    e->finishing = 0;
    queue(e, CALL_CANCEL, r, c, NULL, 0, 0);
}

static LRESULT CALLBACK field_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    grid_editor *e = (grid_editor *)ref;
    switch (msg) {
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DefSubclassProc(hwnd, msg, wp, lp);
    case WM_KEYDOWN:
        if (wp == VK_RETURN) { finish(e, GRID_END_RETURN); return 0; }
        if (wp == VK_TAB) { finish(e, GetKeyState(VK_SHIFT) < 0 ? GRID_END_BACKTAB : GRID_END_TAB); return 0; }
        if (wp == VK_ESCAPE) { cancel(e); return 0; }
        break;
    case WM_CHAR:
        if (wp == '\r' || wp == '\t' || wp == 27) return 0; /* no beep */
        break;
    case WM_KILLFOCUS:
        if (e->field == hwnd && !e->finishing && !in_popup(e, (HWND)wp)) {
            LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
            finish(e, GRID_END_BLUR); /* clicked elsewhere */
            return r;
        }
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, field_proc, id);
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static void begin(grid_editor *e, int row, int col, const char *text, const char *placeholder,
                  grid_editor_kind kind) {
    close_quietly(e);
    int nrows = (int)SendMessageW(e->lv, LVM_GETITEMCOUNT, 0, 0);
    HWND header = (HWND)SendMessageW(e->lv, LVM_GETHEADER, 0, 0);
    int ncols = header ? Header_GetItemCount(header) : 0;
    if (row < 0 || col < 0 || row >= nrows || col >= ncols) return;
    e->row = row;
    e->col = col;
    e->kind = kind;
    SendMessageW(e->lv, LVM_ENSUREVISIBLE, (WPARAM)row, FALSE);
    /* scroll the column into view horizontally */
    RECT cr = cell_rect(e, row, col), client;
    GetClientRect(e->lv, &client);
    if (cr.left < client.left || cr.right > client.right) {
        int dx = cr.left < client.left ? cr.left - client.left : cr.right - client.right;
        if (cr.right - cr.left > client.right - client.left) dx = cr.left - client.left;
        SendMessageW(e->lv, LVM_SCROLL, (WPARAM)dx, 0);
    }
    WCHAR *w = sqlc_w_wide(text);
    e->finishing = 1; /* the focus change below is ours */
    e->field = CreateWindowExW(WS_EX_CLIENTEDGE, L"edit", w ? w : L"",
                               WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                               0, 0, 10, 10, e->lv, NULL, GetModuleHandleW(NULL), NULL);
    free(w);
    if (!e->field) { e->finishing = 0; return; }
    SendMessageW(e->field, WM_SETFONT, SendMessageW(e->lv, WM_GETFONT, 0, 0), FALSE);
    if (placeholder) {
        WCHAR *wp = sqlc_w_wide(placeholder);
        if (wp) SendMessageW(e->field, EM_SETCUEBANNER, TRUE, (LPARAM)wp);
        free(wp);
    }
    SetWindowSubclass(e->field, field_proc, FIELD_SUBCLASS_ID, (DWORD_PTR)e);
    place_field(e);
    SetFocus(e->field);
    SendMessageW(e->field, EM_SETSEL, 0, -1);
    if (kind != GRID_EDITOR_TEXT) show_calendar(e);
    e->finishing = 0;
}

/* ---------- context menu ---------- */

static void show_menu(grid_editor *e, POINT screen, int row, int col) {
    int nrows = (int)SendMessageW(e->lv, LVM_GETITEMCOUNT, 0, 0);
    if (row < 0 || row >= nrows) return;
    /* keep a multi-row selection that includes the row; otherwise select it */
    if (!(SendMessageW(e->lv, LVM_GETITEMSTATE, (WPARAM)row, LVIS_SELECTED) & LVIS_SELECTED))
        sqlc_w_select_only_row(e->lv, row);
    unsigned enabled = e->cb.menu_open ? e->cb.menu_open(row, col, e->cb.data) : 0;
    if (enabled == 0) return;
    int nsel = (int)SendMessageW(e->lv, LVM_GETSELECTEDCOUNT, 0, 0);
    WCHAR del[64];
    if (nsel > 1) wsprintfW(del, L"Delete %d Rows\x2026", nsel);
    else lstrcpyW(del, L"Delete Row\x2026");
    const WCHAR *titles[GRID_MENU_COUNT] = { L"Edit Cell", L"Set to NULL", L"Copy Value", del };
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    for (int i = 0; i < GRID_MENU_COUNT; i++) {
        if (i == GRID_MENU_COPY || i == GRID_MENU_DELETE_ROWS) AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_STRING | ((enabled & GRID_MENU_BIT(i)) ? 0 : MF_GRAYED), (UINT_PTR)(i + 1), titles[i]);
    }
    int chosen = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                     screen.x, screen.y, 0, e->lv, NULL);
    DestroyMenu(menu);
    if (chosen > 0) queue(e, CALL_MENU, row, col, NULL, 0, (grid_menu_item)(chosen - 1));
}

/* ---------- the list view ---------- */

static LRESULT CALLBACK grid_proc(HWND lv, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    grid_editor *e = (grid_editor *)ref;
    switch (msg) {
    case WM_LBUTTONDBLCLK: {
        LRESULT r = DefSubclassProc(lv, msg, wp, lp);
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ClientToScreen(lv, &p);
        int col;
        int row = sqlc_w_row_at(lv, p, &col);
        if (row >= 0 && col >= 0 && e->cb.double_clicked) queue(e, CALL_DOUBLE, row, col, NULL, 0, 0);
        return r;
    }
    case WM_CONTEXTMENU: {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int row, col = 0;
        if (p.x == -1 && p.y == -1) { /* keyboard: the focused row, first column */
            row = (int)SendMessageW(lv, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_FOCUSED);
            RECT r = { LVIR_BOUNDS, 0, 0, 0 };
            if (row < 0 || !SendMessageW(lv, LVM_GETITEMRECT, (WPARAM)row, (LPARAM)&r)) return 0;
            p.x = r.left;
            p.y = r.bottom;
            ClientToScreen(lv, &p);
        } else {
            row = sqlc_w_row_at(lv, p, &col);
        }
        show_menu(e, p, row, col);
        return 0;
    }
    case WM_COMMAND:
        if ((HWND)lp == e->field && e->field && HIWORD(wp) == EN_CHANGE) {
            calendar_from_field(e); /* typing moves the calendar */
            return 0;
        }
        break;
    case WM_VSCROLL: case WM_HSCROLL: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: case WM_SIZE: {
        LRESULT r = DefSubclassProc(lv, msg, wp, lp);
        place_field(e); /* keep the editor over its cell */
        return r;
    }
    case WM_NOTIFY: {
        /* a column was resized: move the editor with it */
        LRESULT r = DefSubclassProc(lv, msg, wp, lp);
        const NMHDR *nm = (const NMHDR *)lp;
        if (nm && (nm->code == HDN_ITEMCHANGEDW || nm->code == HDN_ENDTRACKW)) place_field(e);
        return r;
    }
    case WM_NCDESTROY:
        RemoveWindowSubclass(lv, grid_proc, id);
        e->dead = 1;
        if (e->popup) DestroyWindow(e->popup); /* owned by the window, not the list view */
        e->popup = e->cal = e->clock = NULL;
        e->field = NULL; /* destroyed with the list view */
        unref(e);
        break;
    }
    return DefSubclassProc(lv, msg, wp, lp);
}

static grid_editor *editor_of(uiTable *t) {
    DWORD_PTR ref = 0;
    HWND lv = (HWND)uiControlHandle(uiControl(t));
    return GetWindowSubclass(lv, grid_proc, GRID_SUBCLASS_ID, &ref) ? (grid_editor *)ref : NULL;
}

/* ---------- C API ---------- */

void sqlc_grid_enable_editing(uiTable *t, const grid_edit_callbacks *cb) {
    HWND lv = (HWND)uiControlHandle(uiControl(t));
    grid_editor *e = calloc(1, sizeof *e);
    if (!e) return;
    e->lv = lv;
    e->cb = *cb;
    e->refs = 1;
    if (!SetWindowSubclass(lv, grid_proc, GRID_SUBCLASS_ID, (DWORD_PTR)e)) free(e);
}

void sqlc_grid_begin_edit(uiTable *t, int row, int col, const char *text,
                          const char *placeholder, grid_editor_kind kind) {
    grid_editor *e = editor_of(t);
    if (e && !e->dead) begin(e, row, col, text, placeholder, kind);
}

void sqlc_grid_close_editor(uiTable *t) {
    grid_editor *e = editor_of(t);
    if (!e) return;
    e->finishing = 1;
    close_quietly(e);
    e->finishing = 0;
}

void sqlc_grid_detach(uiTable *t) {
    grid_editor *e = editor_of(t);
    if (!e) return;
    e->dead = 1; /* drop callbacks already queued and to come */
    e->finishing = 1;
    close_quietly(e);
    e->finishing = 0;
    EndMenu();
}

int sqlc_grid_editing(uiTable *t, int *row, int *col, char **text) {
    grid_editor *e = editor_of(t);
    if (!e || !e->field) return 0;
    if (row) *row = e->row;
    if (col) *col = e->col;
    if (text) *text = field_text(e);
    return 1;
}

int sqlc_grid_editor_has_calendar(uiTable *t) {
    grid_editor *e = editor_of(t);
    return e && e->field && e->popup && e->cal;
}

void sqlc_grid_scroll_to_row(uiTable *t, int row) {
    HWND lv = (HWND)uiControlHandle(uiControl(t));
    if (row >= 0 && row < (int)SendMessageW(lv, LVM_GETITEMCOUNT, 0, 0))
        SendMessageW(lv, LVM_ENSUREVISIBLE, (WPARAM)row, FALSE);
}

void sqlc_grid_test_finish(uiTable *t, const char *text, grid_end how) {
    grid_editor *e = editor_of(t);
    if (!e || !e->field) return;
    WCHAR *w = sqlc_w_wide(text);
    SetWindowTextW(e->field, w ? w : L"");
    free(w);
    finish(e, how);
}

void sqlc_grid_test_pick_date(uiTable *t, const char *value) {
    grid_editor *e = editor_of(t);
    int y, mo, d, h, mi, sec;
    if (!e || !e->cal || !row_edit_calendar_value(value, &y, &mo, &d, &h, &mi, &sec)) return;
    SYSTEMTIME st = { (WORD)y, (WORD)mo, 0, (WORD)d, (WORD)h, (WORD)mi, (WORD)sec, 0 };
    e->syncing = 1;
    SendMessageW(e->cal, MCM_SETCURSEL, 0, (LPARAM)&st);
    if (e->clock) SendMessageW(e->clock, DTM_SETSYSTEMTIME, GDT_VALID, (LPARAM)&st);
    e->syncing = 0;
    field_from_calendar(e);
}
