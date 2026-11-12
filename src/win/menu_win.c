/* Windows version of macos_menu.h: keyboard shortcuts for the menu items
 * gui.c adds to libui's menus (Ctrl+Enter, Ctrl+T, Ctrl+W), alerts, a fixed
 * control width, Return in an entry and the end-of-job notice. */

#include <windows.h>
#include <commctrl.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"
#include "ui_windows.h"
#include "macos_menu.h"
#include "win_util.h"

/* ---------- shortcuts ---------- */

static sqlc_menu_callbacks callbacks;
static HHOOK key_hook;

static void call_run(void *d) { (void)d; if (callbacks.run) callbacks.run(); }
static void call_new_tab(void *d) { (void)d; if (callbacks.new_tab) callbacks.new_tab(); }
static void call_close_tab(void *d) { (void)d; if (callbacks.close_tab) callbacks.close_tab(); }

/* 1 when hwnd is in a libui window (not a dialog or a menu). */
static int in_app_window(HWND hwnd) {
    HWND root = hwnd ? GetAncestor(hwnd, GA_ROOT) : NULL;
    WCHAR cls[64];
    return root && GetClassNameW(root, cls, 64) && wcscmp(cls, L"libui_uiWindowClass") == 0;
}

/* Looks at every message the UI thread takes from its queue: Ctrl+Return,
 * Ctrl+T and Ctrl+W become the menu actions (run after this message, like
 * a menu click), and the key itself is dropped, so the SQL editor doesn't
 * also get a new line. */
static LRESULT CALLBACK key_proc(int code, WPARAM wp, LPARAM lp) {
    MSG *m = (MSG *)lp;
    if (code == HC_ACTION && wp == PM_REMOVE && m->message == WM_KEYDOWN &&
        GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_MENU) >= 0 && GetKeyState(VK_SHIFT) >= 0 &&
        in_app_window(m->hwnd)) {
        void (*f)(void *) = NULL;
        if (m->wParam == VK_RETURN) f = call_run;
        else if (m->wParam == 'T') f = call_new_tab;
        else if (m->wParam == 'W') f = call_close_tab;
        if (f) {
            m->message = WM_NULL;
            sqlc_w_post(f, NULL);
        }
    }
    return CallNextHookEx(key_hook, code, wp, lp);
}

void sqlc_install_menus(const sqlc_menu_callbacks *cb) {
    if (cb) callbacks = *cb;
    /* The items themselves are libui menu items made in gui.c before the
     * window (libui can't add menus later on Windows); Edit shortcuts are
     * built into the native text fields. */
    if (!key_hook) key_hook = SetWindowsHookExW(WH_GETMESSAGE, key_proc, NULL, GetCurrentThreadId());
}

/* ---------- alerts ---------- */

/* A task dialog with the given buttons (ids 100, 101, ...); returns the id
 * chosen, or cancel_id when closed with Escape / the close box (only
 * possible when allow_cancel). */
static int task_dialog(const char *title, const char *message, PCWSTR icon,
                       const char *const *labels, int n, int default_id, int allow_cancel) {
    WCHAR *wt = sqlc_w_wide(title), *wm = sqlc_w_wide(message);
    TASKDIALOG_BUTTON buttons[4];
    WCHAR *wl[4] = { 0 };
    if (n > 4) n = 4;
    for (int i = 0; i < n; i++) {
        wl[i] = sqlc_w_wide(labels[i]);
        buttons[i].nButtonID = 100 + i;
        buttons[i].pszButtonText = wl[i] ? wl[i] : L"OK";
    }
    TASKDIALOGCONFIG c;
    memset(&c, 0, sizeof c);
    c.cbSize = sizeof c;
    c.hwndParent = sqlc_w_owner();
    c.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | (allow_cancel ? TDF_ALLOW_DIALOG_CANCELLATION : 0);
    c.pszWindowTitle = L"sqlc";
    c.pszMainIcon = icon;
    c.pszMainInstruction = wt ? wt : L"";
    c.pszContent = wm && *wm ? wm : NULL;
    c.cButtons = (UINT)n;
    c.pButtons = buttons;
    c.nDefaultButton = default_id;
    int chosen = -1;
    if (FAILED(TaskDialogIndirect(&c, &chosen, NULL, NULL))) chosen = -1;
    free(wt);
    free(wm);
    for (int i = 0; i < n; i++) free(wl[i]);
    return chosen;
}

int sqlc_confirm(const char *title, const char *message, const char *ok_label) {
    const char *labels[] = { ok_label ? ok_label : "OK", "Cancel" };
    return task_dialog(title, message, NULL, labels, 2, 100, 1) == 100;
}

/* Used by table_menu_win.c for the "are you sure?" alerts. */
int sqlc_w_task_dialog(const char *title, const char *message, int warning,
                       const char *const *labels, int n, int default_id, int allow_cancel) {
    return task_dialog(title, message, warning ? TD_WARNING_ICON : NULL, labels, n, default_id, allow_cancel);
}

/* ---------- controls ---------- */

void sqlc_button_set_small(uiButton *b) {
    (void)b; /* Windows has one push-button size */
}

/* A fixed width: replace the control's MinimumSize with one that reports
 * the width (scaled for the screen) and the control's own height. */
typedef struct {
    uiWindowsControl *c;
    void (*orig)(uiWindowsControl *, int *, int *);
    double width;
} fixed_width;

static fixed_width fixed[16];
static size_t nfixed;

static void fixed_min_size(uiWindowsControl *c, int *w, int *h) {
    for (size_t i = 0; i < nfixed; i++) {
        if (fixed[i].c != c) continue;
        fixed[i].orig(c, w, h);
        *w = sqlc_w_scale((HWND)uiControlHandle(uiControl(c)), (int)fixed[i].width);
        return;
    }
    *w = *h = 0;
}

void sqlc_control_set_width(uiControl *c, double width) {
    uiWindowsControl *wc = uiWindowsControl(c);
    for (size_t i = 0; i < nfixed; i++)
        if (fixed[i].c == wc) { fixed[i].width = width; uiWindowsControlMinimumSizeChanged(wc); return; }
    if (nfixed == sizeof fixed / sizeof fixed[0]) return;
    fixed[nfixed].c = wc;
    fixed[nfixed].orig = wc->MinimumSize;
    fixed[nfixed].width = width;
    nfixed++;
    wc->MinimumSize = fixed_min_size;
    uiWindowsControlMinimumSizeChanged(wc);
}

/* ---------- Return in an entry ---------- */

typedef struct {
    void (*fn)(void *);
    void *data;
} return_target;

static void call_return(void *p) {
    return_target *r = p;
    r->fn(r->data);
}

static LRESULT CALLBACK entry_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    return_target *r = (return_target *)ref;
    switch (msg) {
    case WM_GETDLGCODE:
        if (lp && ((MSG *)lp)->message == WM_KEYDOWN && ((MSG *)lp)->wParam == VK_RETURN)
            return DLGC_WANTALLKEYS;
        break;
    case WM_KEYDOWN:
        if (wp == VK_RETURN && GetKeyState(VK_CONTROL) >= 0) {
            if (r->fn) sqlc_w_post(call_return, r);
            return 0;
        }
        break;
    case WM_CHAR:
        if (wp == '\r' || wp == '\n') return 0; /* no beep */
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, entry_proc, id);
        free(r);
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void sqlc_entry_on_return(uiEntry *e, void (*f)(void *data), void *data) {
    HWND hwnd = (HWND)uiControlHandle(uiControl(e));
    return_target *r = calloc(1, sizeof *r);
    if (!r) return;
    r->fn = f;
    r->data = data;
    if (!SetWindowSubclass(hwnd, entry_proc, 0x5371, (DWORD_PTR)r)) free(r);
}

/* ---------- end of a long job ---------- */

void sqlc_notify_done(const char *title, const char *message, int failed) {
    /* in the background: flash the taskbar button until the window is active */
    HWND owner = sqlc_w_owner();
    if (owner && GetForegroundWindow() != owner) {
        FLASHWINFO fi = { sizeof fi, owner, FLASHW_TRAY | FLASHW_TIMERNOFG, 3, 0 };
        FlashWindowEx(&fi);
    }
    MessageBeep(failed ? MB_ICONEXCLAMATION : MB_ICONASTERISK);
    const char *labels[] = { "OK" };
    task_dialog(title, message, failed ? TD_WARNING_ICON : TD_INFORMATION_ICON, labels, 1, 100, 1);
}
