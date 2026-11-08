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

/* Current sidebar width in points, and moving the divider from code (the
 * same limits as a drag apply). */
int sqlc_split_sidebar_width(uiControl *split);
void sqlc_split_set_sidebar_width(uiControl *split, int width);

/* Make a uiTable's last column grow/shrink with the table's width. */
void sqlc_table_fill_width(uiTable *t);

#endif
