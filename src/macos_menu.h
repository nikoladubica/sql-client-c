#ifndef SQLC_MACOS_MENU_H
#define SQLC_MACOS_MENU_H

#include "ui.h"

/* What the menus added by sqlc_install_menus call, on the main thread. */
typedef struct {
    void (*run)(void);       /* Query > Run (Cmd+Return) */
    void (*new_tab)(void);   /* File > New Tab (Cmd+T) */
    void (*close_tab)(void); /* File > Close Tab (Cmd+W) */
} sqlc_menu_callbacks;

/* Add the standard Edit menu (Undo/Redo/Cut/Copy/Paste/Select All), a
 * Query menu with Run, and New Tab / Close Tab at the top of libui's File
 * menu. Turns off macOS's automatic window tabbing (sqlc has its own tabs).
 * Call after uiInit() and after the libui menus and window are created.
 * *cb is copied. */
void sqlc_install_menus(const sqlc_menu_callbacks *cb);

/* Modal "OK / Cancel" alert; returns 1 if the user chose ok_label. */
int sqlc_confirm(const char *title, const char *message, const char *ok_label);

/* Give a uiButton the small control size (smaller bezel and font). */
void sqlc_button_set_small(uiButton *b);

/* Fix a control's width (points), e.g. a uiProgressBar, which libui sizes
 * to whatever its box gives it. */
void sqlc_control_set_width(uiControl *c, double width);

/* A long job ended: bounce the Dock icon once if the app is in the
 * background, play a sound ("Glass", or the system alert sound when
 * failed), then show a modal alert (warning style when failed). */
void sqlc_notify_done(const char *title, const char *message, int failed);

#endif
