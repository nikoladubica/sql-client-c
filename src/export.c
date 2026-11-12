#include "export.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "platform.h"

/* An INSERT is closed after this many rows or (roughly) bytes, well below
 * the default max_allowed_packet (16 MB in MariaDB/MySQL 8). */
#define SQL_BATCH_ROWS  1000
#define SQL_BATCH_BYTES (512 * 1024)

struct export_writer {
    FILE *f;
    export_format fmt;
    export_opts opts;
    char *table;
    int is_view;
    unsigned int ncols;
    export_col *cols;     /* with copied names */
    /* SQL batching */
    int in_insert;
    int wrote_insert;     /* any INSERT written (dump: blank line at the end) */
    size_t batch_rows, batch_bytes;
};

/* Session settings around a SQL file, so it loads in any table order and
 * TIMESTAMPs (read in UTC) come back unchanged. */
static const char SQL_SESSION_BEGIN[] =
    "SET NAMES utf8mb4;\n"
    "SET @OLD_TIME_ZONE=@@TIME_ZONE, TIME_ZONE='+00:00';\n"
    "SET @OLD_SQL_MODE=@@SQL_MODE, SQL_MODE='NO_AUTO_VALUE_ON_ZERO';\n"
    "SET @OLD_FOREIGN_KEY_CHECKS=@@FOREIGN_KEY_CHECKS, FOREIGN_KEY_CHECKS=0;\n\n";
static const char SQL_SESSION_END[] =
    "SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS;\n"
    "SET SQL_MODE=@OLD_SQL_MODE;\n"
    "SET TIME_ZONE=@OLD_TIME_ZONE;\n";

export_opts export_default_opts(void) {
    export_opts o = { .sql_structure = 1, .sql_drop = 1, .sql_data = 1,
                      .csv_null_as_n = 0, .csv_bom = 1 };
    return o;
}

const char *export_extension(export_format fmt) {
    switch (fmt) {
    case EXPORT_SQL: return "sql";
    case EXPORT_CSV: return "csv";
    case EXPORT_XML: return "xml";
    }
    return "txt";
}

/* ---------- escaping ---------- */

void export_sql_string(FILE *f, const char *s, unsigned long len) {
    putc('\'', f);
    for (unsigned long i = 0; i < len; i++) {
        char c = s[i];
        switch (c) {
        case '\0':   fputs("\\0", f); break;
        case '\n':   fputs("\\n", f); break;
        case '\r':   fputs("\\r", f); break;
        case '\\':   fputs("\\\\", f); break;
        case '\'':   fputs("\\'", f); break;
        case '"':    fputs("\\\"", f); break;
        case '\032': fputs("\\Z", f); break;
        default:     putc(c, f);
        }
    }
    putc('\'', f);
}

void export_sql_ident(FILE *f, const char *name) {
    putc('`', f);
    for (const char *c = name; *c; c++) {
        if (*c == '`') putc('`', f);
        putc(*c, f);
    }
    putc('`', f);
}

void export_sql_comment_ident(FILE *f, const char *name) {
    putc('`', f);
    for (const char *c = name; *c; c++) {
        unsigned char u = (unsigned char)*c;
        if (*c == '`') putc('`', f);
        putc(u < 0x20 || u == 0x7F ? '?' : *c, f);
    }
    putc('`', f);
}

static void write_hex(FILE *f, const char *s, unsigned long len) {
    static const char digits[] = "0123456789ABCDEF";
    for (unsigned long i = 0; i < len; i++) {
        unsigned char b = (unsigned char)s[i];
        putc(digits[b >> 4], f);
        putc(digits[b & 15], f);
    }
}

void export_csv_field(FILE *f, const char *s, unsigned long len) {
    int quote = len == 0; /* "" marks an empty string, as opposed to NULL */
    for (unsigned long i = 0; i < len && !quote; i++)
        if (s[i] == ',' || s[i] == '"' || s[i] == '\r' || s[i] == '\n') quote = 1;
    if (!quote) { fwrite(s, 1, len, f); return; }
    putc('"', f);
    for (unsigned long i = 0; i < len; i++) {
        if (s[i] == '"') putc('"', f);
        putc(s[i], f);
    }
    putc('"', f);
}

/* Decode one UTF-8 sequence at s[0..len). Returns its length and sets *cp,
 * or returns 0 if it's invalid (bad lead/continuation, overlong, surrogate,
 * beyond U+10FFFF). */
static unsigned long utf8_decode(const unsigned char *s, unsigned long len, unsigned long *cp) {
    unsigned char c = s[0];
    unsigned long n, v, min;
    if (c < 0x80) { *cp = c; return 1; }
    else if ((c & 0xE0) == 0xC0) { n = 2; v = c & 0x1F; min = 0x80; }
    else if ((c & 0xF0) == 0xE0) { n = 3; v = c & 0x0F; min = 0x800; }
    else if ((c & 0xF8) == 0xF0) { n = 4; v = c & 0x07; min = 0x10000; }
    else return 0;
    if (len < n) return 0;
    for (unsigned long i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0;
        v = (v << 6) | (s[i] & 0x3F);
    }
    if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) return 0;
    *cp = v;
    return n;
}

static int xml_char_ok(unsigned long cp) {
    return cp == 0x9 || cp == 0xA || cp == 0xD || (cp >= 0x20 && cp <= 0xD7FF) ||
           (cp >= 0xE000 && cp <= 0xFFFD) || (cp >= 0x10000 && cp <= 0x10FFFF);
}

void export_xml_text(FILE *f, const char *s, unsigned long len) {
    const unsigned char *u = (const unsigned char *)s;
    unsigned long i = 0;
    while (i < len) {
        unsigned long cp;
        unsigned long n = utf8_decode(u + i, len - i, &cp);
        if (n == 0 || !xml_char_ok(cp)) {
            fputs("\xEF\xBF\xBD", f); /* U+FFFD */
            i += n ? n : 1;
            continue;
        }
        switch (cp) {
        case '&':  fputs("&amp;", f); break;
        case '<':  fputs("&lt;", f); break;
        case '>':  fputs("&gt;", f); break;
        case '"':  fputs("&quot;", f); break;
        case '\'': fputs("&apos;", f); break;
        /* as references, so parsers don't normalize them away */
        case '\r': fputs("&#13;", f); break;
        case '\n': fputs("&#10;", f); break;
        case '\t': fputs("&#9;", f); break;
        default:   fwrite(s + i, 1, n, f);
        }
        i += n;
    }
}

static void xml_attr(FILE *f, const char *s) { export_xml_text(f, s, strlen(s)); }

/* A number from the server is safe to write as is; anything else gets quoted. */
static int plain_number(const char *s, unsigned long len) {
    if (len == 0) return 0;
    for (unsigned long i = 0; i < len; i++)
        if (!strchr("0123456789.-+eE", s[i])) return 0;
    return 1;
}

/* ---------- writer ---------- */

static void writer_free(export_writer *w) {
    for (unsigned int c = 0; c < w->ncols; c++) free((char *)w->cols[c].name);
    free(w->cols);
    free(w->table);
    free(w);
}

static void sql_header(export_writer *w, const char *create_sql) {
    FILE *f = w->f;
    if (w->opts.sql_in_dump) {
        fputs(w->is_view ? "-- View " : "-- Table ", f);
        export_sql_comment_ident(f, w->table);
        putc('\n', f);
    } else {
        fputs("-- sqlc export of ", f);
        fputs(w->is_view ? "view " : "table ", f);
        export_sql_comment_ident(f, w->table);
        fputs("\n\n", f);
        fputs(SQL_SESSION_BEGIN, f);
    }
    if (w->opts.sql_structure) {
        if (w->opts.sql_drop) {
            fputs(w->is_view ? "DROP VIEW IF EXISTS " : "DROP TABLE IF EXISTS ", f);
            export_sql_ident(f, w->table);
            fputs(";\n", f);
        }
        if (create_sql) fprintf(f, "%s;\n\n", create_sql);
    }
}

export_writer *export_begin(FILE *f, export_format fmt, const export_opts *opts,
                            const char *table, const char *create_sql, int is_view,
                            unsigned int ncols, const export_col *cols) {
    export_writer *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->f = f;
    w->fmt = fmt;
    w->opts = opts ? *opts : export_default_opts();
    w->table = strdup(table ? table : "");
    w->is_view = is_view;
    w->ncols = ncols;
    w->cols = calloc(ncols ? ncols : 1, sizeof *w->cols);
    if (!w->table || !w->cols) { free(w->table); free(w->cols); free(w); return NULL; }
    for (unsigned int c = 0; c < ncols; c++) {
        w->cols[c] = cols[c];
        w->cols[c].name = strdup(cols[c].name ? cols[c].name : "");
        if (!w->cols[c].name) { w->ncols = c; writer_free(w); return NULL; }
    }

    switch (fmt) {
    case EXPORT_SQL:
        sql_header(w, create_sql);
        break;
    case EXPORT_CSV:
        if (w->opts.csv_bom) fputs("\xEF\xBB\xBF", f);
        for (unsigned int c = 0; c < ncols; c++) {
            if (c) putc(',', f);
            export_csv_field(f, w->cols[c].name, strlen(w->cols[c].name));
        }
        fputs("\r\n", f);
        break;
    case EXPORT_XML:
        fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<table name=\"", f);
        xml_attr(f, w->table);
        fputs("\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">\n", f);
        break;
    }
    return w;
}

static void sql_close_insert(export_writer *w) {
    if (!w->in_insert) return;
    fputs(";\n", w->f);
    w->in_insert = 0;
}

static void sql_row(export_writer *w, char **vals, const unsigned long *lens) {
    FILE *f = w->f;
    if (w->is_view || !w->opts.sql_data) return;
    if (!w->in_insert) {
        fputs("INSERT INTO ", f);
        export_sql_ident(f, w->table);
        fputs(" (", f);
        for (unsigned int c = 0; c < w->ncols; c++) {
            if (c) fputs(", ", f);
            export_sql_ident(f, w->cols[c].name);
        }
        fputs(") VALUES\n", f);
        w->in_insert = 1;
        w->wrote_insert = 1;
        w->batch_rows = w->batch_bytes = 0;
    } else {
        fputs(",\n", f);
    }
    putc('(', f);
    for (unsigned int c = 0; c < w->ncols; c++) {
        if (c) putc(',', f);
        const char *v = vals[c];
        unsigned long n = lens[c];
        if (!v) fputs("NULL", f);
        else if (w->cols[c].binary) {
            if (n == 0) fputs("''", f);
            else { fputs("0x", f); write_hex(f, v, n); }
        } else if (w->cols[c].numeric && plain_number(v, n)) fwrite(v, 1, n, f);
        else export_sql_string(f, v, n);
        w->batch_bytes += 2 * n + 4;
    }
    putc(')', f);
    if (++w->batch_rows >= SQL_BATCH_ROWS || w->batch_bytes >= SQL_BATCH_BYTES)
        sql_close_insert(w);
}

static void csv_row(export_writer *w, char **vals, const unsigned long *lens) {
    FILE *f = w->f;
    for (unsigned int c = 0; c < w->ncols; c++) {
        if (c) putc(',', f);
        if (!vals[c]) {
            if (w->opts.csv_null_as_n) fputs("\\N", f);
        } else if (w->cols[c].binary && lens[c]) {
            fputs("0x", f);
            write_hex(f, vals[c], lens[c]);
        } else {
            export_csv_field(f, vals[c], lens[c]);
        }
    }
    fputs("\r\n", f);
}

static void xml_row(export_writer *w, char **vals, const unsigned long *lens) {
    FILE *f = w->f;
    fputs("  <row>\n", f);
    for (unsigned int c = 0; c < w->ncols; c++) {
        fputs("    <field name=\"", f);
        xml_attr(f, w->cols[c].name);
        if (!vals[c]) {
            fputs("\" xsi:nil=\"true\"/>\n", f);
        } else if (w->cols[c].binary) {
            fputs("\" encoding=\"hex\">", f);
            write_hex(f, vals[c], lens[c]);
            fputs("</field>\n", f);
        } else {
            fputs("\">", f);
            export_xml_text(f, vals[c], lens[c]);
            fputs("</field>\n", f);
        }
    }
    fputs("  </row>\n", f);
}

int export_row(export_writer *w, char **vals, const unsigned long *lens) {
    switch (w->fmt) {
    case EXPORT_SQL: sql_row(w, vals, lens); break;
    case EXPORT_CSV: csv_row(w, vals, lens); break;
    case EXPORT_XML: xml_row(w, vals, lens); break;
    }
    return ferror(w->f) ? -1 : 0;
}

int export_end(export_writer *w) {
    if (!w) return -1;
    FILE *f = w->f;
    switch (w->fmt) {
    case EXPORT_SQL:
        sql_close_insert(w);
        if (!w->opts.sql_in_dump) {
            putc('\n', f);
            fputs(SQL_SESSION_END, f);
        } else if (w->wrote_insert) {
            putc('\n', f);
        }
        break;
    case EXPORT_CSV:
        break;
    case EXPORT_XML:
        fputs("</table>\n", f);
        break;
    }
    writer_free(w);
    return ferror(f) ? -1 : 0;
}

/* ---------- whole-database dump ---------- */

export_obj_kind export_object_kind(const char *table_type) {
    if (!table_type) return EXPORT_OBJ_SKIP;
    if (strcmp(table_type, "BASE TABLE") == 0 || strcmp(table_type, "SYSTEM VERSIONED") == 0)
        return EXPORT_OBJ_TABLE;
    if (strcmp(table_type, "VIEW") == 0) return EXPORT_OBJ_VIEW;
    return EXPORT_OBJ_SKIP;
}

void export_sql_dump_header(FILE *f, const char *db, size_t ntables, size_t nviews,
                            int selection) {
    fputs(selection ? "-- sqlc export of selected tables from database "
                    : "-- sqlc export of database ", f);
    export_sql_comment_ident(f, db ? db : "");
    fprintf(f, ": %zu table%s, %zu view%s\n\n", ntables, ntables == 1 ? "" : "s",
            nviews, nviews == 1 ? "" : "s");
    fputs(SQL_SESSION_BEGIN, f);
}

void export_sql_dump_footer(FILE *f) {
    fputs(SQL_SESSION_END, f);
}

void export_sql_dump_skipped(FILE *f, const char *name, const char *table_type) {
    fputs("-- Skipped ", f);
    export_sql_comment_ident(f, name ? name : "");
    fputs(" (", f);
    for (const char *c = table_type ? table_type : "unknown type"; *c; c++)
        putc((unsigned char)*c < 0x20 ? '?' : *c, f);
    fputs("): not exported\n\n", f);
}

typedef struct {
    const char *name;
    size_t index;
} named_index;

static int cmp_named(const void *a, const void *b) {
    return strcmp(((const named_index *)a)->name, ((const named_index *)b)->name);
}

int export_select_names(size_t n, const char *const *names, size_t nsel,
                        const char *const *sel, unsigned char *keep, size_t *missing) {
    if (n) memset(keep, 0, n);
    if (nsel == 0) return 0;
    /* names sorted (with their positions), then one binary search per selected name */
    named_index *sorted = malloc((n ? n : 1) * sizeof *sorted);
    if (!sorted) return -1;
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        if (!names[i]) continue;
        sorted[m].name = names[i];
        sorted[m].index = i;
        m++;
    }
    qsort(sorted, m, sizeof *sorted, cmp_named);
    int rc = 0;
    for (size_t k = 0; k < nsel; k++) {
        named_index key = { .name = sel[k] ? sel[k] : "", .index = 0 };
        named_index *hit = m ? bsearch(&key, sorted, m, sizeof *sorted, cmp_named) : NULL;
        if (!hit || !sel[k]) {
            if (rc == 0) {
                rc = 1;
                if (missing) *missing = k;
            }
            continue;
        }
        /* equal names sit next to each other: keep them all */
        size_t at = (size_t)(hit - sorted), lo = at, hi = at;
        while (lo > 0 && strcmp(sorted[lo - 1].name, key.name) == 0) lo--;
        while (hi + 1 < m && strcmp(sorted[hi + 1].name, key.name) == 0) hi++;
        for (size_t i = lo; i <= hi; i++) keep[sorted[i].index] = 1;
    }
    free(sorted);
    return rc;
}

void export_dump_basename(char *buf, size_t size, const char *db, size_t nselected) {
    if (!size) return;
    if (!db || !*db) db = "database";
    if (nselected == 0) snprintf(buf, size, "%s", db);
    else snprintf(buf, size, "%s-%zu-table%s", db, nselected, nselected == 1 ? "" : "s");
}

/* 1 if name (up to its first '.') is a Windows device name. */
static int reserved_device(const char *name) {
    static const char *const fixed[] = { "CON", "PRN", "AUX", "NUL", NULL };
    size_t len = strcspn(name, ".");
    for (int i = 0; fixed[i]; i++)
        if (len == 3 && strncasecmp(name, fixed[i], 3) == 0) return 1;
    if (len == 4 && (strncasecmp(name, "COM", 3) == 0 || strncasecmp(name, "LPT", 3) == 0) &&
        name[3] >= '1' && name[3] <= '9')
        return 1;
    return 0;
}

void export_safe_filename(char *buf, size_t size, const char *name) {
    if (!buf || !size) return;
    if (!name) name = "";
    size_t o = 0;
    if (reserved_device(name) && size > 1) buf[o++] = '_';
    for (const char *p = name; *p; ) {
        /* the whole UTF-8 sequence, or nothing */
        unsigned char c = (unsigned char)*p;
        size_t len = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        for (size_t k = 1; k < len; k++) if (!p[k]) { len = k; break; }
        if (o + len >= size) break;
        if (len == 1 && (c < 0x20 || c == 0x7F || strchr("/\\:*?\"<>|", c))) buf[o++] = '-';
        else { memcpy(buf + o, p, len); o += len; }
        p += len;
    }
    while (o > 0 && (buf[o - 1] == '.' || buf[o - 1] == ' ')) o--;
    buf[o] = '\0';
    if (o == 0) snprintf(buf, size, "export");
}

/* `name` with backticks doubled, as a string (malloc'd), or NULL. */
static char *quoted_ident(const char *name) {
    size_t n = 3;
    for (const char *c = name; *c; c++) n += *c == '`' ? 2 : 1;
    char *q = malloc(n);
    if (!q) return NULL;
    char *p = q;
    *p++ = '`';
    for (const char *c = name; *c; c++) {
        if (*c == '`') *p++ = '`';
        *p++ = *c;
    }
    *p++ = '`';
    *p = '\0';
    return q;
}

int export_view_order(size_t n, const char *const *names, const char *const *defs,
                      size_t *order) {
    for (size_t i = 0; i < n; i++) order[i] = i;
    if (n < 2) return 0;
    if (n > SIZE_MAX / n) return -1;

    /* mentions[i * n + j]: view i's definition mentions view j;
     * waiting[i]: how many unplaced views view i mentions */
    unsigned char *mentions = calloc(n * n, 1);
    size_t *waiting = calloc(n, sizeof *waiting);
    unsigned char *placed = calloc(n, 1);
    char **quoted = calloc(n, sizeof *quoted);
    int rc = -1;
    if (!mentions || !waiting || !placed || !quoted) goto done;
    for (size_t j = 0; j < n; j++)
        if (!(quoted[j] = quoted_ident(names[j] ? names[j] : ""))) goto done;
    for (size_t i = 0; i < n; i++) {
        if (!defs[i]) continue;
        for (size_t j = 0; j < n; j++) {
            if (j == i || !strstr(defs[i], quoted[j])) continue;
            mentions[i * n + j] = 1;
            waiting[i]++;
        }
    }

    for (size_t k = 0; k < n; k++) {
        size_t pick = n;
        for (size_t i = 0; i < n && pick == n; i++)
            if (!placed[i] && waiting[i] == 0) pick = i;
        for (size_t i = 0; i < n && pick == n; i++) /* a cycle: take the first */
            if (!placed[i]) pick = i;
        order[k] = pick;
        placed[pick] = 1;
        for (size_t i = 0; i < n; i++)
            if (mentions[i * n + pick]) { mentions[i * n + pick] = 0; waiting[i]--; }
    }
    rc = 0;

done:
    if (quoted) for (size_t j = 0; j < n; j++) free(quoted[j]);
    free(quoted);
    free(placed);
    free(waiting);
    free(mentions);
    if (rc != 0) for (size_t i = 0; i < n; i++) order[i] = i;
    return rc;
}

/* ---------- progress ---------- */

int export_progress_percent(const export_progress *p) {
    if (!p || p->count == 0) return -1;
    double pct;
    if (p->count == 1) {
        if (p->est == 0) return -1;
        pct = (double)p->rows * 100.0 / (double)p->est;
    } else {
        size_t index = p->index ? p->index : 1;
        if (index > p->count) index = p->count;
        unsigned long long within = p->rows < p->est ? p->rows : p->est;
        double done = (double)p->est_before + (double)(index - 1) + (double)within;
        double total = (double)p->est_total + (double)p->count;
        pct = done * 100.0 / total;
    }
    if (pct < 0) pct = 0;
    if (pct > 99) pct = 99;
    return (int)pct;
}

void export_format_duration(char *buf, size_t size, double seconds) {
    if (!buf || size == 0) return;
    unsigned long long s = seconds > 0 ? (unsigned long long)seconds : 0;
    if (s < 60)
        snprintf(buf, size, "%llus", s);
    else if (s < 3600)
        snprintf(buf, size, "%llum %02llus", s / 60, s % 60);
    else
        snprintf(buf, size, "%lluh %02llum", s / 3600, s % 3600 / 60);
}

void export_format_count(char *buf, size_t size, unsigned long long n) {
    if (!buf || size == 0) return;
    char digits[32], out[48];
    int nd = snprintf(digits, sizeof digits, "%llu", n);
    size_t k = 0;
    for (int i = 0; i < nd; i++) {
        if (i > 0 && (nd - i) % 3 == 0) out[k++] = ',';
        out[k++] = digits[i];
    }
    out[k] = '\0';
    snprintf(buf, size, "%s", out);
}

void export_tilde_path(char *buf, size_t size, const char *path, const char *home) {
    if (!buf || size == 0) return;
    if (!path) { buf[0] = '\0'; return; }
    size_t hl = home ? strlen(home) : 0;
    while (hl > 1 && home[hl - 1] == '/') hl--; /* "/Users/me/" */
    if (hl > 1 && strncmp(path, home, hl) == 0 && (path[hl] == '/' || path[hl] == '\0'))
        snprintf(buf, size, "~%s", path + hl);
    else
        snprintf(buf, size, "%s", path);
}

/* ---------- writing a file safely ---------- */

static char *path_error(const char *what, const char *path, int errnum) {
    size_t n = strlen(what) + strlen(path) + 128;
    char *s = malloc(n);
    if (s) snprintf(s, n, "%s %s: %s", what, path, strerror(errnum));
    return s;
}

FILE *export_temp_open(const char *path, char **tmp, char **err) {
    size_t n = strlen(path) + 16;
    *tmp = malloc(n);
    if (!*tmp) { *err = strdup("Out of memory"); return NULL; }
    snprintf(*tmp, n, "%s.sqlc-XXXXXX", path);
    FILE *f = plat_mkstemp(*tmp, 0);
    if (!f) {
        *err = path_error("Could not create", path, errno);
        free(*tmp);
        *tmp = NULL;
    }
    return f;
}

int export_temp_finish(FILE *f, char *tmp, const char *path, int ok, int write_errno,
                       char **err) {
    int rc = -1;
    if (ok && !write_errno && plat_fsync(f) != 0) write_errno = errno ? errno : EIO;
    if (fclose(f) != 0 && ok && !write_errno) write_errno = errno ? errno : EIO;
    if (write_errno) {
        if (!*err) *err = path_error("Could not write", path, write_errno);
    } else if (ok) {
        if (plat_replace(tmp, path) == 0) rc = 0;
        else if (!*err) *err = path_error("Could not save", path, errno);
    }
    if (rc != 0) plat_unlink(tmp);
    free(tmp);
    return rc;
}
