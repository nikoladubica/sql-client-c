#include "complete.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <editline/readline.h>

static const char *const keywords[] = {
    "ADD", "ALTER", "AND", "AS", "ASC", "AUTO_INCREMENT", "BETWEEN", "BY",
    "CASE", "COLUMN", "COLUMNS", "COUNT", "CREATE", "DATABASE", "DATABASES",
    "DEFAULT", "DELETE", "DESC", "DESCRIBE", "DISTINCT", "DROP", "ELSE",
    "END", "EXISTS", "EXPLAIN", "FROM", "FULL", "GROUP", "HAVING", "IN",
    "INDEX", "INNER", "INSERT", "INTO", "IS", "JOIN", "KEY", "LEFT", "LIKE",
    "LIMIT", "NOT", "NULL", "OFFSET", "ON", "OR", "ORDER", "PRIMARY",
    "PROCESSLIST", "REPLACE", "RIGHT", "SELECT", "SET", "SHOW", "STATUS",
    "TABLE", "TABLES", "THEN", "TRUNCATE", "UNION", "UNIQUE", "UPDATE",
    "USE", "VALUES", "VARIABLES", "WHEN", "WHERE",
};
#define NKEYWORDS (sizeof keywords / sizeof keywords[0])

/* Names loaded from the server (databases, base tables, their columns). */
static char **names;
static size_t nnames, capnames;
static int completion_enabled;

static void add_name(const char *s) {
    if (nnames == capnames) {
        size_t ncap = capnames ? capnames * 2 : 256;
        char **n = realloc(names, ncap * sizeof *n);
        if (!n) return;
        names = n;
        capnames = ncap;
    }
    char *dup = strdup(s);
    if (dup) names[nnames++] = dup;
}

void completion_free(void) {
    for (size_t i = 0; i < nnames; i++) free(names[i]);
    free(names);
    names = NULL;
    nnames = capnames = 0;
}

static void load_column(MYSQL *conn, const char *sql) {
    if (mysql_query(conn, sql) != 0) return;
    MYSQL_RES *res = mysql_store_result(conn);
    if (!res) return;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)))
        if (row[0]) add_name(row[0]);
    mysql_free_result(res);
}

void completion_rehash(MYSQL *conn) {
    if (!completion_enabled) return;
    completion_free();

    load_column(conn, "SHOW DATABASES");
    /* Base tables only: VIEW and SYSTEM VIEW are skipped. */
    load_column(conn,
        "SELECT TABLE_NAME FROM information_schema.TABLES "
        "WHERE TABLE_SCHEMA = DATABASE() "
        "AND TABLE_TYPE NOT IN ('VIEW', 'SYSTEM VIEW')");
    load_column(conn,
        "SELECT DISTINCT c.COLUMN_NAME FROM information_schema.COLUMNS c "
        "JOIN information_schema.TABLES t "
        "  ON t.TABLE_SCHEMA = c.TABLE_SCHEMA AND t.TABLE_NAME = c.TABLE_NAME "
        "WHERE c.TABLE_SCHEMA = DATABASE() "
        "AND t.TABLE_TYPE NOT IN ('VIEW', 'SYSTEM VIEW')");
}

/* Generator called repeatedly by readline: state 0 starts a new search. */
static char *generator(const char *text, int state) {
    static size_t ki, ni, len;
    if (state == 0) { ki = ni = 0; len = strlen(text); }

    while (ki < NKEYWORDS) {
        const char *k = keywords[ki++];
        if (strncasecmp(k, text, len) == 0) return strdup(k);
    }
    while (ni < nnames) {
        const char *n = names[ni++];
        if (strncasecmp(n, text, len) == 0) return strdup(n);
    }
    return NULL;
}

static char **complete(const char *text, int start, int end) {
    (void)start; (void)end;
    rl_attempted_completion_over = 1; /* never fall back to filenames */
    if (!completion_enabled) return NULL;
    return rl_completion_matches(text, generator);
}

void completion_init(int enabled) {
    completion_enabled = enabled;
    rl_attempted_completion_function = complete;
    rl_basic_word_break_characters = " \t\n\"'`@$><=;|&{(,.";
}
