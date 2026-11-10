#ifndef SQLC_TABS_H
#define SQLC_TABS_H

/*
 * Text for the GUI's tabs: a tab's title and the list of running jobs the
 * quit and close-tab confirmations show. Pure string formatting: no UI, no
 * database. Output is always NUL-terminated and never cut inside a UTF-8
 * character.
 */

#include <stddef.h>

/* Longest tab title, in bytes (without the NUL). */
#define TAB_TITLE_MAX 60

/*
 * A tab's title into buf (size bytes, size >= 1): the saved connection's
 * name when it connected from one (profile non-NULL and not empty), else
 * "user@host / db" ("user@host" without a database, "host" without a user),
 * else, not connected (host NULL or empty), "New connection". Cut to
 * TAB_TITLE_MAX bytes (and to size) with an ellipsis (U+2026, 3 bytes) at
 * the end. Returns buf.
 */
char *tab_title(char *buf, size_t size, const char *profile, const char *user,
                const char *host, const char *db);

/* One running job: the title of its tab and what it is ("query",
 * "export of shop", ...). */
typedef struct {
    const char *tab;
    const char *job;
} tab_job_info;

/*
 * The n jobs, one line each ("<tab> - <job>", lines joined by "\n", no
 * trailing newline), into buf (size bytes, size >= 1). Each part is cut to
 * 80 bytes. When the rest doesn't fit, the last line is "...and N more"
 * instead. Returns the number of jobs listed in full.
 */
size_t running_jobs_text(char *buf, size_t size, const tab_job_info *items, size_t n);

#endif
