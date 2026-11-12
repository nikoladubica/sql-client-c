/* Shared helpers for the Windows GUI shims (see win_util.h). */

#include "win_util.h"

#include <stdlib.h>
#include <string.h>

#include "ui.h"

WCHAR *sqlc_w_wide(const char *s) {
    if (!s) s = "";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    WCHAR *w = malloc((size_t)n * sizeof *w);
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

char *sqlc_w_utf8(const WCHAR *w) {
    if (!w) w = L"";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char *s = malloc((size_t)n);
    if (s) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

HWND sqlc_w_owner(void) {
    HWND w = GetActiveWindow();
    if (w) {
        HWND root = GetAncestor(w, GA_ROOTOWNER);
        if (root) w = root;
    }
    return w;
}

#define REG_PATH L"Software\\sqlc"

int sqlc_w_get_int(const char *name, int *out) {
    WCHAR *wn = sqlc_w_wide(name);
    if (!wn) return 0;
    DWORD v = 0, size = sizeof v;
    LSTATUS rc = RegGetValueW(HKEY_CURRENT_USER, REG_PATH, wn, RRF_RT_REG_DWORD, NULL, &v, &size);
    free(wn);
    if (rc != ERROR_SUCCESS) return 0;
    *out = (int)v;
    return 1;
}

void sqlc_w_set_int(const char *name, int value) {
    WCHAR *wn = sqlc_w_wide(name);
    if (!wn) return;
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_PATH, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL) == ERROR_SUCCESS) {
        DWORD v = (DWORD)value;
        RegSetValueExW(key, wn, 0, REG_DWORD, (const BYTE *)&v, sizeof v);
        RegCloseKey(key);
    }
    free(wn);
}

void sqlc_w_post(void (*f)(void *data), void *data) {
    /* uiQueueMain posts a message to libui's utility window: it runs after
     * the current message, including a menu's tracking loop, has returned. */
    uiQueueMain(f, data);
}

int sqlc_w_scale(HWND hwnd, int px) {
    HDC dc = GetDC(hwnd);
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(hwnd, dc);
    return MulDiv(px, dpi, 96);
}

static HFONT ui_font, bold_font;

HFONT sqlc_w_font(void) {
    if (!ui_font) {
        NONCLIENTMETRICSW ncm;
        memset(&ncm, 0, sizeof ncm);
        ncm.cbSize = sizeof ncm;
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0))
            ui_font = CreateFontIndirectW(&ncm.lfMessageFont);
        if (!ui_font) ui_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    }
    return ui_font;
}

HFONT sqlc_w_bold_font(void) {
    if (!bold_font) {
        LOGFONTW lf;
        if (GetObjectW(sqlc_w_font(), sizeof lf, &lf)) {
            lf.lfWeight = FW_SEMIBOLD;
            bold_font = CreateFontIndirectW(&lf);
        }
        if (!bold_font) bold_font = sqlc_w_font();
    }
    return bold_font;
}
