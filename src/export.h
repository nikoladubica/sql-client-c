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

#include <stddef.h>
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
    /* Set by export_database: this table is one part of a database dump, so
     * the SQL writer leaves out its own SET header/footer (the dump writes
     * them once, see export_sql_dump_header) and starts with a short
     * "-- Table `t`" / "-- View `v`" comment instead. */
    int sql_in_dump;
} export_opts;

/* Defaults: SQL structure + DROP + data; CSV NULL as empty, with BOM;
 * a standalone file (sql_in_dump off). */
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

/* ---- whole-database SQL dump (driven by export_database in export_db.h) ---- */

typedef enum { EXPORT_OBJ_TABLE, EXPORT_OBJ_VIEW, EXPORT_OBJ_SKIP } export_obj_kind;

/* What a dump does with an information_schema.TABLES TABLE_TYPE value:
 * "BASE TABLE" and MariaDB's "SYSTEM VERSIONED" are tables (structure + the
 * current rows), "VIEW" is a view (structure only), anything else
 * ("SEQUENCE", "SYSTEM VIEW", "TEMPORARY", NULL, ...) is skipped. */
export_obj_kind export_object_kind(const char *table_type);

/* The dump's opening comment ("-- sqlc export of database `db`: 2 tables,
 * 1 view", or with selection "-- sqlc export of selected tables from
 * database `db`: ..." for a dump of some of its objects) and the SET lines that make it load in any table order:
 * utf8mb4, UTC time zone, NO_AUTO_VALUE_ON_ZERO and FOREIGN_KEY_CHECKS=0,
 * each saved in a user variable first. */
void export_sql_dump_header(FILE *f, const char *db, size_t ntables, size_t nviews,
                            int selection);
/* Restores what export_sql_dump_header changed. */
void export_sql_dump_footer(FILE *f);
/* "-- Skipped `name` (TYPE): not exported" for an EXPORT_OBJ_SKIP object. */
void export_sql_dump_skipped(FILE *f, const char *name, const char *table_type);

/*
 * For a dump of selected objects: sets keep[i] (n entries) to 1 when names[i]
 * is one of the nsel names in sel (exact, case-sensitive match, as the
 * sidebar shows them), else 0. Duplicates in sel are fine; NULL entries in
 * names never match, a NULL entry in sel is not found.
 *
 * Returns 0 if every selected name was found, 1 if one was not (*missing,
 * if not NULL, is then the index in sel of the first such name; keep is
 * still filled for the others), or -1 if out of memory.
 */
int export_select_names(size_t n, const char *const *names, size_t nsel,
                        const char *const *sel, unsigned char *keep, size_t *missing);

/* Default file name, without extension, for a dump: "db" for the whole
 * database, "db-3-tables" for nselected objects of it ("db-1-table").
 * A NULL or empty db is "database". Truncated to fit size. */
void export_dump_basename(char *buf, size_t size, const char *db, size_t nselected);

/*
 * The order to create n views in so that a view comes after the other views
 * its definition mentions (as `name`, the way SHOW CREATE VIEW quotes
 * identifiers). names[i] and defs[i] belong to view i; a NULL def mentions
 * nothing. Views with nothing left to wait for go in their original order;
 * a cycle (or a false match, such as a column named like a view) is broken
 * by taking the first remaining view.
 *
 * Fills order[0..n) with a permutation of 0..n-1. Returns 0, or -1 if out
 * of memory (order is then 0..n-1).
 */
int export_view_order(size_t n, const char *const *names, const char *const *defs,
                      size_t *order);

/* ---- escaping, exposed for tests ---- */

/* 'text' with \0 \n \r \\ ' " and Ctrl-Z escaped (as mysql_real_escape_string). */
void export_sql_string(FILE *f, const char *s, unsigned long len);
/* `name` with backticks doubled. */
void export_sql_ident(FILE *f, const char *name);
/* `name` for a -- comment line: as export_sql_ident, but control characters
 * (line breaks above all, which would end the comment) become '?'. */
void export_sql_comment_ident(FILE *f, const char *name);
/* RFC 4180 field: quoted (with "" for ") when it contains , " CR or LF. */
void export_csv_field(FILE *f, const char *s, unsigned long len);
/* XML text/attribute content: & < > " ' escaped; characters XML 1.0 can't
 * hold (most control characters, invalid UTF-8) become U+FFFD. */
void export_xml_text(FILE *f, const char *s, unsigned long len);

#endif
