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

/* Fix a control's width (points), e.g. a uiProgressBar, which libui sizes
 * to whatever its box gives it. */
void sqlc_control_set_width(uiControl *c, double width);

/* A long job ended: bounce the Dock icon once if the app is in the
 * background, play a sound ("Glass", or the system alert sound when
 * failed), then show a modal alert (warning style when failed). */
void sqlc_notify_done(const char *title, const char *message, int failed);

#endif
