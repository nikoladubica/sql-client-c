#include "browse.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const op_labels[BROWSE_NOPS] = {
    "=", "!=", "<", ">", "<=", ">=", "LIKE", "NOT LIKE", "IS NULL", "IS NOT NULL" };

/* as written in the SQL */
static const char *const op_sql[BROWSE_NOPS] = {
    " = ", " <> ", " < ", " > ", " <= ", " >= ", " LIKE ", " NOT LIKE ", " IS NULL", " IS NOT NULL" };

static int op_valid(browse_op op) { return (int)op >= 0 && op < BROWSE_NOPS; }

const char *browse_op_label(browse_op op) {
    return op_valid(op) ? op_labels[op] : NULL;
}

int browse_op_takes_value(browse_op op) {
    return op_valid(op) && op != BROWSE_IS_NULL && op != BROWSE_IS_NOT_NULL;
}

int browse_filter_active(const browse_filter *f) {
    if (!f || !f->column || !*f->column || !op_valid(f->op)) return 0;
    if (!browse_op_takes_value(f->op)) return 1;
    return f->value && *f->value;
}

/* ---------- SQL ---------- */

typedef struct {
    char *s;
    size_t len, cap;
    int oom;
} sbuf;

static void sb_add(sbuf *b, const char *s, size_t n) {
    if (b->oom) return;
    if (n > SIZE_MAX - 1 - b->len) { b->oom = 1; return; }
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n + 1) {
            if (cap > SIZE_MAX / 2) { cap = b->len + n + 1; break; }
            cap *= 2;
        }
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

/* `name` with backticks doubled (what db_quote_ident does; db.c isn't
 * linked into the unit tests). */
static void sb_ident(sbuf *b, const char *name) {
    sb_add(b, "`", 1);
    for (const char *c = name; *c; c++) {
        if (*c == '`') sb_add(b, "`", 1);
        sb_add(b, c, 1);
    }
    sb_add(b, "`", 1);
}

/* 'escaped value' */
static void sb_value(sbuf *b, const char *v, const row_edit_escaper *esc) {
    size_t n = strlen(v);
    if (n > (SIZE_MAX - 1) / 2 || n > ULONG_MAX / 2) { b->oom = 1; return; }
    char *tmp = malloc(2 * n + 1);
    if (!tmp) { b->oom = 1; return; }
    unsigned long m = esc->fn(tmp, v, (unsigned long)n, esc->data);
    if (m == (unsigned long)-1) { free(tmp); b->oom = 1; return; }
    sb_add(b, "'", 1);
    sb_add(b, tmp, m);
    sb_add(b, "'", 1);
    free(tmp);
}

static void set_err(char *err, size_t errsize, const char *msg) {
    if (err && errsize) snprintf(err, errsize, "%s", msg);
}

char *browse_select_sql(const char *table, const browse_filter *f,
                        const char *const *order, size_t norder,
                        unsigned long long offset, unsigned long limit,
                        const row_edit_escaper *esc, char *err, size_t errsize) {
    if (!table || !*table) { set_err(err, errsize, "No table to browse."); return NULL; }
    int filtered = browse_filter_active(f);
    if (f && f->column && !op_valid(f->op)) { set_err(err, errsize, "Unknown filter operator."); return NULL; }
    if (filtered && browse_op_takes_value(f->op) && (!esc || !esc->fn)) {
        set_err(err, errsize, "No way to escape the filter value.");
        return NULL;
    }

    sbuf b = { 0 };
    char num[64];
    sb_str(&b, "SELECT * FROM ");
    sb_ident(&b, table);
    if (filtered) {
        sb_str(&b, " WHERE ");
        sb_ident(&b, f->column);
        sb_str(&b, op_sql[f->op]);
        if (browse_op_takes_value(f->op)) {
            /* LIKE without a wildcard: a "contains" search, '%v%' */
            int wrap = (f->op == BROWSE_LIKE || f->op == BROWSE_NOT_LIKE) &&
                       !strpbrk(f->value, "%_");
            char *wrapped = NULL;
            if (wrap) {
                size_t n = strlen(f->value);
                if (n > SIZE_MAX - 3 || !(wrapped = malloc(n + 3))) b.oom = 1;
                else {
                    wrapped[0] = '%';
                    memcpy(wrapped + 1, f->value, n);
                    wrapped[n + 1] = '%';
                    wrapped[n + 2] = '\0';
                }
            }
            if (!b.oom) sb_value(&b, wrapped ? wrapped : f->value, esc);
            free(wrapped);
        }
    }
    for (size_t i = 0; order && i < norder; i++) {
        sb_str(&b, i ? ", " : " ORDER BY ");
        sb_ident(&b, order[i] ? order[i] : "");
    }
    snprintf(num, sizeof num, " LIMIT %lu", limit);
    sb_str(&b, num);
    if (offset > 0) {
        snprintf(num, sizeof num, " OFFSET %llu", offset);
        sb_str(&b, num);
    }
    sb_str(&b, ";");
    if (b.oom) { free(b.s); set_err(err, errsize, "Out of memory"); return NULL; }
    return b.s;
}

/* ---------- key columns ---------- */

static int is_key(const db_set *columns, size_t r) {
    const char *key = db_cell(columns, r, 3);
    return key && strcmp(key, "PRI") == 0 && db_cell(columns, r, 0);
}

int browse_key_columns(const db_set *columns, const char ***keys, size_t *nkeys) {
    *keys = NULL;
    *nkeys = 0;
    if (!columns || columns->ncols < 4) return -1;
    size_t n = 0;
    for (size_t r = 0; r < columns->nrows; r++) if (is_key(columns, r)) n++;
    if (n == 0) return 0;
    const char **k = calloc(n, sizeof *k);
    if (!k) return -1;
    size_t i = 0;
    for (size_t r = 0; r < columns->nrows; r++)
        if (is_key(columns, r)) k[i++] = db_cell(columns, r, 0);
    *keys = k;
    *nkeys = n;
    return 0;
}

size_t browse_default_column(const db_set *columns) {
    if (!columns || columns->ncols < 4) return 0;
    for (size_t r = 0; r < columns->nrows; r++) if (is_key(columns, r)) return r;
    return 0;
}

/* ---------- page label ---------- */

/* v with a ',' every three digits ("1,234,567"). out holds at least 32 bytes. */
static void thousands(char *out, size_t size, unsigned long long v) {
    char digits[32];
    int n = snprintf(digits, sizeof digits, "%llu", v);
    if (n < 0 || (size_t)n >= sizeof digits) { snprintf(out, size, "%llu", v); return; }
    char tmp[48];
    size_t o = 0;
    for (int i = 0; i < n; i++) {
        if (i > 0 && (n - i) % 3 == 0) tmp[o++] = ',';
        tmp[o++] = digits[i];
    }
    tmp[o] = '\0';
    snprintf(out, size, "%s", tmp);
}

void browse_page_label(char *buf, size_t size, unsigned long long offset, size_t nrows) {
    if (!buf || !size) return;
    if (nrows == 0) { snprintf(buf, size, "No rows"); return; }
    unsigned long long first = offset < ULLONG_MAX ? offset + 1 : ULLONG_MAX;
    unsigned long long last = (unsigned long long)nrows > ULLONG_MAX - offset
                              ? ULLONG_MAX : offset + (unsigned long long)nrows;
    char a[32], z[32];
    thousands(a, sizeof a, first);
    thousands(z, sizeof z, last);
    snprintf(buf, size, "Rows %s\xE2\x80\x93%s", a, z); /* en dash */
}
