#ifndef SQLC_SEGMENTED_H
#define SQLC_SEGMENTED_H

/*
 * A segmented switch ("Normal | AI | Structure") as a libui control:
 * a native NSSegmentedControl, one segment selected at a time. libui-ng
 * has no segmented control of its own.
 *
 * Main thread only, like every libui control. No database, no app state:
 * the owner decides what a segment means.
 */

#include "ui.h"

/* Called when the user picks a segment (not when the selection is set from
 * code). index is the segment picked, 0-based. */
typedef void (*sqlc_segmented_fn)(uiControl *seg, int index, void *data);

/* A switch with n segments titled labels[0..n-1] (UTF-8, copied), the first
 * one selected. Returns NULL if n < 1. The control is owned by its libui
 * parent, like any other (destroying the parent destroys it). */
uiControl *sqlc_new_segmented(const char *const *labels, int n);

/* The selected segment, or -1 if none. */
int sqlc_segmented_selected(uiControl *seg);

/* Select a segment from code; out-of-range indexes are ignored. Doesn't call
 * the on_changed callback. */
void sqlc_segmented_set_selected(uiControl *seg, int index);

/* Set the callback for user changes (replaces any earlier one; NULL = none). */
void sqlc_segmented_on_changed(uiControl *seg, sqlc_segmented_fn f, void *data);

#endif
