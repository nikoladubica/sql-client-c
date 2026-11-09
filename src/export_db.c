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
        char *qc = row[0] ? db_quote_ident(row[0]) : NULL;
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

/*
 * Write one table or view into the open file f (the export_writer part of
 * an export). create is its CREATE statement if the caller already has it,
 * or NULL to ask the server. Returns 0; or -1 with *err set (server error,
 * out of memory) or with *write_errno set (writing f failed). *rows_out is
 * the number of rows written either way.
 */
static int write_table(MYSQL *conn, FILE *f, const char *table, int is_view,
                       export_format fmt, const export_opts *opts, const char *create,
                       export_progress_fn progress, void *data,
                       unsigned long long *rows_out, int *write_errno, char **err) {
    unsigned long long rows = 0;
    char *qtable = db_quote_ident(table);
    char *own_create = NULL, *sql = NULL, *tz = NULL;
    export_writer *w = NULL;
    MYSQL_RES *res = NULL;
    int rc = -1, tz_changed = 0;
    int want_rows = fmt != EXPORT_SQL || (opts->sql_data && !is_view);

    if (!qtable) { *err = strdup("Out of memory"); goto done; }

    if (fmt == EXPORT_SQL && opts->sql_structure && !create) {
        if (!(own_create = show_create(conn, qtable, err))) goto done;
        create = own_create;
    }
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

    unsigned int ncols = res ? mysql_num_fields(res) : 0;
    export_col *cols = calloc(ncols ? ncols : 1, sizeof *cols);
    if (!cols) { *err = strdup("Out of memory"); goto done; }
    MYSQL_FIELD *fields = res ? mysql_fetch_fields(res) : NULL;
    for (unsigned int c = 0; c < ncols; c++) {
        cols[c].name = fields[c].name;
        cols[c].binary = is_binary_field(&fields[c]);
        cols[c].numeric = IS_NUM(fields[c].type) && fields[c].type != MYSQL_TYPE_BIT;
    }
    w = export_begin(f, fmt, opts, table, create, is_view, ncols, cols);
    free(cols);
    if (!w) { *err = strdup("Out of memory"); goto done; }

    MYSQL_ROW row;
    while (res && (row = mysql_fetch_row(res))) {
        if (export_row(w, row, mysql_fetch_lengths(res)) != 0) {
            *write_errno = errno ? errno : EIO;
            goto done;
        }
        if (++rows % PROGRESS_EVERY == 0 && progress) progress(rows, data);
    }
    if (res && mysql_errno(conn)) { *err = db_error_string(conn); goto done; }

    int end_failed = export_end(w) != 0;
    w = NULL;
    if (end_failed) { *write_errno = errno ? errno : EIO; goto done; }
    rc = 0;

done:
    if (w) export_end(w);
    if (res) mysql_free_result(res); /* reads any rows left, keeping conn usable */
    if (tz_changed) {
        if (tz) set_time_zone(conn, tz);
        else mysql_query(conn, "SET time_zone = DEFAULT");
    }
    free(tz);
    free(own_create);
    free(sql);
    free(qtable);
    *rows_out = rows;
    return rc;
}

/* A temporary file next to path ("<path>.sqlc-XXXXXX"), opened for writing.
 * Returns the FILE with *tmp set to its name (malloc'd), or NULL with *err set. */
static FILE *open_temp(const char *path, char **tmp, char **err) {
    size_t n = strlen(path) + 16;
    *tmp = malloc(n);
    if (!*tmp) { *err = strdup("Out of memory"); return NULL; }
    snprintf(*tmp, n, "%s.sqlc-XXXXXX", path);
    int fd = mkstemp(*tmp);
    if (fd < 0) {
        *err = fmt_error("Could not create %s: %s", path, strerror(errno));
        free(*tmp);
        *tmp = NULL;
        return NULL;
    }
    fchmod(fd, 0644);
    FILE *f = fdopen(fd, "w");
    if (!f) {
        *err = fmt_error("Could not write %s: %s", path, strerror(errno));
        close(fd);
        unlink(*tmp);
        free(*tmp);
        *tmp = NULL;
    }
    return f;
}

/* Close f; if ok (and every write went through) move tmp over path, else
 * remove tmp. Frees tmp. Returns 0, or -1 with *err set (unless the caller
 * already set it). */
static int finish_temp(FILE *f, char *tmp, const char *path, int ok, int write_errno,
                       char **err) {
    int rc = -1;
    if (ok && !write_errno && (fflush(f) != 0 || fsync(fileno(f)) != 0)) write_errno = errno ? errno : EIO;
    if (fclose(f) != 0 && ok && !write_errno) write_errno = errno ? errno : EIO;
    if (write_errno) {
        if (!*err) *err = fmt_error("Could not write %s: %s", path, strerror(write_errno));
    } else if (ok) {
        if (rename(tmp, path) == 0) rc = 0;
        else *err = fmt_error("Could not save %s: %s", path, strerror(errno));
    }
    if (rc != 0) unlink(tmp);
    free(tmp);
    return rc;
}

int export_table(MYSQL *conn, const char *table, int is_view, export_format fmt,
                 const export_opts *opts_in, const char *path,
                 export_progress_fn progress, void *data,
                 unsigned long long *rows_out, char **err) {
    export_opts opts = opts_in ? *opts_in : export_default_opts();
    opts.sql_in_dump = 0;
    unsigned long long rows = 0;
    int rc = -1, write_errno = 0;
    char *tmp = NULL;
    *err = NULL;

    FILE *f = open_temp(path, &tmp, err);
    if (f) {
        int ok = write_table(conn, f, table, is_view, fmt, &opts, NULL, progress, data,
                             &rows, &write_errno, err) == 0;
        rc = finish_temp(f, tmp, path, ok, write_errno, err);
    }
    if (rows_out) *rows_out = rows;
    if (rc != 0 && !*err) *err = strdup("Export failed");
    return rc;
}

/* ---------- whole database ---------- */

typedef struct {
    char *name, *type;
    export_obj_kind kind;
    char *create;              /* views: CREATE VIEW, fetched up front */
} db_object;

static void objects_free(db_object *o, size_t n) {
    for (size_t i = 0; o && i < n; i++) {
        free(o[i].name);
        free(o[i].type);
        free(o[i].create);
    }
    free(o);
}

/* The current database's tables and views, by name (malloc'd array). */
static db_object *list_objects(MYSQL *conn, size_t *n_out, char **err) {
    *n_out = 0;
    MYSQL_RES *r = query_store(conn,
        "SELECT TABLE_NAME, TABLE_TYPE FROM information_schema.TABLES "
        "WHERE TABLE_SCHEMA = DATABASE() ORDER BY TABLE_NAME", err);
    if (!r) return NULL;
    size_t n = (size_t)mysql_num_rows(r);
    db_object *o = calloc(n ? n : 1, sizeof *o);
    if (!o) { mysql_free_result(r); *err = strdup("Out of memory"); return NULL; }
    size_t k = 0;
    MYSQL_ROW row;
    while (k < n && (row = mysql_fetch_row(r))) {
        if (!row[0]) continue;
        o[k].name = strdup(row[0]);
        o[k].type = row[1] ? strdup(row[1]) : NULL;
        o[k].kind = export_object_kind(row[1]);
        if (!o[k].name || (row[1] && !o[k].type)) {
            objects_free(o, k + 1);
            mysql_free_result(r);
            *err = strdup("Out of memory");
            return NULL;
        }
        k++;
    }
    mysql_free_result(r);
    *n_out = k;
    return o;
}

typedef struct {
    export_db_progress_fn fn;
    void *data;
    const char *table;
    size_t index, count;
} db_progress;

static void table_progress(unsigned long long rows, void *data) {
    db_progress *p = data;
    p->fn(p->table, p->index, p->count, rows, p->data);
}

/* export_database (nsel == 0) and export_tables (the nsel names in sel). */
static int dump(MYSQL *conn, const char *const *sel, size_t nsel,
                const export_opts *opts_in, const char *path,
                export_db_progress_fn progress, void *data,
                export_db_stats *stats_out, char **err) {
    export_opts opts = opts_in ? *opts_in : export_default_opts();
    opts.sql_in_dump = 1;
    export_db_stats stats = { 0 };
    char *db = NULL, *tmp = NULL;
    db_object *objs = NULL;
    size_t nobjs = 0, *view_order = NULL;
    const char **vnames = NULL, **vdefs = NULL;
    size_t *vidx = NULL, nviews = 0, ntables = 0;
    FILE *f = NULL;
    int rc = -1, write_errno = 0, ok = 0;
    *err = NULL;

    /* Everything that can fail on the server up front, before the file. */
    MYSQL_RES *r = query_store(conn, "SELECT DATABASE()", err);
    if (!r) goto done;
    MYSQL_ROW row = mysql_fetch_row(r);
    if (row && row[0] && !(db = strdup(row[0]))) *err = strdup("Out of memory");
    mysql_free_result(r);
    if (*err) goto done;
    if (!db) { *err = strdup("No database selected"); goto done; }

    if (!(objs = list_objects(conn, &nobjs, err))) goto done;
    if (nsel) { /* only the selected objects: drop the others from the list */
        const char **names = calloc(nobjs ? nobjs : 1, sizeof *names);
        unsigned char *keep = calloc(nobjs ? nobjs : 1, 1);
        size_t missing = 0;
        int src = -1;
        if (names && keep) {
            for (size_t i = 0; i < nobjs; i++) names[i] = objs[i].name;
            src = export_select_names(nobjs, names, nsel, sel, keep, &missing);
        }
        if (src == 0) {
            size_t k = 0;
            for (size_t i = 0; i < nobjs; i++) {
                if (keep[i]) { objs[k++] = objs[i]; continue; }
                free(objs[i].name);
                free(objs[i].type);
                free(objs[i].create);
            }
            nobjs = k;
        } else if (src > 0) {
            *err = fmt_error("Table %s not found in database %s (dropped or renamed?) - "
                             "reload the table list and select again",
                             sel[missing] ? sel[missing] : "", db);
        } else {
            *err = strdup("Out of memory");
        }
        free(keep);
        free(names);
        if (*err) goto done;
    }
    for (size_t i = 0; i < nobjs; i++) {
        if (objs[i].kind == EXPORT_OBJ_TABLE) ntables++;
        else if (objs[i].kind == EXPORT_OBJ_VIEW && opts.sql_structure) nviews++;
    }

    /* Views go after all tables, each after the views it uses. */
    vnames = calloc(nviews ? nviews : 1, sizeof *vnames);
    vdefs = calloc(nviews ? nviews : 1, sizeof *vdefs);
    vidx = calloc(nviews ? nviews : 1, sizeof *vidx);
    view_order = calloc(nviews ? nviews : 1, sizeof *view_order);
    if (!vnames || !vdefs || !vidx || !view_order) { *err = strdup("Out of memory"); goto done; }
    size_t v = 0;
    for (size_t i = 0; i < nobjs && v < nviews; i++) {
        if (objs[i].kind != EXPORT_OBJ_VIEW) continue;
        char *q = db_quote_ident(objs[i].name);
        if (!q) { *err = strdup("Out of memory"); goto done; }
        objs[i].create = show_create(conn, q, err);
        free(q);
        if (!objs[i].create) goto done;
        vnames[v] = objs[i].name;
        vdefs[v] = objs[i].create;
        vidx[v++] = i;
    }
    if (export_view_order(nviews, vnames, vdefs, view_order) != 0) {
        *err = strdup("Out of memory");
        goto done;
    }

    if (!(f = open_temp(path, &tmp, err))) goto done;
    export_sql_dump_header(f, db, ntables, nviews, nsel != 0);

    db_progress dp = { .fn = progress, .data = data, .count = ntables + nviews };
    size_t index = 0;
    for (size_t i = 0; i < nobjs; i++) {
        if (objs[i].kind == EXPORT_OBJ_SKIP) {
            export_sql_dump_skipped(f, objs[i].name, objs[i].type);
            stats.skipped++;
            continue;
        }
        if (objs[i].kind != EXPORT_OBJ_TABLE) continue;
        dp.table = objs[i].name;
        dp.index = ++index;
        if (progress) progress(dp.table, dp.index, dp.count, 0, data);
        unsigned long long rows = 0;
        int trc = write_table(conn, f, objs[i].name, 0, EXPORT_SQL, &opts, NULL,
                              progress ? table_progress : NULL, &dp,
                              &rows, &write_errno, err);
        stats.rows += rows;
        if (trc != 0) {
            if (*err) {
                char *e = fmt_error("Table %s: %s", objs[i].name, *err);
                if (e) { free(*err); *err = e; }
            }
            goto done;
        }
        stats.tables++;
    }
    for (size_t k = 0; k < nviews; k++) {
        db_object *o = &objs[vidx[view_order[k]]];
        dp.table = o->name;
        dp.index = ++index;
        if (progress) progress(dp.table, dp.index, dp.count, 0, data);
        unsigned long long rows = 0;
        if (write_table(conn, f, o->name, 1, EXPORT_SQL, &opts, o->create, NULL, NULL,
                        &rows, &write_errno, err) != 0) {
            if (*err) {
                char *e = fmt_error("View %s: %s", o->name, *err);
                if (e) { free(*err); *err = e; }
            }
            goto done;
        }
        stats.views++;
    }
    export_sql_dump_footer(f);
    if (ferror(f)) write_errno = errno ? errno : EIO;
    ok = !write_errno;

done:
    if (f) rc = finish_temp(f, tmp, path, ok, write_errno, err);
    free(view_order);
    free(vidx);
    free(vdefs);
    free(vnames);
    objects_free(objs, nobjs);
    free(db);
    if (stats_out) *stats_out = stats;
    if (rc != 0 && !*err) *err = strdup("Export failed");
    return rc;
}

int export_database(MYSQL *conn, const export_opts *opts, const char *path,
                    export_db_progress_fn progress, void *data,
                    export_db_stats *stats, char **err) {
    return dump(conn, NULL, 0, opts, path, progress, data, stats, err);
}

int export_tables(MYSQL *conn, const char *const *names, size_t n,
                  const export_opts *opts, const char *path,
                  export_db_progress_fn progress, void *data,
                  export_db_stats *stats, char **err) {
    if (n == 0) {
        *err = strdup("No tables selected");
        if (stats) memset(stats, 0, sizeof *stats);
        return -1;
    }
    return dump(conn, names, n, opts, path, progress, data, stats, err);
}
