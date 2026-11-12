/* Windows version of segmented.h: push-like radio buttons side by side in
 * a libui container, one pressed at a time. */

#include <windows.h>
#include <windowsx.h>
#include <stdlib.h>

#include "ui.h"
#include "ui_windows.h"
#include "segmented.h"
#include "win_util.h"

#define SqlcSegmentedSignature 0x53717367 /* "Sqsg" */
#define SqlcSegmented(this) ((SqlcSegmented *) (this))

#define MAX_SEGMENTS 8
#define BUTTON_HEIGHT_DLU 14 /* Windows' push button height */
#define BUTTON_PAD_DLU 10    /* text padding inside each segment */

typedef struct SqlcSegmented {
    uiWindowsControl c;
    HWND hwnd;                  /* the container */
    HWND buttons[MAX_SEGMENTS];
    int n;
    int selected;
    sqlc_segmented_fn onChanged;
    void *onChangedData;
} SqlcSegmented;

static void check_only(SqlcSegmented *s, int index) {
    s->selected = index;
    for (int i = 0; i < s->n; i++)
        SendMessageW(s->buttons[i], BM_SETCHECK, i == index ? BST_CHECKED : BST_UNCHECKED, 0);
}

static BOOL on_command(uiControl *c, HWND clicked, WORD code, LRESULT *result) {
    SqlcSegmented *s = SqlcSegmented(c);
    if (code != BN_CLICKED) return FALSE;
    *result = 0;
    for (int i = 0; i < s->n; i++) {
        if (s->buttons[i] != clicked) continue;
        if (i == s->selected) { check_only(s, i); return TRUE; } /* stays pressed */
        check_only(s, i);
        if (s->onChanged) s->onChanged(uiControl(s), i, s->onChangedData); /* last */
        return TRUE;
    }
    return FALSE;
}

static void SqlcSegmentedDestroy(uiControl *c) {
    SqlcSegmented *s = SqlcSegmented(c);
    for (int i = 0; i < s->n; i++) {
        uiWindowsUnregisterWM_COMMANDHandler(s->buttons[i]);
        uiWindowsEnsureDestroyWindow(s->buttons[i]);
    }
    uiWindowsEnsureDestroyWindow(s->hwnd);
    uiFreeControl(c);
}

/* libui's default implementations ignore some of their parameters. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
uiWindowsControlDefaultHandle(SqlcSegmented)
uiWindowsControlDefaultParent(SqlcSegmented)
uiWindowsControlDefaultSetParent(SqlcSegmented)
uiWindowsControlDefaultToplevel(SqlcSegmented)
uiWindowsControlDefaultVisible(SqlcSegmented)
uiWindowsControlDefaultShow(SqlcSegmented)
uiWindowsControlDefaultHide(SqlcSegmented)
uiWindowsControlDefaultEnabled(SqlcSegmented)
uiWindowsControlDefaultEnable(SqlcSegmented)
uiWindowsControlDefaultDisable(SqlcSegmented)
uiWindowsControlDefaultSetParentHWND(SqlcSegmented)
uiWindowsControlDefaultMinimumSizeChanged(SqlcSegmented)
uiWindowsControlDefaultLayoutRect(SqlcSegmented)
uiWindowsControlDefaultAssignControlIDZOrder(SqlcSegmented)
uiWindowsControlDefaultChildVisibilityChanged(SqlcSegmented)
#pragma GCC diagnostic pop

static void SqlcSegmentedSyncEnableState(uiWindowsControl *c, int enabled) {
    SqlcSegmented *s = SqlcSegmented(c);
    if (uiWindowsShouldStopSyncEnableState(c, enabled)) return;
    EnableWindow(s->hwnd, enabled);
    for (int i = 0; i < s->n; i++) EnableWindow(s->buttons[i], enabled);
}

/* Width of segment i: its text plus padding on both sides. */
static int segment_width(SqlcSegmented *s, int i) {
    uiWindowsSizing sz;
    int pad = BUTTON_PAD_DLU, y = 0;
    uiWindowsGetSizing(s->buttons[i], &sz);
    uiWindowsSizingDlgUnitsToPixels(&sz, &pad, &y);
    return uiWindowsWindowTextWidth(s->buttons[i]) + 2 * pad;
}

static void SqlcSegmentedMinimumSize(uiWindowsControl *c, int *width, int *height) {
    SqlcSegmented *s = SqlcSegmented(c);
    *width = *height = 0;
    if (s->n == 0) return;
    for (int i = 0; i < s->n; i++) *width += segment_width(s, i);
    uiWindowsSizing sz;
    int y = BUTTON_HEIGHT_DLU;
    uiWindowsGetSizing(s->buttons[0], &sz);
    uiWindowsSizingDlgUnitsToPixels(&sz, NULL, &y);
    *height = y;
}

/* Segments from the left at their natural widths (like the macOS control,
 * which doesn't stretch). */
static void relayout(uiWindowsControl *c) {
    SqlcSegmented *s = SqlcSegmented(c);
    RECT r;
    uiWindowsEnsureGetClientRect(s->hwnd, &r);
    int x = r.left;
    for (int i = 0; i < s->n; i++) {
        int w = segment_width(s, i);
        uiWindowsEnsureMoveWindowDuringResize(s->buttons[i], x, r.top, w, r.bottom - r.top);
        x += w;
    }
}

uiControl *sqlc_new_segmented(const char *const *labels, int n) {
    if (!labels || n < 1) return NULL;
    if (n > MAX_SEGMENTS) n = MAX_SEGMENTS;
    SqlcSegmented *s;
    uiWindowsNewControl(SqlcSegmented, s);
    s->hwnd = uiWindowsMakeContainer(uiWindowsControl(s), relayout);
    LONG_PTR id = 100;
    HWND after = NULL;
    for (int i = 0; i < n; i++) {
        WCHAR *w = sqlc_w_wide(labels[i]);
        HWND b = uiWindowsEnsureCreateControlHWND(0, L"button", w ? w : L"",
                                                  BS_RADIOBUTTON | BS_PUSHLIKE | (i == 0 ? WS_GROUP | WS_TABSTOP : 0),
                                                  GetModuleHandleW(NULL), NULL, TRUE);
        free(w);
        uiWindowsEnsureSetParentHWND(b, s->hwnd);
        uiWindowsRegisterWM_COMMANDHandler(b, on_command, uiControl(s));
        uiWindowsEnsureAssignControlIDZOrder(b, &id, &after);
        s->buttons[s->n++] = b;
    }
    check_only(s, 0);
    return uiControl(s);
}

int sqlc_segmented_selected(uiControl *seg) {
    return SqlcSegmented(seg)->selected;
}

void sqlc_segmented_set_selected(uiControl *seg, int index) {
    SqlcSegmented *s = SqlcSegmented(seg);
    if (index < 0 || index >= s->n) return;
    check_only(s, index); /* no callback */
}

void sqlc_segmented_on_changed(uiControl *seg, sqlc_segmented_fn f, void *data) {
    SqlcSegmented *s = SqlcSegmented(seg);
    s->onChanged = f;
    s->onChangedData = data;
}
