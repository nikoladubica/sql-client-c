/* Windows version of splitview.h: two libui controls in a libui container
 * with a draggable divider between them (side by side or stacked), plus
 * the uiTable / uiWindow helpers declared next to it. Sizes in the API are
 * in points (96-dpi pixels), scaled for the screen here. */

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"
#include "ui_windows.h"
#include "splitview.h"
#include "win_util.h"

#define SqlcSplitSignature 0x53717370 /* "Sqsp" */
#define SqlcSplit(this) ((SqlcSplit *) (this))

#define MIN_MAIN_WIDTH 240
#define DIVIDER 7 /* points: the gap between the panes, all of it grabbable */

typedef struct SqlcSplit {
    uiWindowsControl c;
    HWND hwnd;                /* the container */
    uiControl *children[2];
    int across;               /* 1 = side by side */
    int wanted;               /* first pane's starting size (points); <= 0 = half */
    int min_first, min_second;/* points */
    int pos;                  /* first pane's size in pixels, once positioned */
    int positioned;
    char *save_key;           /* NULL = don't persist */
    int dragging, drag_offset;
} SqlcSplit;

static int px(SqlcSplit *s, int points) { return sqlc_w_scale(s->hwnd, points); }

static int to_points(SqlcSplit *s, int pixels) {
    int one = px(s, 96);
    return one > 0 ? MulDiv(pixels, 96, one) : pixels;
}

/* Minimum of pane i along the split direction: the given minimum, or the
 * child's own if that is larger. */
static int pane_min(SqlcSplit *s, int i) {
    int w = 0, h = 0;
    uiWindowsControlMinimumSize(uiWindowsControl(s->children[i]), &w, &h);
    int given = px(s, i == 0 ? s->min_first : s->min_second);
    int own = s->across ? w : h;
    return own > given ? own : given;
}

static int length(SqlcSplit *s) {
    RECT r;
    uiWindowsEnsureGetClientRect(s->hwnd, &r);
    return s->across ? r.right - r.left : r.bottom - r.top;
}

/* Room for both panes once the divider is taken out. */
static int pane_space(SqlcSplit *s) { return length(s) - px(s, DIVIDER); }

static int clamp_pos(SqlcSplit *s, int pos) {
    int max = pane_space(s) - pane_min(s, 1);
    if (pos > max) pos = max;
    if (pos < pane_min(s, 0)) pos = pane_min(s, 0);
    return pos;
}

static void SqlcSplitDestroy(uiControl *c) {
    SqlcSplit *s = SqlcSplit(c);
    for (int i = 0; i < 2; i++) {
        uiControlSetParent(s->children[i], NULL);
        uiControlDestroy(s->children[i]);
    }
    free(s->save_key);
    uiWindowsEnsureDestroyWindow(s->hwnd);
    uiFreeControl(c);
}

/* libui's default implementations ignore some of their parameters. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
uiWindowsControlDefaultHandle(SqlcSplit)
uiWindowsControlDefaultParent(SqlcSplit)
uiWindowsControlDefaultSetParent(SqlcSplit)
uiWindowsControlDefaultToplevel(SqlcSplit)
uiWindowsControlDefaultVisible(SqlcSplit)
uiWindowsControlDefaultShow(SqlcSplit)
uiWindowsControlDefaultHide(SqlcSplit)
uiWindowsControlDefaultEnabled(SqlcSplit)
uiWindowsControlDefaultEnable(SqlcSplit)
uiWindowsControlDefaultDisable(SqlcSplit)
uiWindowsControlDefaultSetParentHWND(SqlcSplit)
uiWindowsControlDefaultLayoutRect(SqlcSplit)
uiWindowsControlDefaultAssignControlIDZOrder(SqlcSplit)
#pragma GCC diagnostic pop

static void SqlcSplitSyncEnableState(uiWindowsControl *c, int enabled) {
    SqlcSplit *s = SqlcSplit(c);
    if (uiWindowsShouldStopSyncEnableState(c, enabled)) return;
    for (int i = 0; i < 2; i++)
        uiWindowsControlSyncEnableState(uiWindowsControl(s->children[i]), enabled);
}

static void SqlcSplitMinimumSize(uiWindowsControl *c, int *width, int *height) {
    SqlcSplit *s = SqlcSplit(c);
    int w[2], h[2];
    for (int i = 0; i < 2; i++)
        uiWindowsControlMinimumSize(uiWindowsControl(s->children[i]), &w[i], &h[i]);
    int along = pane_min(s, 0) + px(s, DIVIDER) + pane_min(s, 1);
    if (s->across) {
        *width = along;
        *height = h[0] > h[1] ? h[0] : h[1];
    } else {
        *width = w[0] > w[1] ? w[0] : w[1];
        *height = along;
    }
}

static void relayout(SqlcSplit *s) {
    RECT r;
    uiWindowsEnsureGetClientRect(s->hwnd, &r);
    int space = pane_space(s);
    if (space <= 0) return;
    if (!s->positioned) {
        /* place the divider on the first layout that has room for both
         * minimums, so a tiny first pass can't pin it to one edge */
        int want = s->wanted > 0 ? px(s, s->wanted) : space / 2;
        s->pos = clamp_pos(s, want);
        if (space >= pane_min(s, 0) + pane_min(s, 1)) s->positioned = 1;
    } else {
        s->pos = clamp_pos(s, s->pos); /* the first pane keeps its size */
    }
    int d = px(s, DIVIDER);
    HWND a = (HWND)uiControlHandle(s->children[0]), b = (HWND)uiControlHandle(s->children[1]);
    if (s->across) {
        uiWindowsEnsureMoveWindowDuringResize(a, r.left, r.top, s->pos, r.bottom - r.top);
        uiWindowsEnsureMoveWindowDuringResize(b, r.left + s->pos + d, r.top,
                                              r.right - r.left - s->pos - d, r.bottom - r.top);
    } else {
        uiWindowsEnsureMoveWindowDuringResize(a, r.left, r.top, r.right - r.left, s->pos);
        uiWindowsEnsureMoveWindowDuringResize(b, r.left, r.top + s->pos + d,
                                              r.right - r.left, r.bottom - r.top - s->pos - d);
    }
    InvalidateRect(s->hwnd, NULL, TRUE);
}

static void SqlcSplitMinimumSizeChanged(uiWindowsControl *c) {
    if (uiWindowsControlTooSmall(c)) {
        uiWindowsControlContinueMinimumSizeChanged(c);
        return;
    }
    relayout(SqlcSplit(c));
}

static void SqlcSplitChildVisibilityChanged(uiWindowsControl *c) {
    uiWindowsControlMinimumSizeChanged(c);
}

static void on_resize(uiWindowsControl *c) { relayout(SqlcSplit(c)); }

static void save(SqlcSplit *s) {
    if (s->positioned && s->save_key) sqlc_w_set_int(s->save_key, to_points(s, s->pos));
}

/* 1 when (x, y) in the container is on the divider. */
static int on_divider(SqlcSplit *s, int x, int y) {
    int at = s->across ? x : y;
    return at >= s->pos && at < s->pos + px(s, DIVIDER);
}

/* Mouse on the divider (drag to move it) and its 1-pixel line. */
static LRESULT CALLBACK split_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    SqlcSplit *s = (SqlcSplit *)ref;
    switch (msg) {
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            if (s->dragging || on_divider(s, p.x, p.y)) {
                SetCursor(LoadCursorW(NULL, s->across ? IDC_SIZEWE : IDC_SIZENS));
                return TRUE;
            }
        }
        break;
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (!on_divider(s, x, y)) break;
        s->dragging = 1;
        s->drag_offset = (s->across ? x : y) - s->pos;
        SetCapture(hwnd);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (s->dragging) {
            int at = s->across ? GET_X_LPARAM(lp) : GET_Y_LPARAM(lp);
            int pos = clamp_pos(s, at - s->drag_offset);
            if (pos != s->pos) {
                s->pos = pos;
                s->positioned = 1;
                relayout(s);
                UpdateWindow(GetAncestor(hwnd, GA_ROOT));
            }
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (s->dragging) {
            ReleaseCapture(); /* WM_CAPTURECHANGED ends the drag */
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        if (s->dragging) {
            s->dragging = 0;
            save(s);
        }
        break;
    case WM_PAINT: {
        LRESULT r = DefSubclassProc(hwnd, msg, wp, lp); /* the background */
        HDC dc = GetDC(hwnd);
        RECT c, line;
        GetClientRect(hwnd, &c);
        int mid = s->pos + px(s, DIVIDER) / 2;
        if (s->across) SetRect(&line, mid, c.top, mid + 1, c.bottom);
        else SetRect(&line, c.left, mid, c.right, mid + 1);
        FillRect(dc, &line, GetSysColorBrush(COLOR_3DLIGHT));
        ReleaseDC(hwnd, dc);
        return r;
    }
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, split_proc, id);
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static uiControl *new_split(int across, uiControl *first, uiControl *second,
                            int initial, int min_first, int min_second, const char *save_key) {
    SqlcSplit *s;
    uiWindowsNewControl(SqlcSplit, s);
    s->across = across;
    s->wanted = initial;
    s->min_first = min_first;
    s->min_second = min_second;
    s->hwnd = uiWindowsMakeContainer(uiWindowsControl(s), on_resize);
    if (save_key) {
        s->save_key = strdup(save_key);
        int saved;
        if (sqlc_w_get_int(save_key, &saved) && saved >= min_first) s->wanted = saved;
    }
    SetWindowSubclass(s->hwnd, split_proc, 0x5370, (DWORD_PTR)s);
    LONG_PTR cid = 100;
    HWND after = NULL;
    for (int i = 0; i < 2; i++) {
        uiControl *child = i == 0 ? first : second;
        s->children[i] = child;
        uiControlSetParent(child, uiControl(s));
        uiWindowsControlSetParentHWND(uiWindowsControl(child), s->hwnd);
        uiWindowsControlAssignControlIDZOrder(uiWindowsControl(child), &cid, &after);
    }
    return uiControl(s);
}

uiControl *sqlc_new_sidebar_split(uiControl *side, uiControl *main,
                                  int initial_width, int min_width,
                                  const char *save_key) {
    return new_split(1, side, main, initial_width, min_width, MIN_MAIN_WIDTH, save_key);
}

uiControl *sqlc_new_stacked_split(uiControl *top, uiControl *bottom,
                                  int initial_height, int min_top, int min_bottom,
                                  const char *save_key) {
    return new_split(0, top, bottom, initial_height, min_top, min_bottom, save_key);
}

int sqlc_split_position(uiControl *split) {
    SqlcSplit *s = SqlcSplit(split);
    if (!s->positioned) return s->wanted > 0 ? s->wanted : 0;
    return to_points(s, s->pos);
}

void sqlc_split_set_position(uiControl *split, int size) {
    SqlcSplit *s = SqlcSplit(split);
    if (pane_space(s) <= 0) { s->wanted = size; return; } /* not laid out yet */
    s->pos = clamp_pos(s, px(s, size));
    s->positioned = 1;
    relayout(s);
}

/* ---------- uiTable / uiWindow helpers ---------- */

/* Keep the list view's last column filling the rest of its width. */
static void fit_last_column(HWND lv) {
    HWND header = (HWND)SendMessageW(lv, LVM_GETHEADER, 0, 0);
    int n = header ? Header_GetItemCount(header) : 0;
    if (n > 0) SendMessageW(lv, LVM_SETCOLUMNWIDTH, (WPARAM)(n - 1), LVSCW_AUTOSIZE_USEHEADER);
}

static LRESULT CALLBACK fill_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    (void)ref;
    if (msg == WM_SIZE) {
        LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
        fit_last_column(hwnd);
        return r;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, fill_proc, id);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void sqlc_table_fill_width(uiTable *t) {
    HWND lv = (HWND)uiControlHandle(uiControl(t));
    SetWindowSubclass(lv, fill_proc, 0x5366, 0);
    fit_last_column(lv);
}

void sqlc_window_layout_now(uiWindow *w, int redraw) {
    uiWindowsControlMinimumSizeChanged(uiWindowsControl(w)); /* lays the window out */
    if (redraw)
        RedrawWindow((HWND)uiControlHandle(uiControl(w)), NULL, NULL,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void sqlc_table_set_enabled(uiTable *t, int enabled) {
    /* a disabled list view takes no clicks and draws greyed */
    EnableWindow((HWND)uiControlHandle(uiControl(t)), enabled ? TRUE : FALSE);
}
