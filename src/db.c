#include "db.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

char *db_error_string(MYSQL *conn) {
    const char *msg = mysql_error(conn);
    size_t len = strlen(msg) + 64;
    char *s = malloc(len);
    if (s) snprintf(s, len, "ERROR %u (%s): %s",
                    mysql_errno(conn), mysql_sqlstate(conn), msg);
    return s;
}

MYSQL *db_connect(const char *host, const char *user, const char *pass,
                  const char *db, unsigned int port, char **err) {
    *err = NULL;
    MYSQL *conn = mysql_init(NULL);
    if (!conn) { *err = strdup("mysql_init failed"); return NULL; }
    mysql_options(conn, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    if (!mysql_real_connect(conn, host, user, pass, db, port, NULL,
                            CLIENT_MULTI_STATEMENTS | CLIENT_MULTI_RESULTS)) {
        *err = db_error_string(conn);
        mysql_close(conn);
        return NULL;
    }
    return conn;
}

char *db_quote_ident(const char *name) {
    size_t n = strlen(name);
    char *q = malloc(2 * n + 3), *o = q;
    if (!q) return NULL;
    *o++ = '`';
    for (const char *c = name; *c; c++) { if (*c == '`') *o++ = '`'; *o++ = *c; }
    *o++ = '`';
    *o = '\0';
    return q;
}

/* Case-insensitive keyword match at *p followed by a word boundary. */
static int match_word(const char **p, const char *word) {
    size_t n = strlen(word);
    if (strncasecmp(*p, word, n) != 0) return 0;
    char next = (*p)[n];
    if (next != '\0' && next != ';' && !isspace((unsigned char)next)) return 0;
    *p += n;
    while (isspace((unsigned char)**p)) (*p)++;
    return 1;
}

/*
 * If sql is a single "SHOW [FULL] TABLES ..." statement, return a malloc'd
 * "SHOW FULL TABLES ..." so we get the Table_type column to filter on.
 * *was_full tells whether the user asked for FULL themselves.
 */
static char *rewrite_show_tables(const char *sql, int *was_full) {
    const char *p = sql;
    while (isspace((unsigned char)*p)) p++;
    if (!match_word(&p, "SHOW")) return NULL;
    *was_full = match_word(&p, "FULL");
    if (!match_word(&p, "TABLES")) return NULL;

    /* Only handle a single statement: no ';' before the trailing one. */
    const char *semi = strchr(p, ';');
    if (semi) {
        const char *q = semi + 1;
        while (isspace((unsigned char)*q)) q++;
        if (*q != '\0') return NULL;
    }

    const char *prefix = "SHOW FULL TABLES ";
    char *out = malloc(strlen(prefix) + strlen(p) + 1);
    if (!out) return NULL;
    strcpy(out, prefix);
    strcat(out, p);
    return out;
}

static db_set *push_set(db_outcome *out) {
    db_set *n = realloc(out->sets, (out->nsets + 1) * sizeof *n);
    if (!n) return NULL;
    out->sets = n;
    db_set *s = &out->sets[out->nsets++];
    memset(s, 0, sizeof *s);
    return s;
}

static char *dup_or_null(const char *s, unsigned long len) {
    if (!s) return NULL;
    char *d = malloc(len + 1);
    if (d) { memcpy(d, s, len); d[len] = '\0'; }
    return d;
}

/*
 * Read a result set row by row (mysql_use_result) into s.
 * type_col >= 0: skip rows whose value there contains "VIEW";
 * drop_col >= 0: leave that column out of the copy.
 */
static int collect_rows(MYSQL_RES *res, db_set *s, size_t max_rows,
                        int type_col, int drop_col) {
    unsigned int src_cols = mysql_num_fields(res);
    MYSQL_FIELD *fields = mysql_fetch_fields(res);

    s->has_rows = 1;
    s->ncols = src_cols - (drop_col >= 0 ? 1 : 0);
    s->names = calloc(s->ncols ? s->ncols : 1, sizeof *s->names);
    s->numeric = calloc(s->ncols ? s->ncols : 1, sizeof *s->numeric);
    if (!s->names || !s->numeric) return -1;

    for (unsigned int i = 0, o = 0; i < src_cols; i++) {
        if ((int)i == drop_col) continue;
        s->names[o] = strdup(fields[i].name);
        s->numeric[o] = IS_NUM(fields[i].type);
        o++;
    }

    size_t cap = 0;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        if (type_col >= 0 && row[type_col] && strstr(row[type_col], "VIEW"))
            continue;
        if (max_rows && s->nrows == max_rows) {
            s->truncated = 1;
            continue; /* keep reading so the connection stays in sync */
        }
        if (s->nrows == cap) {
            cap = cap ? cap * 2 : 64;
            char **n = realloc(s->cells, cap * s->ncols * sizeof *n);
            if (!n) return -1;
            s->cells = n;
        }
        unsigned long *lens = mysql_fetch_lengths(res);
        char **dst = s->cells + s->nrows * s->ncols;
        for (unsigned int i = 0, o = 0; i < src_cols; i++) {
            if ((int)i == drop_col) continue;
            dst[o++] = dup_or_null(row[i], lens[i]);
        }
        s->nrows++;
    }
    return 0;
}

static int find_column(MYSQL_RES *res, const char *name) {
    unsigned int n = mysql_num_fields(res);
    MYSQL_FIELD *fields = mysql_fetch_fields(res);
    for (unsigned int i = 0; i < n; i++)
        if (strcasecmp(fields[i].name, name) == 0) return (int)i;
    return -1;
}

int db_execute(MYSQL *conn, const char *sql, const db_exec_opts *opts,
               db_outcome *out) {
    memset(out, 0, sizeof *out);

    int was_full = 0;
    char *rewritten = opts->hide_views ? rewrite_show_tables(sql, &was_full) : NULL;
    int filtering = rewritten != NULL;

    int qrc = mysql_query(conn, filtering ? rewritten : sql);
    free(rewritten);
    if (qrc != 0) { out->error = db_error_string(conn); return -1; }

    int status;
    do {
        MYSQL_RES *res = mysql_use_result(conn);
        db_set *s = push_set(out);
        if (!s) { if (res) mysql_free_result(res); out->error = strdup("out of memory"); return -1; }

        if (res) {
            int type_col = filtering ? find_column(res, "Table_type") : -1;
            int drop_col = (filtering && !was_full) ? type_col : -1;
            int rc = collect_rows(res, s, opts->max_rows, type_col, drop_col);
            unsigned int err = mysql_errno(conn); /* fetch error, e.g. lost connection */
            mysql_free_result(res);
            if (rc != 0) { out->error = strdup("out of memory"); return -1; }
            if (err) { out->error = db_error_string(conn); return -1; }
        } else if (mysql_field_count(conn) == 0) {
            s->affected = mysql_affected_rows(conn);
        } else {
            out->nsets--; /* nothing usable was produced */
            out->error = db_error_string(conn);
            return -1;
        }

        status = mysql_next_result(conn); /* 0 = more, -1 = done, >0 = error */
        if (status > 0) { out->error = db_error_string(conn); return -1; }
    } while (status == 0);

    return 0;
}

void db_outcome_free(db_outcome *out) {
    for (size_t i = 0; i < out->nsets; i++) {
        db_set *s = &out->sets[i];
        for (unsigned int c = 0; c < s->ncols; c++) free(s->names ? s->names[c] : NULL);
        for (size_t k = 0; s->cells && k < s->nrows * s->ncols; k++) free(s->cells[k]);
        free(s->names);
        free(s->numeric);
        free(s->cells);
    }
    free(out->sets);
    free(out->error);
    memset(out, 0, sizeof *out);
}
