#ifndef SQLC_EXPORT_H
#define SQLC_EXPORT_H

/*
 * Streaming table/result-set writers for SQL, CSV and XML.
 *
 * No database code here: feed column info and rows (as returned by
 * mysql_fetch_row/mysql_fetch_lengths) and the writer formats them to a
 * FILE as they arrive, so nothing is held in memory. See export_db.h for
 * exporting a table straight from a connection.
 */

#include <stdio.h>

typedef enum { EXPORT_SQL, EXPORT_CSV, EXPORT_XML } export_format;

typedef struct {
    /* SQL */
    int sql_structure;   /* CREATE TABLE (from SHOW CREATE TABLE) */
    int sql_drop;        /* ... preceded by DROP TABLE IF EXISTS */
    int sql_data;        /* INSERT statements */
    /* CSV */
    int csv_null_as_n;   /* NULL as \N instead of an empty field */
    int csv_bom;         /* start with a UTF-8 byte order mark (for Excel) */
} export_opts;

/* Defaults: SQL structure + DROP + data; CSV NULL as empty, with BOM. */
export_opts export_default_opts(void);

typedef struct {
    const char *name;
    int binary;          /* binary data (BLOB, VARBINARY, BIT...): written as hex */
    int numeric;         /* number: written unquoted in SQL */
} export_col;

/* File extension without the dot: "sql", "csv", "xml". */
const char *export_extension(export_format fmt);

typedef struct export_writer export_writer;

/*
 * Start writing. create_sql is the CREATE TABLE/VIEW statement (SQL format
 * with sql_structure; may be NULL otherwise). For a view (is_view) the SQL
 * format writes only the structure, with DROP VIEW. cols is copied.
 * Returns NULL on allocation failure.
 */
export_writer *export_begin(FILE *f, export_format fmt, const export_opts *opts,
                            const char *table, const char *create_sql, int is_view,
                            unsigned int ncols, const export_col *cols);

/* One row: ncols values (NULL = SQL NULL) and their byte lengths. */
int export_row(export_writer *w, char **vals, const unsigned long *lens);

/* Finish and free the writer (always). Returns 0, or -1 if any write to the
 * FILE failed (check ferror / errno). Doesn't close the FILE. */
int export_end(export_writer *w);

/* ---- escaping, exposed for tests ---- */

/* 'text' with \0 \n \r \\ ' " and Ctrl-Z escaped (as mysql_real_escape_string). */
void export_sql_string(FILE *f, const char *s, unsigned long len);
/* `name` with backticks doubled. */
void export_sql_ident(FILE *f, const char *name);
/* RFC 4180 field: quoted (with "" for ") when it contains , " CR or LF. */
void export_csv_field(FILE *f, const char *s, unsigned long len);
/* XML text/attribute content: & < > " ' escaped; characters XML 1.0 can't
 * hold (most control characters, invalid UTF-8) become U+FFFD. */
void export_xml_text(FILE *f, const char *s, unsigned long len);

#endif
