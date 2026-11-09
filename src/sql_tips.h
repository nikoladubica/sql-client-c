#ifndef SQLC_SQL_TIPS_H
#define SQLC_SQL_TIPS_H

/*
 * Short SQL tips (MySQL / MariaDB), shown one at a time under the spinner
 * while the GUI connects. Pure data and logic: no UI, no database.
 */

#include <stddef.h>

/* Longest tip, in bytes: the GUI has room for about three lines of it. */
#define SQL_TIP_MAX_LEN 140

/* Number of tips. */
size_t sql_tip_count(void);

/* Tip i (0-based, UTF-8, static storage), or NULL when i is out of range. */
const char *sql_tip(size_t i);

/* A tip index chosen from the random number r, never `last` (the tip shown
 * before) unless it is the only one. Pass last = SIZE_MAX (or any index out
 * of range) when nothing was shown yet. */
size_t sql_tip_pick(size_t last, unsigned long r);

#endif
