#ifndef SQLC_MACOS_MENU_H
#define SQLC_MACOS_MENU_H

/* Add the standard Edit menu (Undo/Redo/Cut/Copy/Paste/Select All) and a
 * Query menu whose Run item (Cmd+Return) calls on_run on the main thread.
 * Call after uiInit() and after the libui menus are created. */
void sqlc_install_menus(void (*on_run)(void));

#endif
