#ifndef SQLC_MACOS_MENU_H
#define SQLC_MACOS_MENU_H

/* Add the standard Edit menu (Undo/Redo/Cut/Copy/Paste/Select All) and a
 * Query menu whose Run item (Cmd+Return) calls on_run on the main thread.
 * Call after uiInit() and after the libui menus are created. */
void sqlc_install_menus(void (*on_run)(void));

/* Modal "OK / Cancel" alert; returns 1 if the user chose ok_label. */
int sqlc_confirm(const char *title, const char *message, const char *ok_label);

/* Give a uiButton the small control size (smaller bezel and font). */
#include "ui.h"
void sqlc_button_set_small(uiButton *b);

#endif
