#ifndef SQLC_LOADING_H
#define SQLC_LOADING_H

/*
 * A loading indicator as a libui control: a native spinning
 * NSProgressIndicator with a line of small, centred, wrapping text below it.
 * libui-ng only has a progress bar.
 *
 * The text area is at least three lines high, spinning or not, so in a
 * wide enough place (the welcome form) the layout around it doesn't jump;
 * where the text wraps to more lines (the sidebar) the control grows.
 * Main thread only.
 */

#include "ui.h"

/* Stopped and empty to start with. Owned by its libui parent, like any
 * other control. */
uiControl *sqlc_new_loading(void);

/* Show the spinner and the text (UTF-8, copied; NULL = none). */
void sqlc_loading_start(uiControl *l, const char *text);

/* Hide the spinner and the text (the space stays). */
void sqlc_loading_stop(uiControl *l);

#endif
