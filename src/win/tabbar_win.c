/* Windows version of tabbar.h: the window's tab strip, drawn with GDI in
 * system colors in a libui container (left-aligned tabs with a close x, a
 * "+" after the last one, middle click closes, the full title as a
 * tooltip). Sizes match tabbar.m. */

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"
#include "ui_windows.h"
#include "tabbar.h"
#include "win_util.h"

#define SqlcTabBarSignature 0x53717462 /* "Sqtb" */
#define SqlcTabBar(this) ((SqlcTabBar *) (this))

/* points (96-dpi pixels) */
#define BAR_HEIGHT 30
#define ITEM_MAX 220  /* widest a tab gets */
#define ITEM_MIN 100  /* narrowest; beyond that the strip clips */
#define PLUS_WIDTH 30
#define CLOSE_SIZE 16
#define PAD 10

typedef enum { HIT_NONE, HIT_ITEM, HIT_CLOSE, HIT_PLUS } hit_kind;

typedef struct SqlcTabBar {
    uiWindowsControl c;
    HWND hwnd;              /* the container we paint */
    sqlc_tabbar_callbacks cb;
    WCHAR **titles;
    int n, cap;
    int selected;
    hit_kind press_kind;    /* left button down on a close x or "+" */
    int press_index;
    int press_inside;
    int middle_index;       /* middle button down on this tab, or -1 */
    HWND tips;              /* tooltip control: one tool per tab */
    int ntips;              /* tools it has */
} SqlcTabBar;

static int px(SqlcTabBar *b, int points) { return sqlc_w_scale(b->hwnd, points); }

/* Width of each tab: they shrink as tabs are added, between the limits. */
static int item_width(SqlcTabBar *b) {
    if (b->n == 0) return px(b, ITEM_MAX);
    RECT r;
    GetClientRect(b->hwnd, &r);
    int w = (r.right - px(b, PLUS_WIDTH)) / b->n;
    if (w > px(b, ITEM_MAX)) w = px(b, ITEM_MAX);
    if (w < px(b, ITEM_MIN)) w = px(b, ITEM_MIN);
    return w;
}

static RECT item_rect(SqlcTabBar *b, int i) {
    int w = item_width(b);
    RECT r = { w * i, 0, w * (i + 1), px(b, BAR_HEIGHT) - 1 };
    return r;
}

static RECT close_rect(SqlcTabBar *b, int i) {
    RECT r = item_rect(b, i);
    int cs = px(b, CLOSE_SIZE);
    int left = r.right - px(b, 8) - cs, top = (r.bottom - r.top - cs) / 2;
    RECT c = { left, top, left + cs, top + cs };
    return c;
}

static RECT plus_rect(SqlcTabBar *b) {
    int x = item_width(b) * b->n;
    RECT r = { x, 0, x + px(b, PLUS_WIDTH), px(b, BAR_HEIGHT) - 1 };
    return r;
}

static hit_kind hit_at(SqlcTabBar *b, POINT p, int *index) {
    *index = -1;
    RECT plus = plus_rect(b);
    if (PtInRect(&plus, p)) return HIT_PLUS;
    for (int i = 0; i < b->n; i++) {
        RECT r = item_rect(b, i);
        if (!PtInRect(&r, p)) continue;
        *index = i;
        RECT c = close_rect(b, i);
        InflateRect(&c, 2, 2);
        return PtInRect(&c, p) ? HIT_CLOSE : HIT_ITEM;
    }
    return HIT_NONE;
}

/* One tooltip tool per tab, over its rectangle; the text is asked for
 * when shown (TTN_GETDISPINFO), so titles can change. */
static void rebuild_tips(SqlcTabBar *b) {
    if (!b->tips) return;
    TOOLINFOW ti;
    memset(&ti, 0, sizeof ti);
    ti.cbSize = sizeof ti;
    ti.hwnd = b->hwnd;
    for (int i = 0; i < b->ntips; i++) {
        ti.uId = (UINT_PTR)i;
        SendMessageW(b->tips, TTM_DELTOOLW, 0, (LPARAM)&ti);
    }
    b->ntips = 0;
    for (int i = 0; i < b->n; i++) {
        ti.uFlags = TTF_SUBCLASS;
        ti.uId = (UINT_PTR)i;
        ti.rect = item_rect(b, i);
        ti.lpszText = LPSTR_TEXTCALLBACKW;
        if (SendMessageW(b->tips, TTM_ADDTOOLW, 0, (LPARAM)&ti)) b->ntips = i + 1;
    }
}

static void changed(SqlcTabBar *b) {
    rebuild_tips(b);
    b->press_kind = HIT_NONE;
    b->press_index = -1;
    b->middle_index = -1;
    InvalidateRect(b->hwnd, NULL, FALSE);
}

/* ---------- drawing ---------- */

static void line(HDC dc, int x0, int y0, int x1, int y1) {
    MoveToEx(dc, x0, y0, NULL);
    LineTo(dc, x1, y1);
}

static void draw_cross(HDC dc, RECT r, int half, COLORREF color, int width) {
    HPEN pen = CreatePen(PS_SOLID, width, color), old = SelectObject(dc, pen);
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    line(dc, cx - half, cy - half, cx + half + 1, cy + half + 1);
    line(dc, cx + half, cy - half, cx - half - 1, cy + half + 1);
    SelectObject(dc, old);
    DeleteObject(pen);
}

static void draw_plus(HDC dc, RECT r, int half, COLORREF color, int width) {
    HPEN pen = CreatePen(PS_SOLID, width, color), old = SelectObject(dc, pen);
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    line(dc, cx - half, cy, cx + half + 1, cy);
    line(dc, cx, cy - half, cx, cy + half + 1);
    SelectObject(dc, old);
    DeleteObject(pen);
}

static void fill(HDC dc, RECT r, int sys_color) {
    FillRect(dc, &r, GetSysColorBrush(sys_color));
}

static void paint(SqlcTabBar *b, HDC dc, RECT bounds) {
    fill(dc, bounds, COLOR_BTNFACE);
    SetBkMode(dc, TRANSPARENT);
    int pen_w = px(b, 1) > 1 ? px(b, 1) : 1;

    for (int i = 0; i < b->n; i++) {
        RECT r = item_rect(b, i);
        int sel = i == b->selected;
        if (sel) {
            fill(dc, r, COLOR_WINDOW);
            RECT accent = { r.left, r.bottom - px(b, 2), r.right, r.bottom };
            fill(dc, accent, COLOR_HIGHLIGHT);
        }
        RECT sep = { r.right - 1, r.top + px(b, 6), r.right, r.bottom - px(b, 6) };
        fill(dc, sep, COLOR_3DSHADOW);

        RECT close = close_rect(b, i);
        if (b->press_kind == HIT_CLOSE && b->press_index == i && b->press_inside) fill(dc, close, COLOR_3DLIGHT);
        draw_cross(dc, close, px(b, 3), GetSysColor(sel ? COLOR_WINDOWTEXT : COLOR_GRAYTEXT), pen_w);

        HFONT old = SelectObject(dc, sel ? sqlc_w_bold_font() : sqlc_w_font());
        SetTextColor(dc, GetSysColor(sel ? COLOR_WINDOWTEXT : COLOR_GRAYTEXT));
        RECT tr = { r.left + px(b, PAD), r.top, close.left - px(b, 4), r.bottom };
        if (tr.right > tr.left)
            DrawTextW(dc, b->titles[i], -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, old);
    }

    RECT plus = plus_rect(b);
    if (b->press_kind == HIT_PLUS && b->press_inside) {
        RECT in = plus;
        InflateRect(&in, -px(b, 3), -px(b, 4));
        fill(dc, in, COLOR_3DLIGHT);
    }
    draw_plus(dc, plus, px(b, 5), GetSysColor(COLOR_GRAYTEXT), pen_w);

    RECT bottom = { bounds.left, bounds.bottom - 1, bounds.right, bounds.bottom };
    fill(dc, bottom, COLOR_3DSHADOW);
}

/* ---------- mouse ---------- */

static void mouse_down(SqlcTabBar *b, POINT p) {
    int i;
    hit_kind k = hit_at(b, p, &i);
    if (k == HIT_ITEM) {
        if (i == b->selected) return;
        b->selected = i;
        InvalidateRect(b->hwnd, NULL, FALSE);
        if (b->cb.on_select) b->cb.on_select(uiControl(b), i, b->cb.data); /* last */
        return;
    }
    if (k == HIT_CLOSE || k == HIT_PLUS) {
        b->press_kind = k;
        b->press_index = i;
        b->press_inside = 1;
        SetCapture(b->hwnd);
        InvalidateRect(b->hwnd, NULL, FALSE);
    }
}

static void mouse_move(SqlcTabBar *b, POINT p) {
    if (b->press_kind == HIT_NONE) return;
    int i;
    hit_kind k = hit_at(b, p, &i);
    int inside = k == b->press_kind && i == b->press_index;
    if (inside != b->press_inside) {
        b->press_inside = inside;
        InvalidateRect(b->hwnd, NULL, FALSE);
    }
}

static void mouse_up(SqlcTabBar *b, POINT p) {
    if (b->press_kind == HIT_NONE) return;
    int i;
    hit_kind k = hit_at(b, p, &i);
    hit_kind pressed = b->press_kind;
    int index = b->press_index;
    b->press_kind = HIT_NONE;
    b->press_index = -1;
    if (GetCapture() == b->hwnd) ReleaseCapture();
    InvalidateRect(b->hwnd, NULL, FALSE);
    if (k != pressed || i != index) return; /* released elsewhere: nothing */
    /* last: the callback may change the strip */
    if (pressed == HIT_CLOSE && b->cb.on_close) b->cb.on_close(uiControl(b), index, b->cb.data);
    else if (pressed == HIT_PLUS && b->cb.on_new) b->cb.on_new(uiControl(b), b->cb.data);
}

static void middle_up(SqlcTabBar *b, POINT p) {
    int i;
    hit_kind k = hit_at(b, p, &i);
    int started = b->middle_index;
    b->middle_index = -1;
    if ((k != HIT_ITEM && k != HIT_CLOSE) || i != started) return;
    if (b->cb.on_close) b->cb.on_close(uiControl(b), i, b->cb.data); /* last */
}

static LRESULT CALLBACK bar_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    SqlcTabBar *b = (SqlcTabBar *)ref;
    POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r;
        GetClientRect(hwnd, &r);
        /* off screen, then one copy: no flicker */
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, r.right > 0 ? r.right : 1, r.bottom > 0 ? r.bottom : 1);
        HBITMAP old = SelectObject(mem, bmp);
        paint(b, mem, r);
        BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        InvalidateRect(hwnd, NULL, FALSE);
        rebuild_tips(b); /* tab widths depend on the width */
        break;
    case WM_NOTIFY: {
        NMTTDISPINFOW *di = (NMTTDISPINFOW *)lp;
        if (di->hdr.hwndFrom == b->tips && di->hdr.code == TTN_GETDISPINFOW) {
            int i = (int)di->hdr.idFrom;
            di->lpszText = i >= 0 && i < b->n ? b->titles[i] : (WCHAR *)L"";
            return 0;
        }
        break;
    }
    case WM_LBUTTONDOWN: mouse_down(b, p); return 0;
    case WM_MOUSEMOVE: mouse_move(b, p); return 0;
    case WM_LBUTTONUP: mouse_up(b, p); return 0;
    case WM_CAPTURECHANGED:
        if ((HWND)lp != hwnd && b->press_kind != HIT_NONE) changed(b);
        break;
    case WM_MBUTTONDOWN: {
        int i;
        hit_kind k = hit_at(b, p, &i);
        b->middle_index = k == HIT_ITEM || k == HIT_CLOSE ? i : -1;
        return 0;
    }
    case WM_MBUTTONUP: middle_up(b, p); return 0;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, bar_proc, id);
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

/* ---------- libui control ---------- */

static void SqlcTabBarDestroy(uiControl *c) {
    SqlcTabBar *b = SqlcTabBar(c);
    if (b->tips) DestroyWindow(b->tips);
    for (int i = 0; i < b->n; i++) free(b->titles[i]);
    free(b->titles);
    uiWindowsEnsureDestroyWindow(b->hwnd);
    uiFreeControl(c);
}

/* libui's default implementations ignore some of their parameters. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
uiWindowsControlAllDefaultsExceptDestroy(SqlcTabBar)
#pragma GCC diagnostic pop

static void SqlcTabBarMinimumSize(uiWindowsControl *c, int *width, int *height) {
    SqlcTabBar *b = SqlcTabBar(c);
    *width = 0;
    *height = px(b, BAR_HEIGHT);
}

static void on_resize(uiWindowsControl *c) { InvalidateRect(SqlcTabBar(c)->hwnd, NULL, FALSE); }

uiControl *sqlc_new_tabbar(const sqlc_tabbar_callbacks *cb) {
    SqlcTabBar *b;
    uiWindowsNewControl(SqlcTabBar, b);
    if (cb) b->cb = *cb;
    b->selected = -1;
    b->press_index = -1;
    b->middle_index = -1;
    b->hwnd = uiWindowsMakeContainer(uiWindowsControl(b), on_resize);
    SetWindowSubclass(b->hwnd, bar_proc, 0x5374, (DWORD_PTR)b);
    b->tips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                              CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                              b->hwnd, NULL, GetModuleHandleW(NULL), NULL);
    return uiControl(b);
}

int sqlc_tabbar_append(uiControl *bar, const char *title) {
    SqlcTabBar *b = SqlcTabBar(bar);
    if (b->n == b->cap) {
        int ncap = b->cap ? b->cap * 2 : 8;
        WCHAR **t = realloc(b->titles, (size_t)ncap * sizeof *t);
        if (!t) return -1;
        b->titles = t;
        b->cap = ncap;
    }
    WCHAR *w = sqlc_w_wide(title);
    if (!w) return -1;
    b->titles[b->n] = w;
    if (b->n == 0) b->selected = 0;
    b->n++;
    changed(b);
    return b->n - 1;
}

void sqlc_tabbar_remove(uiControl *bar, int index) {
    SqlcTabBar *b = SqlcTabBar(bar);
    if (index < 0 || index >= b->n) return;
    free(b->titles[index]);
    memmove(b->titles + index, b->titles + index + 1, (size_t)(b->n - index - 1) * sizeof *b->titles);
    b->n--;
    if (b->n == 0) b->selected = -1;
    else if (b->selected > index) b->selected--;
    else if (b->selected >= b->n) b->selected = b->n - 1;
    changed(b);
}

void sqlc_tabbar_set_title(uiControl *bar, int index, const char *title) {
    SqlcTabBar *b = SqlcTabBar(bar);
    if (index < 0 || index >= b->n) return;
    WCHAR *w = sqlc_w_wide(title);
    if (!w) return;
    free(b->titles[index]);
    b->titles[index] = w;
    InvalidateRect(b->hwnd, NULL, FALSE);
}

void sqlc_tabbar_set_selected(uiControl *bar, int index) {
    SqlcTabBar *b = SqlcTabBar(bar);
    if (index < 0 || index >= b->n) return;
    b->selected = index;
    InvalidateRect(b->hwnd, NULL, FALSE);
}

int sqlc_tabbar_selected(uiControl *bar) { return SqlcTabBar(bar)->selected; }

int sqlc_tabbar_count(uiControl *bar) { return SqlcTabBar(bar)->n; }
