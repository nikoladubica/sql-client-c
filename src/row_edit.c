#include "row_edit.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---------- column kinds ---------- */

col_kind row_edit_kind(const char *t, int *approx) {
    static const char *const numbers[] = {
        "tinyint", "smallint", "mediumint", "int", "integer", "bigint",
        "decimal", "numeric", "dec", "fixed", "year", NULL };
    static const char *const floats[] = { "float", "double", "real", "double precision", NULL };
    static const char *const binary[] = {
        "binary", "varbinary", "tinyblob", "blob", "mediumblob", "longblob", "bit",
        "geometry", "point", "linestring", "polygon", "multipoint", "multilinestring",
        "multipolygon", "geometrycollection", "geomcollection", "vector", NULL };
    if (approx) *approx = 0;
    if (!t) return COL_TEXT;
    for (int i = 0; numbers[i]; i++) if (strcasecmp(t, numbers[i]) == 0) return COL_NUMBER;
    for (int i = 0; floats[i]; i++)
        if (strcasecmp(t, floats[i]) == 0) { if (approx) *approx = 1; return COL_NUMBER; }
    for (int i = 0; binary[i]; i++) if (strcasecmp(t, binary[i]) == 0) return COL_BINARY;
    if (strcasecmp(t, "date") == 0) return COL_DATE;
    if (strcasecmp(t, "datetime") == 0 || strcasecmp(t, "timestamp") == 0) return COL_DATETIME;
    if (strcasecmp(t, "time") == 0) return COL_TIME;
    return COL_TEXT;
}

void table_info_free(table_info *ti) {
    for (unsigned int i = 0; ti->cols && i < ti->ncols; i++) free(ti->cols[i].name);
    free(ti->cols);
    memset(ti, 0, sizeof *ti);
}

int table_info_load(table_info *ti, const db_set *columns, const db_set *rows) {
    memset(ti, 0, sizeof *ti);
    if (!columns || !rows || columns->ncols < 5 || rows->ncols == 0) return -1;
    ti->cols = calloc(rows->ncols, sizeof *ti->cols);
    if (!ti->cols) return -1;
    ti->ncols = rows->ncols;

    unsigned int pk_total = 0;
    for (size_t r = 0; r < columns->nrows; r++) {
        const char *key = db_cell(columns, r, 3);
        if (key && strcmp(key, "PRI") == 0) pk_total++;
    }
    unsigned char *used = calloc(columns->nrows ? columns->nrows : 1, 1);
    if (!used) { table_info_free(ti); return -1; }
    ti->star = rows->ncols == columns->nrows;

    for (unsigned int c = 0; c < rows->ncols; c++) {
        const char *want = rows->org_names ? rows->org_names[c] : rows->names[c];
        size_t r = columns->nrows;
        if (want)
            for (r = 0; r < columns->nrows; r++) {
                const char *name = db_cell(columns, r, 0);
                if (name && strcmp(name, want) == 0) break;
            }
        /* an expression, or a column shown twice (one cell, two values) */
        if (r == columns->nrows || used[r]) { free(used); table_info_free(ti); return -1; }
        used[r] = 1;
        if (r != c) ti->star = 0;
        col_info *ci = &ti->cols[c];
        const char *nullable = db_cell(columns, r, 2);
        const char *key = db_cell(columns, r, 3);
        const char *extra = db_cell(columns, r, 4);
        ci->name = strdup(db_cell(columns, r, 0)); /* the real name, for the SQL */
        if (!ci->name) { free(used); table_info_free(ti); return -1; }
        ci->kind = row_edit_kind(db_cell(columns, r, 1), &ci->approx);
        ci->nullable = nullable && strcasecmp(nullable, "YES") == 0;
        ci->primary = key && strcmp(key, "PRI") == 0;
        ci->auto_increment = extra && strcasestr(extra, "auto_increment") != NULL;
        /* "VIRTUAL GENERATED", "STORED GENERATED", "PERSISTENT"; not MySQL's
         * DEFAULT_GENERATED (an expression default, still writable) */
        ci->generated = extra && (strcasestr(extra, "VIRTUAL") || strcasestr(extra, "STORED") ||
                                  strcasestr(extra, "PERSISTENT"));
        if (ci->primary) ti->npk++;
    }
    free(used);
    ti->whole = rows->ncols == columns->nrows; /* no repeats: so all of them */
    if (ti->npk != pk_total) { /* part of the key isn't in the rows */
        for (unsigned int c = 0; c < ti->ncols; c++) ti->cols[c].primary = 0;
        ti->npk = 0;
    }
    return 0;
}

int row_edit_editable(const table_info *ti, unsigned int col) {
    if (!ti || col >= ti->ncols) return 0;
    const col_info *c = &ti->cols[col];
    return c->kind != COL_BINARY && !c->generated;
}

/* ---------- typed values ---------- */

static void set_err(char *err, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_err(char *err, size_t n, const char *fmt, ...) {
    if (!err || !n) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

/* Reads 1..max digits at *p into *v. */
static int digits(const char **p, int max, int *v) {
    int n = 0, val = 0;
    while (n < max && isdigit((unsigned char)**p)) { val = val * 10 + (**p - '0'); (*p)++; n++; }
    if (n == 0) return 0;
    *v = val;
    return n;
}

static int is_number(const char *s) {
    const char *p = s;
    if (*p == '+' || *p == '-') p++;
    int intd = 0, fracd = 0;
    while (isdigit((unsigned char)*p)) { p++; intd++; }
    if (*p == '.') { p++; while (isdigit((unsigned char)*p)) { p++; fracd++; } }
    if (intd + fracd == 0) return 0;
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '+' || *p == '-') p++;
        if (!isdigit((unsigned char)*p)) return 0;
        while (isdigit((unsigned char)*p)) p++;
    }
    return *p == '\0';
}

/* YYYY-MM-DD (also / or . between, 1-2 digit month and day); zeros allowed. */
static int parse_date(const char **p, int *y, int *m, int *d) {
    if (digits(p, 4, y) != 4) return 0;
    char sep = **p;
    if (sep != '-' && sep != '/' && sep != '.') return 0;
    (*p)++;
    if (!digits(p, 2, m) || **p != sep) return 0;
    (*p)++;
    if (!digits(p, 2, d)) return 0;
    return *m <= 12 && *d <= 31;
}

/* [-]H[HH]:MM[:SS[.ffffff]]; max_hours limits H. frac gets ".fff" or "". */
static int parse_time(const char **p, int max_hours, int *neg, int *h, int *mi, int *s,
                      char *frac, size_t fracsize) {
    *neg = 0;
    if (max_hours > 24 && **p == '-') { *neg = 1; (*p)++; }
    if (!digits(p, max_hours > 24 ? 3 : 2, h) || *h > max_hours) return 0;
    if (**p != ':') return 0;
    (*p)++;
    if (digits(p, 2, mi) != 2 || *mi > 59) return 0;
    *s = 0;
    frac[0] = '\0';
    if (**p == ':') {
        (*p)++;
        if (digits(p, 2, s) != 2 || *s > 59) return 0;
        if (**p == '.') {
            size_t n = 0;
            (*p)++;
            frac[n++] = '.';
            while (isdigit((unsigned char)**p) && n < 7 && n + 1 < fracsize) frac[n++] = *(*p)++;
            frac[n] = '\0';
            if (n == 1 || isdigit((unsigned char)**p)) return 0; /* none, or more than 6 */
        }
    }
    return 1;
}

char *row_edit_normalize(const col_info *c, const char *text, int *is_null,
                         char *err, size_t errsize) {
    *is_null = 0;
    if (c->generated || c->kind == COL_BINARY) {
        set_err(err, errsize, "Column `%s` can't be edited here.", c->name);
        return NULL;
    }
    if (c->kind == COL_TEXT) {
        char *v = strdup(text);
        if (!v) set_err(err, errsize, "Out of memory");
        return v;
    }

    /* trim */
    while (isspace((unsigned char)*text)) text++;
    size_t len = strlen(text);
    while (len && isspace((unsigned char)text[len - 1])) len--;
    char t[128];
    if (len >= sizeof t) { set_err(err, errsize, "The value for `%s` is too long.", c->name); return NULL; }
    memcpy(t, text, len);
    t[len] = '\0';

    if (len == 0) {
        if (c->nullable) { *is_null = 1; return NULL; }
        set_err(err, errsize, "`%s` can't be NULL: enter a value.", c->name);
        return NULL;
    }

    char out[160];
    const char *p = t;
    int y, m, d, neg, h, mi, s;
    char frac[16];
    switch (c->kind) {
    case COL_NUMBER:
        if (!is_number(t)) {
            set_err(err, errsize, "`%s` holds numbers: \"%s\" isn't one.", c->name, t);
            return NULL;
        }
        snprintf(out, sizeof out, "%s", t);
        break;
    case COL_DATE:
        if (!parse_date(&p, &y, &m, &d) || *p) {
            set_err(err, errsize, "`%s` holds dates: enter YYYY-MM-DD, not \"%s\".", c->name, t);
            return NULL;
        }
        snprintf(out, sizeof out, "%04d-%02d-%02d", y, m, d);
        break;
    case COL_DATETIME:
        if (!parse_date(&p, &y, &m, &d)) goto bad_datetime;
        h = mi = s = 0;
        frac[0] = '\0';
        if (*p == ' ' || *p == 'T') {
            p++;
            while (*p == ' ') p++;
            if (!parse_time(&p, 23, &neg, &h, &mi, &s, frac, sizeof frac)) goto bad_datetime;
        }
        if (*p) goto bad_datetime;
        snprintf(out, sizeof out, "%04d-%02d-%02d %02d:%02d:%02d%s", y, m, d, h, mi, s, frac);
        break;
    bad_datetime:
        set_err(err, errsize, "`%s` holds date and time: enter YYYY-MM-DD HH:MM:SS, not \"%s\".",
                c->name, t);
        return NULL;
    case COL_TIME:
        if (!parse_time(&p, 838, &neg, &h, &mi, &s, frac, sizeof frac) || *p) {
            set_err(err, errsize, "`%s` holds times: enter HH:MM:SS, not \"%s\".", c->name, t);
            return NULL;
        }
        snprintf(out, sizeof out, "%s%02d:%02d:%02d%s", neg ? "-" : "", h, mi, s, frac);
        break;
    default:
        snprintf(out, sizeof out, "%s", t);
        break;
    }
    char *v = strdup(out);
    if (!v) set_err(err, errsize, "Out of memory");
    return v;
}

/* ---------- SQL ---------- */

typedef struct {
    char *s;
    size_t len, cap;
    int oom;
} sbuf;

static void sb_add(sbuf *b, const char *s, size_t n) {
    if (b->oom) return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (b->len + n + 1 > cap) cap *= 2;
        char *ns = realloc(b->s, cap);
        if (!ns) { b->oom = 1; return; }
        b->s = ns;
        b->cap = cap;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = '\0';
}

static void sb_str(sbuf *b, const char *s) { sb_add(b, s, strlen(s)); }

static void sb_ident(sbuf *b, const char *name) {
    sb_add(b, "`", 1);
    for (const char *c = name; *c; c++) {
        if (*c == '`') sb_add(b, "`", 1);
        sb_add(b, c, 1);
    }
    sb_add(b, "`", 1);
}

/* 'escaped string', or NULL; raw = a checked number, written unquoted. */
static void sb_value(sbuf *b, const char *v, int raw, const row_edit_escaper *esc) {
    if (!v) { sb_str(b, "NULL"); return; }
    if (raw) { sb_str(b, v); return; }
    size_t n = strlen(v);
    char *tmp = malloc(2 * n + 1);
    if (!tmp) { b->oom = 1; return; }
    unsigned long m = esc->fn(tmp, v, (unsigned long)n, esc->data);
    if (m == (unsigned long)-1) { free(tmp); b->oom = 1; return; }
    sb_add(b, "'", 1);
    sb_add(b, tmp, m);
    sb_add(b, "'", 1);
    free(tmp);
}

static char *sb_finish(sbuf *b, char *err, size_t errsize) {
    if (b->oom) { free(b->s); set_err(err, errsize, "Out of memory"); return NULL; }
    return b->s;
}

/* Columns usable to find a row again, given its values. */
static int where_col(const table_info *ti, unsigned int c) {
    const col_info *ci = &ti->cols[c];
    if (ti->npk) return ci->primary;
    return ci->kind != COL_BINARY && !ci->approx;
}

static int can_identify(const table_info *ti, char *err, size_t errsize) {
    if (!ti->npk && !ti->whole) { /* some columns only: their values may not be unique */
        set_err(err, errsize, "Add the table's primary key to the query to edit these rows.");
        return 0;
    }
    for (unsigned int c = 0; c < ti->ncols; c++) if (where_col(ti, c)) return 1;
    set_err(err, errsize, "This table has no primary key and no columns that can identify a row.");
    return 0;
}

/* " WHERE ..." for a row given by its ncols values. */
static void sb_where(sbuf *b, const table_info *ti, const char *const *vals,
                     const row_edit_escaper *esc) {
    int first = 1;
    sb_str(b, " WHERE ");
    for (unsigned int c = 0; c < ti->ncols; c++) {
        if (!where_col(ti, c)) continue;
        if (!first) sb_str(b, " AND ");
        first = 0;
        sb_ident(b, ti->cols[c].name);
        if (ti->npk) {
            if (vals[c]) { sb_str(b, " = "); sb_value(b, vals[c], 0, esc); }
            else sb_str(b, " IS NULL");
        } else {
            sb_str(b, " <=> ");
            sb_value(b, vals[c], 0, esc);
        }
    }
}

/* "SELECT <the rows' columns> FROM `table`", to read a changed row back the
 * way the grid shows it ("SELECT *" when that's the same). */
static void sb_select(sbuf *b, const table_info *ti, const char *table) {
    sb_str(b, "SELECT ");
    if (ti->star) {
        sb_str(b, "*");
    } else {
        for (unsigned int c = 0; c < ti->ncols; c++) {
            if (c) sb_str(b, ", ");
            sb_ident(b, ti->cols[c].name);
        }
    }
    sb_str(b, " FROM ");
    sb_ident(b, table);
}

static const char *const *row_values(const db_set *set, size_t row) {
    return (const char *const *)set->cells + row * set->ncols;
}

static int value_raw(const col_info *c, const char *v) {
    return v && c->kind == COL_NUMBER && is_number(v);
}

char *row_edit_update_sql(const char *table, const table_info *ti, const db_set *set,
                          size_t row, unsigned int col, const char *value,
                          const row_edit_escaper *esc, char *err, size_t errsize) {
    if (!ti || ti->ncols != set->ncols || row >= set->nrows || !row_edit_editable(ti, col)) {
        set_err(err, errsize, "This cell can't be edited.");
        return NULL;
    }
    if (!can_identify(ti, err, errsize)) return NULL;

    const char *const *old = row_values(set, row);
    const char **now = malloc(set->ncols * sizeof *now);
    if (!now) { set_err(err, errsize, "Out of memory"); return NULL; }
    memcpy(now, old, set->ncols * sizeof *now);
    now[col] = value;

    sbuf b = { 0 };
    sb_str(&b, "UPDATE ");
    sb_ident(&b, table);
    sb_str(&b, " SET ");
    sb_ident(&b, ti->cols[col].name);
    sb_str(&b, " = ");
    sb_value(&b, value, value_raw(&ti->cols[col], value), esc);
    sb_where(&b, ti, old, esc);
    sb_str(&b, " LIMIT 1;\n");
    sb_select(&b, ti, table);
    sb_where(&b, ti, now, esc);
    sb_str(&b, " LIMIT 1");
    free(now);
    return sb_finish(&b, err, errsize);
}

char *row_edit_insert_sql(const char *table, const table_info *ti, const db_set *set,
                          size_t row, const unsigned char *is_set,
                          const row_edit_escaper *esc, char *err, size_t errsize) {
    if (!ti || ti->ncols != set->ncols || row >= set->nrows) {
        set_err(err, errsize, "This row can't be inserted.");
        return NULL;
    }
    const char *const *vals = row_values(set, row);
    sbuf b = { 0 };
    sb_str(&b, "INSERT INTO ");
    sb_ident(&b, table);
    sb_str(&b, " (");
    int n = 0;
    for (unsigned int c = 0; c < ti->ncols; c++) {
        if (!is_set[c] || !row_edit_editable(ti, c)) continue;
        if (n++) sb_str(&b, ", ");
        sb_ident(&b, ti->cols[c].name);
    }
    sb_str(&b, ") VALUES (");
    n = 0;
    for (unsigned int c = 0; c < ti->ncols; c++) {
        if (!is_set[c] || !row_edit_editable(ti, c)) continue;
        if (n++) sb_str(&b, ", ");
        sb_value(&b, vals[c], value_raw(&ti->cols[c], vals[c]), esc);
    }
    sb_str(&b, ")");

    /* Read it back by its key: typed in, or the auto-increment value. */
    int findable = ti->npk > 0;
    for (unsigned int c = 0; c < ti->ncols && findable; c++) {
        const col_info *ci = &ti->cols[c];
        if (ci->primary && !(is_set[c] && vals[c]) && !ci->auto_increment) findable = 0;
    }
    if (findable) {
        sb_str(&b, ";\n");
        sb_select(&b, ti, table);
        sb_str(&b, " WHERE ");
        int first = 1;
        for (unsigned int c = 0; c < ti->ncols; c++) {
            const col_info *ci = &ti->cols[c];
            if (!ci->primary) continue;
            if (!first) sb_str(&b, " AND ");
            first = 0;
            sb_ident(&b, ci->name);
            sb_str(&b, " = ");
            if (is_set[c] && vals[c]) sb_value(&b, vals[c], 0, esc);
            else sb_str(&b, "LAST_INSERT_ID()");
        }
        sb_str(&b, " LIMIT 1");
    }
    return sb_finish(&b, err, errsize);
}

char *row_edit_delete_sql(const char *table, const table_info *ti, const db_set *set,
                          const size_t *rows, size_t nrows,
                          const row_edit_escaper *esc, char *err, size_t errsize) {
    if (!ti || ti->ncols != set->ncols || nrows == 0) {
        set_err(err, errsize, "No rows to delete.");
        return NULL;
    }
    if (!can_identify(ti, err, errsize)) return NULL;
    sbuf b = { 0 };
    sb_str(&b, "START TRANSACTION;\n");
    for (size_t i = 0; i < nrows; i++) {
        if (rows[i] >= set->nrows) continue;
        sb_str(&b, "DELETE FROM ");
        sb_ident(&b, table);
        sb_where(&b, ti, row_values(set, rows[i]), esc);
        sb_str(&b, " LIMIT 1;\n");
    }
    sb_str(&b, "COMMIT");
    return sb_finish(&b, err, errsize);
}
