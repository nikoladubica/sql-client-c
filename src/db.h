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

    /* Where the columns come from, as the server reports it (MYSQL_FIELD
     * org_name, org_table, db). org_names[c] is the column's real name (an
     * alias undone), NULL for an expression; org_names itself may be NULL
     * (a set not read from the server). org_table and org_db are set only
     * when every column is a real column of that one table, which is what
     * makes a query's result editable. */
    char **org_names;
    char *org_table, *org_db;
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

/* Open a connection: utf8mb4, CLIENT_MULTI_STATEMENTS | CLIENT_MULTI_RESULTS
 * (as db_execute expects). db may be NULL (no database selected), port 0
 * (the default). Returns the connection (close it with mysql_close), or NULL
 * with *err set to a malloc'd message (NULL if out of memory); the handle is
 * closed then. Blocks; the GUI calls it on its worker thread. */
MYSQL *db_connect(const char *host, const char *user, const char *pass,
                  const char *db, unsigned int port, char **err);

/* malloc'd `name` with any backticks doubled, for use as an identifier. */
char *db_quote_ident(const char *name);

/* 1 if err (mysql_errno) means the connection itself is gone and every
 * further call on it fails the same way: 2006 "Server has gone away"
 * (e.g. closed by wait_timeout while idle), 2013 "Lost connection to server
 * during query", 1927 "Connection was killed" (MariaDB) and 4031 "The client
 * was disconnected by the server because of inactivity" (MySQL 8). */
static inline int db_errno_lost(unsigned int err) {
    return err == 2006 || err == 2013 || err == 1927 || err == 4031;
}

/* The same for the last call on conn. */
static inline int db_conn_lost(MYSQL *conn) {
    return db_errno_lost(mysql_errno(conn));
}

#endif
