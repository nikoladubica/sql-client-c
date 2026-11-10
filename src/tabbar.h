#ifndef SQLC_TABBAR_H
#define SQLC_TABBAR_H

/*
 * The window's tab strip as a libui control: left-aligned tabs, each with
 * its title and a close button (x), a "+" button right after the last tab,
 * and empty space to the right, over a thin separator. Drawn by one custom
 * NSView in system colors (light and dark mode). libui-ng's uiTab centres
 * its tabs and has no close or "+" buttons.
 *
 * It only shows tabs and reports clicks: the owner keeps the pages and
 * decides what selecting, closing or adding a tab does. Main thread only,
 * like every libui control. No database, no app state.
 */

#include "ui.h"

/* What the strip reports, called from its mouse handling (never when the
 * strip is changed from code):
 * - on_select: a click on tab index (not on its x), which is now selected;
 * - on_close: a click on tab index's x, or a middle click anywhere on the
 *   tab. The tab is not removed: call sqlc_tabbar_remove if it closes;
 * - on_new: a click on "+".
 * The callbacks may change the strip (append, remove, select). */
typedef struct {
    void (*on_select)(uiControl *bar, int index, void *data);
    void (*on_close)(uiControl *bar, int index, void *data);
    void (*on_new)(uiControl *bar, void *data);
    void *data;
} sqlc_tabbar_callbacks;

/* An empty strip (just "+"). *cb is copied; cb may be NULL (no callbacks).
 * Owned by its libui parent, like any other control. */
uiControl *sqlc_new_tabbar(const sqlc_tabbar_callbacks *cb);

/* Add a tab at the end (title: UTF-8, copied). Doesn't change the
 * selection, except that the first tab added is selected. Returns its
 * index. */
int sqlc_tabbar_append(uiControl *bar, const char *title);

/* Remove tab index (out of range: ignored). If it was selected, the tab now
 * at its place (or the last one) becomes selected; -1 once none are left. */
void sqlc_tabbar_remove(uiControl *bar, int index);

/* Change tab index's title (UTF-8, copied; out of range: ignored). */
void sqlc_tabbar_set_title(uiControl *bar, int index, const char *title);

/* Select tab index from code (out of range: ignored). Doesn't call
 * on_select. */
void sqlc_tabbar_set_selected(uiControl *bar, int index);

/* The selected tab, or -1 if there are none. */
int sqlc_tabbar_selected(uiControl *bar);

/* Number of tabs. */
int sqlc_tabbar_count(uiControl *bar);

#endif
