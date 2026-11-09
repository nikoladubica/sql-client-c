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
 * divider. The top pane starts at initial_height (or the saved height) and
 * keeps it when the window is resized; neither pane goes below its minimum. */
uiControl *sqlc_new_stacked_split(uiControl *top, uiControl *bottom,
                                  int initial_height, int min_top, int min_bottom,
                                  const char *save_key);

/* Size of the first pane (sidebar width, top pane height) in points, and
 * moving the divider from code (the same limits as a drag apply). */
int sqlc_split_position(uiControl *split);
void sqlc_split_set_position(uiControl *split, int size);

/* Make a uiTable's last column grow/shrink with the table's width. */
void sqlc_table_fill_width(uiTable *t);

#endif
