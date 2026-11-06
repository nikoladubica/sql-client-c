#ifndef SQLC_DB_H
#define SQLC_DB_H

/*
 * UI-independent core shared by the CLI (sqlc) and the GUI (sqlc-gui):
 * run SQL and collect the results as plain data, with no printing.
 */

#include <stddef.h>
#include <mysql.h>

/* One statement's outcome: either a result set or "Query OK". */
typedef struct {
    int has_rows;               /* 1 = result set, 0 = OK (affected rows) */
    unsigned long long affected;

    unsigned int ncols;
    char **names;               /* ncols column names */
    int *numeric;               /* ncols flags: right-align numbers */
    size_t nrows;
    char **cells;               /* nrows * ncols, row-major; NULL = SQL NULL */
    int truncated;              /* more rows existed than max_rows */
} db_set;

typedef struct {
    db_set *sets;
    size_t nsets;
    char *error;                /* NULL on success; "ERROR 1054 (42S22): ..." */
} db_outcome;

typedef struct {
    int hide_views;             /* drop views from a lone SHOW [FULL] TABLES */
    size_t max_rows;            /* 0 = unlimited */
} db_exec_opts;

/* Run SQL (possibly several statements). Returns 0 on success, -1 on error.
 * Sets that completed before an error are still returned in out. */
int db_execute(MYSQL *conn, const char *sql, const db_exec_opts *opts,
               db_outcome *out);

void db_outcome_free(db_outcome *out);

/* Cell accessor; returns NULL for SQL NULL. */
static inline const char *db_cell(const db_set *s, size_t row, unsigned int col) {
    return s->cells[row * s->ncols + col];
}

/* malloc'd "ERROR <n> (<state>): <message>" for the connection's last error. */
char *db_error_string(MYSQL *conn);

#endif
