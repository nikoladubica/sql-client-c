#ifndef SQLC_GRID_EDIT_H
#define SQLC_GRID_EDIT_H

/*
 * In-place cell editing for a libui uiTable (libui-ng can't edit on
 * double-click, has no date cells and no per-cell context menu):
 *
 * - double-clicking a cell reports (row, column); the app then opens an
 *   editor on it with sqlc_grid_begin_edit()
 * - the editor is a text field over the cell; for dates it also shows a
 *   calendar below it. Typing and the calendar stay in sync.
 * - Return / Tab / Shift-Tab / clicking elsewhere end the edit and report
 *   the text; Escape cancels
 * - a right-click menu on cells (Edit, Set to NULL, Copy, Delete rows)
 *
 * All callbacks run on the main thread after the event that caused them has
 * finished, so they may start jobs, show dialogs or reopen the editor.
 */

#include <stddef.h>

#include "ui.h"

typedef enum {
    GRID_EDITOR_TEXT,
    GRID_EDITOR_DATE,     /* text field + calendar */
    GRID_EDITOR_DATETIME  /* text field + calendar and clock */
} grid_editor_kind;

typedef enum {
    GRID_END_RETURN,
    GRID_END_TAB,
    GRID_END_BACKTAB,
    GRID_END_BLUR         /* focus moved elsewhere */
} grid_end;

typedef enum {
    GRID_MENU_EDIT,
    GRID_MENU_SET_NULL,
    GRID_MENU_COPY,
    GRID_MENU_DELETE_ROWS,
    GRID_MENU_COUNT
} grid_menu_item;

#define GRID_MENU_BIT(item) (1u << (item))

typedef struct {
    void (*double_clicked)(int row, int col, void *data);
    void (*committed)(int row, int col, const char *text, grid_end how, void *data);
    void (*cancelled)(int row, int col, void *data);
    /* right-click on (row, col): return the GRID_MENU_BIT()s to enable, 0 = no menu */
    unsigned (*menu_open)(int row, int col, void *data);
    void (*menu_action)(grid_menu_item item, int row, int col, void *data);
    void *data;
} grid_edit_callbacks;

/* Hook a table up for editing (lives as long as the table). */
void sqlc_grid_enable_editing(uiTable *t, const grid_edit_callbacks *cb);

/* Open the editor on a cell (scrolling to it), selecting `text`.
 * placeholder is shown greyed while the field is empty (e.g. "NULL"). */
void sqlc_grid_begin_edit(uiTable *t, int row, int col, const char *text,
                          const char *placeholder, grid_editor_kind kind);

/* Close the editor without reporting anything. */
void sqlc_grid_close_editor(uiTable *t);

/* Close the editor and drop every callback still queued or to come:
 * call before the table is destroyed. */
void sqlc_grid_detach(uiTable *t);

/* 1 if an editor is open, with its cell and current text (malloc'd) if wanted. */
int sqlc_grid_editing(uiTable *t, int *row, int *col, char **text);

/* 1 if the open editor shows a calendar. */
int sqlc_grid_editor_has_calendar(uiTable *t);

void sqlc_grid_scroll_to_row(uiTable *t, int row);

/* For tests: replace the editor's text and end the edit as `how` would. */
void sqlc_grid_test_finish(uiTable *t, const char *text, grid_end how);

/* For tests: pick a date in the open editor's calendar (YYYY-MM-DD[ HH:MM:SS]). */
void sqlc_grid_test_pick_date(uiTable *t, const char *value);

#endif
