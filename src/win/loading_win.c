/* Windows version of loading.h: a marquee progress bar (Windows' "busy"
 * indicator) with a centred, wrapping line of text under it, in a libui
 * container. */

#include <windows.h>
#include <commctrl.h>
#include <stdlib.h>

#include "ui.h"
#include "ui_windows.h"
#include "loading.h"
#include "win_util.h"

#define SqlcLoadingSignature 0x53716c64 /* "Sqld" */
#define SqlcLoading(this) ((SqlcLoading *) (this))

#define BAR_WIDTH 160  /* 96-dpi pixels */
#define BAR_HEIGHT 6
#define GAP 10         /* between the bar and the text */
#define MIN_LINES 3    /* the text area is at least this many lines high */

typedef struct SqlcLoading {
    uiWindowsControl c;
    HWND hwnd;   /* the container */
    HWND bar;    /* msctls_progress32, PBS_MARQUEE */
    HWND text;   /* static, centred, wraps */
    int running;
    int text_height; /* last measured, for MinimumSize */
} SqlcLoading;

static void SqlcLoadingDestroy(uiControl *c) {
    SqlcLoading *l = SqlcLoading(c);
    SendMessageW(l->bar, PBM_SETMARQUEE, FALSE, 0);
    uiWindowsEnsureDestroyWindow(l->bar);
    uiWindowsEnsureDestroyWindow(l->text);
    uiWindowsEnsureDestroyWindow(l->hwnd);
    uiFreeControl(c);
}

/* libui's default implementations ignore some of their parameters. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
uiWindowsControlDefaultHandle(SqlcLoading)
uiWindowsControlDefaultParent(SqlcLoading)
uiWindowsControlDefaultSetParent(SqlcLoading)
uiWindowsControlDefaultToplevel(SqlcLoading)
uiWindowsControlDefaultVisible(SqlcLoading)
uiWindowsControlDefaultShow(SqlcLoading)
uiWindowsControlDefaultHide(SqlcLoading)
uiWindowsControlDefaultEnabled(SqlcLoading)
uiWindowsControlDefaultEnable(SqlcLoading)
uiWindowsControlDefaultDisable(SqlcLoading)
uiWindowsControlDefaultSyncEnableState(SqlcLoading)
uiWindowsControlDefaultSetParentHWND(SqlcLoading)
uiWindowsControlDefaultMinimumSizeChanged(SqlcLoading)
uiWindowsControlDefaultLayoutRect(SqlcLoading)
uiWindowsControlDefaultAssignControlIDZOrder(SqlcLoading)
uiWindowsControlDefaultChildVisibilityChanged(SqlcLoading)
#pragma GCC diagnostic pop

/* Height of the text wrapped at width, at least MIN_LINES lines. */
static int measure_text(SqlcLoading *l, int width) {
    HDC dc = GetDC(l->text);
    HFONT old = SelectObject(dc, (HFONT)SendMessageW(l->text, WM_GETFONT, 0, 0));
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    int h = tm.tmHeight * MIN_LINES;
    int len = GetWindowTextLengthW(l->text);
    if (len > 0 && width > 0) {
        WCHAR *buf = malloc(((size_t)len + 1) * sizeof *buf);
        if (buf) {
            GetWindowTextW(l->text, buf, len + 1);
            RECT r = { 0, 0, width, 0 };
            DrawTextW(dc, buf, -1, &r, DT_CALCRECT | DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
            if (r.bottom > h) h = r.bottom;
            free(buf);
        }
    }
    SelectObject(dc, old);
    ReleaseDC(l->text, dc);
    return h;
}

static void SqlcLoadingMinimumSize(uiWindowsControl *c, int *width, int *height) {
    SqlcLoading *l = SqlcLoading(c);
    if (l->text_height == 0) l->text_height = measure_text(l, 0);
    *width = sqlc_w_scale(l->hwnd, BAR_WIDTH / 4);
    *height = sqlc_w_scale(l->hwnd, BAR_HEIGHT + GAP) + l->text_height;
}

static void relayout(uiWindowsControl *c) {
    SqlcLoading *l = SqlcLoading(c);
    RECT r;
    uiWindowsEnsureGetClientRect(l->hwnd, &r);
    int w = r.right - r.left;
    int bw = sqlc_w_scale(l->hwnd, BAR_WIDTH), bh = sqlc_w_scale(l->hwnd, BAR_HEIGHT);
    if (bw > w) bw = w;
    uiWindowsEnsureMoveWindowDuringResize(l->bar, r.left + (w - bw) / 2, r.top, bw, bh);
    int top = r.top + bh + sqlc_w_scale(l->hwnd, GAP);
    uiWindowsEnsureMoveWindowDuringResize(l->text, r.left, top, w, r.bottom - top);
    /* the text wraps differently at this width: ask for a new height */
    int h = measure_text(l, w);
    if (h != l->text_height) {
        l->text_height = h;
        uiWindowsControlMinimumSizeChanged(c);
    }
}

uiControl *sqlc_new_loading(void) {
    SqlcLoading *l;
    uiWindowsNewControl(SqlcLoading, l);
    l->hwnd = uiWindowsMakeContainer(uiWindowsControl(l), relayout);
    l->bar = uiWindowsEnsureCreateControlHWND(0, PROGRESS_CLASSW, L"", PBS_MARQUEE,
                                              GetModuleHandleW(NULL), NULL, FALSE);
    uiWindowsEnsureSetParentHWND(l->bar, l->hwnd);
    l->text = uiWindowsEnsureCreateControlHWND(0, L"static", L"", SS_CENTER | SS_NOPREFIX,
                                               GetModuleHandleW(NULL), NULL, TRUE);
    uiWindowsEnsureSetParentHWND(l->text, l->hwnd);
    ShowWindow(l->bar, SW_HIDE);
    ShowWindow(l->text, SW_HIDE);
    return uiControl(l);
}

void sqlc_loading_start(uiControl *c, const char *text) {
    SqlcLoading *l = SqlcLoading(c);
    WCHAR *w = sqlc_w_wide(text);
    SetWindowTextW(l->text, w ? w : L"");
    free(w);
    ShowWindow(l->text, SW_SHOWNA);
    ShowWindow(l->bar, SW_SHOWNA);
    SendMessageW(l->bar, PBM_SETMARQUEE, TRUE, 30);
    l->running = 1;
    relayout(uiWindowsControl(l)); /* re-measures the text */
}

void sqlc_loading_stop(uiControl *c) {
    SqlcLoading *l = SqlcLoading(c);
    SendMessageW(l->bar, PBM_SETMARQUEE, FALSE, 0);
    ShowWindow(l->bar, SW_HIDE);
    SetWindowTextW(l->text, L""); /* back to the minimum height */
    ShowWindow(l->text, SW_HIDE);
    l->running = 0;
    relayout(uiWindowsControl(l));
}
