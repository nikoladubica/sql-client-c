#include "export.h"

#include <stdlib.h>
#include <string.h>

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
    size_t batch_rows, batch_bytes;
};

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
    fputs("-- sqlc export of ", f);
    fputs(w->is_view ? "view " : "table ", f);
    export_sql_ident(f, w->table);
    fputs("\n\n"
          "SET NAMES utf8mb4;\n"
          "SET @OLD_TIME_ZONE=@@TIME_ZONE, TIME_ZONE='+00:00';\n"
          "SET @OLD_SQL_MODE=@@SQL_MODE, SQL_MODE='NO_AUTO_VALUE_ON_ZERO';\n"
          "SET @OLD_FOREIGN_KEY_CHECKS=@@FOREIGN_KEY_CHECKS, FOREIGN_KEY_CHECKS=0;\n\n", f);
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
        fputs("\nSET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS;\n"
              "SET SQL_MODE=@OLD_SQL_MODE;\n"
              "SET TIME_ZONE=@OLD_TIME_ZONE;\n", f);
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
