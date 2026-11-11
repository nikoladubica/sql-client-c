#ifndef SQLC_ROW_EDIT_H
#define SQLC_ROW_EDIT_H

/*
 * Editing the rows of a browsed table (no UI, no network): what each column
 * holds, checking and normalising what the user typed, and the SQL for
 * UPDATE / INSERT / DELETE of single grid rows.
 *
 * Rows are identified by the primary key. Without one, every comparable
 * column is matched with <=> and the statement is limited to one row.
 */

#include <stddef.h>

#include "db.h"

typedef enum {
    COL_TEXT,     /* char, varchar, text, enum, json, ... */
    COL_NUMBER,   /* integer and decimal types, float, double */
    COL_DATE,     /* date */
    COL_DATETIME, /* datetime, timestamp */
    COL_TIME,     /* time */
    COL_BINARY    /* blob, binary, bit, geometry: not editable here */
} col_kind;

typedef struct {
    char *name;
    col_kind kind;
    int approx;          /* float / double: can't be matched by its text */
    int nullable;
    int primary;         /* part of the primary key */
    int auto_increment;
    int generated;       /* virtual / stored generated column: read-only */
} col_info;

typedef struct {
    unsigned int ncols;  /* same columns, same order as the browsed rows */
    col_info *cols;
    unsigned int npk;    /* primary key columns (0 = none) */
    int whole;           /* the rows hold every column of the table */
    int star;            /* ... in the table's order, as SELECT * has them */
} table_info;

/* Kind of an information_schema DATA_TYPE ("int", "varchar", ...). */
col_kind row_edit_kind(const char *data_type, int *approx);

/*
 * Build ti for the columns of `rows` (a SELECT * of the table, or a query
 * of some of its columns, in any order) from `columns`, the rows of
 *   SELECT COLUMN_NAME, DATA_TYPE, IS_NULLABLE, COLUMN_KEY, EXTRA
 *   FROM information_schema.COLUMNS ... ORDER BY ORDINAL_POSITION
 * A column is matched by its real name (rows->org_names, so an alias
 * works) or, without org_names, by its name. Returns 0, or -1 (ti empty)
 * if a column of `rows` isn't described there (an expression) or appears
 * twice. A primary key column missing from `rows` drops the key; rows
 * without a key can then only be edited if they hold every column (whole).
 */
int table_info_load(table_info *ti, const db_set *columns, const db_set *rows);
void table_info_free(table_info *ti);

/* Can the user type into this column? */
int row_edit_editable(const table_info *ti, unsigned int col);

/*
 * Check and normalise a typed value for its column:
 *   number   "  42 " -> "42", "1.5e3"; anything else is an error
 *   date     "2024-1-5" or "2024/01/05" -> "2024-01-05"
 *   datetime "2024-01-05 9:30" / "2024-01-05T09:30:00" / "2024-01-05"
 *            -> "2024-01-05 09:30:00" (fractions kept)
 *   time     "9:30" -> "09:30:00", "-838:59:59"
 *   text     unchanged
 * Empty input (spaces only) for a non-text column means NULL: *is_null is
 * set and NULL returned, or an error if the column is NOT NULL.
 * Returns the malloc'd value, or NULL with err filled in (or *is_null).
 */
char *row_edit_normalize(const col_info *c, const char *text, int *is_null,
                         char *err, size_t errsize);

/* Escapes len bytes of from into to (2 * len + 1 bytes), returns the length:
 * mysql_real_escape_string() in the app, which knows the server's sql_mode. */
typedef unsigned long (*row_edit_escape_fn)(char *to, const char *from,
                                            unsigned long len, void *data);
typedef struct {
    row_edit_escape_fn fn;
    void *data;
} row_edit_escaper;

/*
 * The SQL to set column col of row `row` of set to value (NULL = SQL NULL;
 * already normalised), followed by a SELECT of the changed row so the grid
 * can show what the server stored:
 *   UPDATE `t` SET `c` = 'v' WHERE `id` = '3' LIMIT 1;
 *   SELECT * FROM `t` WHERE `id` = '3' LIMIT 1
 * Returns malloc'd SQL, or NULL with err filled in.
 */
char *row_edit_update_sql(const char *table, const table_info *ti, const db_set *set,
                          size_t row, unsigned int col, const char *value,
                          const row_edit_escaper *esc, char *err, size_t errsize);

/*
 * INSERT of row `row` of set, with the columns where is_set[col] is true
 * (the rest get their defaults), then a SELECT of the new row when it can be
 * found again (by the key typed in, or LAST_INSERT_ID() for an
 * auto-increment key). Without that second statement, reload the table.
 */
char *row_edit_insert_sql(const char *table, const table_info *ti, const db_set *set,
                          size_t row, const unsigned char *is_set,
                          const row_edit_escaper *esc, char *err, size_t errsize);

/* One "DELETE FROM `t` WHERE ... LIMIT 1;" per row, inside
 * START TRANSACTION; ... COMMIT (ROLLBACK if a statement fails). */
char *row_edit_delete_sql(const char *table, const table_info *ti, const db_set *set,
                          const size_t *rows, size_t nrows,
                          const row_edit_escaper *esc, char *err, size_t errsize);

#endif
