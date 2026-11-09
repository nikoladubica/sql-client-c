#ifndef SQLC_TABLE_MENU_H
#define SQLC_TABLE_MENU_H

#include <stddef.h>

#include "ui.h"

/* Items of the sidebar's context menu, in menu order. */
typedef enum {
    TABLE_MENU_COPY_NAME,
    TABLE_MENU_RENAME,
    TABLE_MENU_TRUNCATE,
    TABLE_MENU_DELETE,
    TABLE_MENU_EXPORT,
    TABLE_MENU_COUNT
} table_menu_item;

#define TABLE_MENU_BIT(item) (1u << (item))
#define TABLE_MENU_ALL ((1u << TABLE_MENU_COUNT) - 1)

/* Called when the menu opens on `row` (already selected): return the
 * TABLE_MENU_BIT()s of the enabled items, or 0 to show no menu. */
typedef unsigned (*table_menu_open_fn)(int row, void *data);
/* Called after the menu closes with the chosen item and its row. */
typedef void (*table_menu_action_fn)(table_menu_item item, int row, void *data);

/* Give a uiTable a right-click menu. Right-clicking a row selects it first;
 * right-clicking empty space shows nothing. Lives as long as the table. */
void sqlc_table_set_context_menu(uiTable *t, table_menu_open_fn on_open,
                                 table_menu_action_fn on_action, void *data);

/* For tests: build the menu as a right-click on `row` would (selecting it)
 * and write "Title[+|-]" per item ("+" enabled), separated by ", ". */
void sqlc_table_menu_describe(uiTable *t, int row, char *buf, size_t size);

void sqlc_copy_to_clipboard(const char *text);
/* The clipboard's text (malloc'd), or NULL. */
char *sqlc_clipboard_text(void);

/* Modal prompt with a text field prefilled with `initial` (selected).
 * Returns the entered text (malloc'd), or NULL if cancelled. */
char *sqlc_prompt(const char *title, const char *message, const char *initial,
                  const char *ok_label);
/* The same with a password field (typing is hidden), starting empty. */
char *sqlc_prompt_secret(const char *title, const char *message, const char *ok_label);

/* Modal "are you sure?" alert where Cancel is the default button (Return
 * and Escape cancel). destructive marks ok_label red (macOS 11+).
 * Returns 1 only if ok_label was clicked. */
int sqlc_confirm_danger(const char *title, const char *message, const char *ok_label,
                        int destructive);

#endif
