#ifndef SQLC_SPLITVIEW_H
#define SQLC_SPLITVIEW_H

#include "ui.h"

/* Draggable splits for libui-ng (which has none): native NSSplitViews
 * wrapped as libui controls. Main thread only, like all libui calls. */

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

/* Current sidebar width in points, and moving the divider from code (the
 * same limits as a drag apply). */
int sqlc_split_sidebar_width(uiControl *split);
void sqlc_split_set_sidebar_width(uiControl *split, int width);

/* A native NSSplitView with `top` above `bottom` and a draggable horizontal
 * divider between them.
 *
 * - The divider starts at initial_fraction of the height (0.5 = half; a
 *   value outside (0, 1) means half). It is placed on the first layout that
 *   has room for both panes.
 * - Neither pane can go below min_height points, by dragging or by resizing
 *   the window; when the window grows or shrinks, both panes share the change.
 * - If save_key is not NULL, the divider position is stored in the user
 *   defaults under that key as a fraction of the height and restored on the
 *   next launch, clamped to the minimum heights (so it never restores with a
 *   pane hidden).
 *
 * The split control owns both children (destroying it destroys them). */
uiControl *sqlc_new_vertical_split(uiControl *top, uiControl *bottom,
                                   double initial_fraction, int min_height,
                                   const char *save_key);

/* Current top pane height in points, and moving the divider from code (the
 * same limits as a drag apply). */
int sqlc_split_top_height(uiControl *split);
void sqlc_split_set_top_height(uiControl *split, int height);

/* Make a uiTable's last column grow/shrink with the table's width. */
void sqlc_table_fill_width(uiTable *t);

#endif
