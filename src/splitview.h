#ifndef SQLC_SPLITVIEW_H
#define SQLC_SPLITVIEW_H

#include "ui.h"

/* A native NSSplitView wrapped as a libui control: `side` on the left,
 * `main` on the right, with a draggable divider between them.
 *
 * - The sidebar starts at initial_width and can't go below min_width;
 *   the main pane takes the remaining space (and keeps it when the window
 *   is resized).
 * - If save_key is not NULL, the sidebar width is stored in the user
 *   defaults under that key and restored on the next launch.
 *
 * The split control owns both children (destroying it destroys them). */
uiControl *sqlc_new_sidebar_split(uiControl *side, uiControl *main,
                                  int initial_width, int min_width,
                                  const char *save_key);

/* The same, stacked: `top` above `bottom` with a draggable horizontal
 * divider.
 *
 * - The top pane starts at initial_height points, or at half of the height
 *   when initial_height is 0 or less (or at the saved height, see below).
 *   The divider is placed on the first layout that has room for both
 *   minimums, so a tiny first pass can't pin it against one edge.
 * - Neither pane goes below its minimum, by dragging, by resizing the
 *   window or from code; the top pane keeps its height when the window is
 *   resized and the bottom one absorbs the change.
 * - If save_key is not NULL, the top pane's height is stored in the user
 *   defaults under that key and restored on the next launch, clamped to
 *   both minimums (so it never comes back with a pane hidden). */
uiControl *sqlc_new_stacked_split(uiControl *top, uiControl *bottom,
                                  int initial_height, int min_top, int min_bottom,
                                  const char *save_key);

/* Size of the first pane (sidebar width, top pane height) in points, and
 * moving the divider from code (the same limits as a drag apply). */
int sqlc_split_position(uiControl *split);
void sqlc_split_set_position(uiControl *split, int size);

/* Make a uiTable's last column grow/shrink with the table's width. */
void sqlc_table_fill_width(uiTable *t);

/* Lay the window's views out now, even before it is shown, and (redraw)
 * repaint all of it. Splits place their divider on their first layout, so
 * one whose first layout happens on screen leaves its starting divider
 * drawn; laying them out before the window shows avoids that. */
void sqlc_window_layout_now(uiWindow *w, int redraw);

/* Enable or disable a uiTable for clicks, and dim it while disabled
 * (libui-ng's uiControlDisable doesn't reach a table). */
void sqlc_table_set_enabled(uiTable *t, int enabled);

#endif
