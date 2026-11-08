#include "export_db.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "db.h"

#define PROGRESS_EVERY 5000

static char *fmt_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static char *fmt_error(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return strdup(buf);
}

/* `name` with backticks doubled; malloc'd. */
static char *quote_ident(const char *name) {
    size_t n = strlen(name);
    char *q = malloc(2 * n + 3), *o = q;
    if (!q) return NULL;
    *o++ = '`';
    for (const char *c = name; *c; c++) { if (*c == '`') *o++ = '`'; *o++ = *c; }
    *o++ = '`';
    *o = '\0';
    return q;
}

/* Run a query and return its result (stored), or NULL with *err set. */
static MYSQL_RES *query_store(MYSQL *conn, const char *sql, char **err) {
    if (mysql_real_query(conn, sql, strlen(sql)) != 0) { *err = db_error_string(conn); return NULL; }
    MYSQL_RES *r = mysql_store_result(conn);
    if (!r) *err = mysql_errno(conn) ? db_error_string(conn) : strdup("Query returned no result");
    return r;
}

static int is_binary_field(const MYSQL_FIELD *f) {
    switch (f->type) {
    case MYSQL_TYPE_BIT:
    case MYSQL_TYPE_GEOMETRY:
        return 1;
    case MYSQL_TYPE_STRING: case MYSQL_TYPE_VAR_STRING: case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_TINY_BLOB: case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB: case MYSQL_TYPE_BLOB:
        return f->charsetnr == 63; /* the "binary" character set */
    default:
        return 0;
    }
}

/* SHOW CREATE TABLE's statement (works for views too); malloc'd. */
static char *show_create(MYSQL *conn, const char *qtable, char **err) {
    char *sql = malloc(strlen(qtable) + 32);
    if (!sql) { *err = strdup("Out of memory"); return NULL; }
    snprintf(sql, strlen(qtable) + 32, "SHOW CREATE TABLE %s", qtable);
    MYSQL_RES *r = query_store(conn, sql, err);
    free(sql);
    if (!r) return NULL;
    MYSQL_ROW row = mysql_fetch_row(r);
    char *create = row && mysql_num_fields(r) >= 2 && row[1] ? strdup(row[1]) : NULL;
    if (!create && !*err) *err = strdup("SHOW CREATE TABLE returned nothing");
    mysql_free_result(r);
    return create;
}

/* "SELECT `a`, `b` FROM `t`" without generated columns (SQL format), or
 * "SELECT * FROM `t`". malloc'd. */
static char *select_sql(MYSQL *conn, const char *table, const char *qtable,
                        int skip_generated, char **err) {
    if (!skip_generated) {
        char *sql = malloc(strlen(qtable) + 32);
        if (sql) snprintf(sql, strlen(qtable) + 32, "SELECT * FROM %s", qtable);
        else *err = strdup("Out of memory");
        return sql;
    }
    size_t tlen = strlen(table);
    char *esc = malloc(2 * tlen + 1);
    char *q = esc ? malloc(2 * tlen + 256) : NULL;
    if (!q) { free(esc); *err = strdup("Out of memory"); return NULL; }
    mysql_real_escape_string(conn, esc, table, (unsigned long)tlen);
    snprintf(q, 2 * tlen + 256, "SELECT COLUMN_NAME FROM information_schema.COLUMNS "
               "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = '%s' "
               "AND EXTRA NOT LIKE '%%GENERATED%%' ORDER BY ORDINAL_POSITION", esc);
    free(esc);
    MYSQL_RES *r = query_store(conn, q, err);
    free(q);
    if (!r) return NULL;

    size_t cap = strlen(qtable) + 32, len = 0;
    char *sql = malloc(cap);
    if (sql) len = (size_t)snprintf(sql, cap, "SELECT ");
    MYSQL_ROW row;
    unsigned int ncols = 0;
    while (sql && (row = mysql_fetch_row(r))) {
        char *qc = row[0] ? quote_ident(row[0]) : NULL;
        if (!qc) continue;
        size_t need = len + strlen(qc) + strlen(qtable) + 16;
        if (need > cap) {
            cap = need * 2;
            char *n = realloc(sql, cap);
            if (!n) { free(sql); sql = NULL; free(qc); break; }
            sql = n;
        }
        len += (size_t)snprintf(sql + len, cap - len, "%s%s", ncols++ ? ", " : "", qc);
        free(qc);
    }
    mysql_free_result(r);
    if (!sql) { *err = strdup("Out of memory"); return NULL; }
    if (ncols == 0) { free(sql); *err = fmt_error("No columns found for table %s", table); return NULL; }
    snprintf(sql + len, cap - len, " FROM %s", qtable);
    return sql;
}

/* The session time zone (malloc'd), or NULL. */
static char *get_time_zone(MYSQL *conn) {
    char *err = NULL;
    MYSQL_RES *r = query_store(conn, "SELECT @@session.time_zone", &err);
    free(err);
    if (!r) return NULL;
    MYSQL_ROW row = mysql_fetch_row(r);
    char *tz = row && row[0] ? strdup(row[0]) : NULL;
    mysql_free_result(r);
    return tz;
}

static void set_time_zone(MYSQL *conn, const char *tz) {
    size_t n = strlen(tz);
    char *esc = malloc(2 * n + 1);
    char *sql = esc ? malloc(2 * n + 32) : NULL;
    if (sql) {
        mysql_real_escape_string(conn, esc, tz, (unsigned long)n);
        snprintf(sql, 2 * n + 32, "SET time_zone = '%s'", esc);
        mysql_real_query(conn, sql, strlen(sql));
    }
    free(esc);
    free(sql);
}

int export_table(MYSQL *conn, const char *table, int is_view, export_format fmt,
                 const export_opts *opts_in, const char *path,
                 export_progress_fn progress, void *data,
                 unsigned long long *rows_out, char **err) {
    export_opts opts = opts_in ? *opts_in : export_default_opts();
    unsigned long long rows = 0;
    char *qtable = quote_ident(table);
    char *create = NULL, *sql = NULL, *tz = NULL, *tmp = NULL;
    FILE *f = NULL;
    export_writer *w = NULL;
    MYSQL_RES *res = NULL;
    int rc = -1, write_errno = 0, tz_changed = 0;
    int want_rows = fmt != EXPORT_SQL || (opts.sql_data && !is_view);
    *err = NULL;

    if (!qtable) { *err = strdup("Out of memory"); goto done; }

    /* Gather everything that can fail on the server before touching the disk. */
    if (fmt == EXPORT_SQL && opts.sql_structure && !(create = show_create(conn, qtable, err)))
        goto done;
    if (fmt == EXPORT_SQL && want_rows) {
        tz = get_time_zone(conn);
        if (mysql_query(conn, "SET time_zone = '+00:00'") != 0) { *err = db_error_string(conn); goto done; }
        tz_changed = 1;
    }
    if (want_rows) {
        if (!(sql = select_sql(conn, table, qtable, fmt == EXPORT_SQL, err))) goto done;
        if (mysql_real_query(conn, sql, strlen(sql)) != 0) { *err = db_error_string(conn); goto done; }
        if (!(res = mysql_use_result(conn))) { *err = db_error_string(conn); goto done; }
    }

    tmp = malloc(strlen(path) + 16);
    if (!tmp) { *err = strdup("Out of memory"); goto done; }
    snprintf(tmp, strlen(path) + 16, "%s.sqlc-XXXXXX", path);
    int fd = mkstemp(tmp);
    if (fd < 0) { *err = fmt_error("Could not create %s: %s", path, strerror(errno)); free(tmp); tmp = NULL; goto done; }
    fchmod(fd, 0644);
    if (!(f = fdopen(fd, "w"))) { *err = fmt_error("Could not write %s: %s", path, strerror(errno)); close(fd); goto done; }

    unsigned int ncols = res ? mysql_num_fields(res) : 0;
    export_col *cols = calloc(ncols ? ncols : 1, sizeof *cols);
    if (!cols) { *err = strdup("Out of memory"); goto done; }
    MYSQL_FIELD *fields = res ? mysql_fetch_fields(res) : NULL;
    for (unsigned int c = 0; c < ncols; c++) {
        cols[c].name = fields[c].name;
        cols[c].binary = is_binary_field(&fields[c]);
        cols[c].numeric = IS_NUM(fields[c].type) && fields[c].type != MYSQL_TYPE_BIT;
    }
    w = export_begin(f, fmt, &opts, table, create, is_view, ncols, cols);
    free(cols);
    if (!w) { *err = strdup("Out of memory"); goto done; }

    MYSQL_ROW row;
    while (res && (row = mysql_fetch_row(res))) {
        if (export_row(w, row, mysql_fetch_lengths(res)) != 0) { write_errno = errno; break; }
        if (++rows % PROGRESS_EVERY == 0 && progress) progress(rows, data);
    }
    if (!write_errno && res && mysql_errno(conn)) { *err = db_error_string(conn); goto done; }

    int end_failed = export_end(w) != 0;
    w = NULL;
    if (!write_errno && end_failed) write_errno = errno;
    if (!write_errno && (fflush(f) != 0 || fsync(fileno(f)) != 0)) write_errno = errno;
    if (fclose(f) != 0 && !write_errno) write_errno = errno;
    f = NULL;
    if (write_errno) {
        *err = fmt_error("Could not write %s: %s", path, strerror(write_errno));
        goto done;
    }
    if (rename(tmp, path) != 0) { *err = fmt_error("Could not save %s: %s", path, strerror(errno)); goto done; }
    free(tmp);
    tmp = NULL; /* nothing to clean up */
    rc = 0;

done:
    if (w) export_end(w);
    if (f) fclose(f);
    if (tmp) { unlink(tmp); free(tmp); }
    if (res) mysql_free_result(res); /* reads any rows left, keeping conn usable */
    if (tz_changed) {
        if (tz) set_time_zone(conn, tz);
        else mysql_query(conn, "SET time_zone = DEFAULT");
    }
    free(tz);
    free(create);
    free(sql);
    free(qtable);
    if (rows_out) *rows_out = rows;
    if (rc != 0 && !*err) *err = strdup("Export failed");
    return rc;
}
