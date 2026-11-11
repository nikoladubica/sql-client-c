#ifndef SQLC_BROWSE_H
#define SQLC_BROWSE_H

/*
 * Browsing a table in pages, with an optional one-column filter (no UI, no
 * network): the SELECT for a page, the primary key columns it is ordered
 * by, and the page bar's label.
 *
 *   SELECT * FROM `orders` WHERE `status` = 'paid' ORDER BY `id` LIMIT 1000 OFFSET 2000;
 *
 * Identifiers are backtick-quoted here; values only go through the caller's
 * escaper (mysql_real_escape_string in the app). Nothing here counts rows:
 * a next page is found by asking for one row more than a page holds.
 */

#include <stddef.h>

#include "db.h"
#include "row_edit.h" /* row_edit_escaper */

enum { BROWSE_PAGE_ROWS = 1000 };

/* The filter's operators, in the order the GUI's dropdown lists them. */
typedef enum {
    BROWSE_EQ, BROWSE_NE, BROWSE_LT, BROWSE_GT, BROWSE_LE, BROWSE_GE,
    BROWSE_LIKE, BROWSE_NOT_LIKE, BROWSE_IS_NULL, BROWSE_IS_NOT_NULL,
    BROWSE_NOPS
} browse_op;

/* "=", "!=", "<", ">", "<=", ">=", "LIKE", "NOT LIKE", "IS NULL",
 * "IS NOT NULL" (as the dropdown shows them; != is written <> in the SQL).
 * NULL if op is out of range. */
const char *browse_op_label(browse_op op);

/* 1 if op compares with a value, 0 for IS NULL / IS NOT NULL (and out of range). */
int browse_op_takes_value(browse_op op);

typedef struct {
    const char *column;   /* NULL = no filter */
    browse_op op;
    const char *value;    /* ignored when !browse_op_takes_value(op); for
                           * (NOT) LIKE without % or _ it is searched as
                           * '%value%' (contains) */
} browse_filter;

/* 1 if f filters anything: a column and an operator in range, and a
 * non-empty value or a NULL operator. f may be NULL (0). An empty value
 * means "no filter", so `= ''` can't be asked for. */
int browse_filter_active(const browse_filter *f);

/*
 * SELECT * FROM `table` [WHERE `col` <op> 'v'] [ORDER BY `k1`, `k2`] LIMIT limit [OFFSET n];
 * The WHERE only when browse_filter_active(f) (f may be NULL); the value is
 * escaped with esc and quoted (also for numbers: the server converts).
 * order: norder column names (the primary key), none for no ORDER BY.
 * OFFSET only when offset > 0. Always ends with ';'.
 * Returns malloc'd SQL, or NULL with err filled in (no table, an operator
 * out of range, out of memory).
 */
char *browse_select_sql(const char *table, const browse_filter *f,
                        const char *const *order, size_t norder,
                        unsigned long long offset, unsigned long limit,
                        const row_edit_escaper *esc, char *err, size_t errsize);

/*
 * columns: the rows of
 *   SELECT COLUMN_NAME, DATA_TYPE, IS_NULLABLE, COLUMN_KEY, EXTRA
 *   FROM information_schema.COLUMNS ... ORDER BY ORDINAL_POSITION
 * (the first set fetch_table_meta reads in the GUI).
 *
 * browse_key_columns: the primary key's columns (COLUMN_KEY = 'PRI') in
 * that order. *keys is a malloc'd array of *nkeys pointers into columns
 * (free the array, not the names; it is only valid while columns is), or
 * NULL when there is no key (*nkeys 0). Returns 0, or -1 (out of memory,
 * or columns has fewer than 4 columns) with *keys NULL and *nkeys 0.
 */
int browse_key_columns(const db_set *columns, const char ***keys, size_t *nkeys);

/* Row index in columns of the filter's default column: the first key
 * column, else 0 (also for NULL or an empty set). */
size_t browse_default_column(const db_set *columns);

/* The page bar's label for nrows rows from row offset (0-based):
 * "Rows 1–1,000", "Rows 1,001–1,234", "No rows" (en dash, thousands
 * separators). Always NUL-terminated (cut short if size is too small). */
void browse_page_label(char *buf, size_t size, unsigned long long offset, size_t nrows);

#endif
