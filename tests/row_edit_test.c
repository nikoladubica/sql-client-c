/* Unit tests for src/row_edit.c: column kinds, typed values, row SQL (no database). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "row_edit.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", \
                                          __FILE__, __LINE__, #c); failures++; } } while (0)
#define EXPECT_STR(got, want) do { \
    const char *g_ = (got), *w_ = (want); \
    if (!g_ || !w_ ? g_ != w_ : strcmp(g_, w_) != 0) { \
        fprintf(stderr, "%s:%d: expected [%s]\n  got [%s]\n", __FILE__, __LINE__, \
                w_ ? w_ : "(null)", g_ ? g_ : "(null)"); \
        failures++; \
    } \
} while (0)

/* Backslash-style escaping, like the server's default sql_mode. */
static unsigned long escape(char *to, const char *from, unsigned long len, void *data) {
    (void)data;
    char *o = to;
    for (unsigned long i = 0; i < len; i++) {
        if (from[i] == '\'' || from[i] == '\\') *o++ = '\\';
        *o++ = from[i];
    }
    *o = '\0';
    return (unsigned long)(o - to);
}
static const row_edit_escaper esc = { escape, NULL };

/* A db_set from a NULL-terminated name list and nrows * ncols cells ("\x01" = NULL). */
static db_set make_set(const char *const *names, size_t nrows, const char *const *cells) {
    db_set s = { .has_rows = 1 };
    while (names[s.ncols]) s.ncols++;
    s.names = calloc(s.ncols, sizeof *s.names);
    for (unsigned int c = 0; c < s.ncols; c++) s.names[c] = strdup(names[c]);
    s.nrows = nrows;
    s.cells = calloc(nrows * s.ncols + 1, sizeof *s.cells);
    for (size_t i = 0; i < nrows * s.ncols; i++)
        s.cells[i] = strcmp(cells[i], "\x01") == 0 ? NULL : strdup(cells[i]);
    return s;
}

static void free_set(db_set *s) {
    for (unsigned int c = 0; c < s->ncols; c++) free(s->names[c]);
    for (size_t i = 0; i < s->nrows * s->ncols; i++) free(s->cells[i]);
    free(s->names);
    free(s->cells);
}

static const char *const COLNAMES[] = { "COLUMN_NAME", "DATA_TYPE", "IS_NULLABLE", "COLUMN_KEY", "EXTRA", NULL };
#define N "\x01"

static char *norm(col_kind kind, int nullable, const char *text, int *is_null, char *err) {
    col_info c = { .name = "c", .kind = kind, .nullable = nullable };
    return row_edit_normalize(&c, text, is_null, err, 256);
}

static void test_kinds(void) {
    int approx;
    CHECK(row_edit_kind("int", &approx) == COL_NUMBER && !approx);
    CHECK(row_edit_kind("BIGINT", &approx) == COL_NUMBER);
    CHECK(row_edit_kind("decimal", &approx) == COL_NUMBER && !approx);
    CHECK(row_edit_kind("double", &approx) == COL_NUMBER && approx);
    CHECK(row_edit_kind("varchar", &approx) == COL_TEXT);
    CHECK(row_edit_kind("enum", &approx) == COL_TEXT);
    CHECK(row_edit_kind("date", &approx) == COL_DATE);
    CHECK(row_edit_kind("timestamp", &approx) == COL_DATETIME);
    CHECK(row_edit_kind("time", &approx) == COL_TIME);
    CHECK(row_edit_kind("blob", &approx) == COL_BINARY);
    CHECK(row_edit_kind("bit", &approx) == COL_BINARY);
}

static void test_normalize(void) {
    int is_null;
    char err[256];
    char *v;

#define OK(kind, in, want) do { err[0] = 0; v = norm(kind, 1, in, &is_null, err); \
    EXPECT_STR(v, want); if (!v) fprintf(stderr, "  (%s)\n", err); free(v); } while (0)
#define BAD(kind, in) do { err[0] = 0; v = norm(kind, 1, in, &is_null, err); \
    CHECK(v == NULL && !is_null && err[0]); free(v); } while (0)

    OK(COL_NUMBER, " 42 ", "42");
    OK(COL_NUMBER, "-3.25", "-3.25");
    OK(COL_NUMBER, "1.5e3", "1.5e3");
    OK(COL_NUMBER, ".5", ".5");
    BAD(COL_NUMBER, "abc");
    BAD(COL_NUMBER, "1,5");
    BAD(COL_NUMBER, "1; DROP TABLE x");
    BAD(COL_NUMBER, "e5");

    OK(COL_DATE, "2024-01-05", "2024-01-05");
    OK(COL_DATE, "2024-1-5", "2024-01-05");
    OK(COL_DATE, "2024/12/31", "2024-12-31");
    OK(COL_DATE, "0000-00-00", "0000-00-00");
    BAD(COL_DATE, "2024-13-01");
    BAD(COL_DATE, "05.01.2024");
    BAD(COL_DATE, "2024-01-05 10:00");
    BAD(COL_DATE, "tomorrow");

    OK(COL_DATETIME, "2024-01-05 9:30", "2024-01-05 09:30:00");
    OK(COL_DATETIME, "2024-01-05T09:30:15", "2024-01-05 09:30:15");
    OK(COL_DATETIME, "2024-01-05", "2024-01-05 00:00:00");
    OK(COL_DATETIME, "2024-01-05 23:59:59.123", "2024-01-05 23:59:59.123");
    BAD(COL_DATETIME, "2024-01-05 24:00");
    BAD(COL_DATETIME, "2024-01-05 10:60");
    BAD(COL_DATETIME, "2024-01-05 10:00:00.1234567");

    OK(COL_TIME, "9:30", "09:30:00");
    OK(COL_TIME, "-838:59:59", "-838:59:59");
    OK(COL_TIME, "100:00:00", "100:00:00");
    BAD(COL_TIME, "9");
    BAD(COL_TIME, "839:00:00");

    OK(COL_TEXT, "  keep spaces ", "  keep spaces ");
    OK(COL_TEXT, "", "");

    /* empty: NULL if allowed */
    v = norm(COL_NUMBER, 1, "  ", &is_null, err);
    CHECK(v == NULL && is_null);
    err[0] = 0;
    v = norm(COL_DATE, 0, "", &is_null, err);
    CHECK(v == NULL && !is_null && strstr(err, "NULL"));

    col_info blob = { .name = "b", .kind = COL_BINARY };
    v = row_edit_normalize(&blob, "x", &is_null, err, sizeof err);
    CHECK(v == NULL && !is_null);
#undef OK
#undef BAD
}

static void test_info(void) {
    const char *names[] = { "id", "name", "price", "body", "total", NULL };
    db_set rows = make_set(names, 0, NULL);
    const char *cols[] = {
        "total", "int", "YES", "", "STORED GENERATED",
        "id", "int", "NO", "PRI", "auto_increment",
        "name", "varchar", "YES", "", "",
        "price", "double", "NO", "", "DEFAULT_GENERATED",
        "body", "blob", "YES", "", "",
    };
    db_set info = make_set(COLNAMES, 5, cols);
    table_info ti;
    CHECK(table_info_load(&ti, &info, &rows) == 0);
    CHECK(ti.ncols == 5 && ti.npk == 1);
    CHECK(ti.cols[0].primary && ti.cols[0].auto_increment && !ti.cols[0].nullable);
    CHECK(ti.cols[1].kind == COL_TEXT && ti.cols[1].nullable);
    CHECK(ti.cols[2].approx && !ti.cols[2].generated); /* DEFAULT_GENERATED is writable */
    CHECK(ti.cols[4].generated);
    CHECK(row_edit_editable(&ti, 0) && row_edit_editable(&ti, 1) && row_edit_editable(&ti, 2));
    CHECK(!row_edit_editable(&ti, 3) && !row_edit_editable(&ti, 4) && !row_edit_editable(&ti, 9));
    table_info_free(&ti);

    /* a column that isn't described: no editing */
    const char *more[] = { "id", "missing", NULL };
    db_set rows2 = make_set(more, 0, NULL);
    CHECK(table_info_load(&ti, &info, &rows2) == -1 && ti.ncols == 0);

    /* key column not in the rows (invisible): key dropped */
    const char *nokey[] = { "name", NULL };
    db_set rows3 = make_set(nokey, 0, NULL);
    CHECK(table_info_load(&ti, &info, &rows3) == 0 && ti.npk == 0 && !ti.cols[0].primary);
    table_info_free(&ti);

    free_set(&rows); free_set(&rows2); free_set(&rows3); free_set(&info);
}

static void load(table_info *ti, const db_set *rows, const char *const *cols, size_t n) {
    db_set info = make_set(COLNAMES, n, cols);
    CHECK(table_info_load(ti, &info, rows) == 0);
    free_set(&info);
}

static void test_sql_pk(void) {
    const char *names[] = { "id", "name", "qty", "born", NULL };
    const char *cells[] = { "1", "O'Brien", "3", N,
                            "2", "a\\b", N, "2024-01-05" };
    db_set rows = make_set(names, 2, cells);
    const char *cols[] = {
        "id", "int", "NO", "PRI", "auto_increment",
        "name", "varchar", "YES", "", "",
        "qty", "int", "YES", "", "",
        "born", "date", "YES", "", "",
    };
    table_info ti;
    load(&ti, &rows, cols, 4);
    char err[256] = "";
    char *sql;

    sql = row_edit_update_sql("t`x", &ti, &rows, 0, 1, "Pat's", &esc, err, sizeof err);
    EXPECT_STR(sql, "UPDATE `t``x` SET `name` = 'Pat\\'s' WHERE `id` = '1' LIMIT 1;\n"
                    "SELECT * FROM `t``x` WHERE `id` = '1' LIMIT 1");
    free(sql);

    sql = row_edit_update_sql("t", &ti, &rows, 1, 2, "42", &esc, err, sizeof err);
    EXPECT_STR(sql, "UPDATE `t` SET `qty` = 42 WHERE `id` = '2' LIMIT 1;\n"
                    "SELECT * FROM `t` WHERE `id` = '2' LIMIT 1");
    free(sql);

    sql = row_edit_update_sql("t", &ti, &rows, 1, 3, NULL, &esc, err, sizeof err);
    EXPECT_STR(sql, "UPDATE `t` SET `born` = NULL WHERE `id` = '2' LIMIT 1;\n"
                    "SELECT * FROM `t` WHERE `id` = '2' LIMIT 1");
    free(sql);

    /* changing the key: read the row back by the new key */
    sql = row_edit_update_sql("t", &ti, &rows, 0, 0, "10", &esc, err, sizeof err);
    EXPECT_STR(sql, "UPDATE `t` SET `id` = 10 WHERE `id` = '1' LIMIT 1;\n"
                    "SELECT * FROM `t` WHERE `id` = '10' LIMIT 1");
    free(sql);

    /* insert: only the set columns; auto-increment key read back */
    unsigned char set1[] = { 0, 1, 1, 0 };
    sql = row_edit_insert_sql("t", &ti, &rows, 0, set1, &esc, err, sizeof err);
    EXPECT_STR(sql, "INSERT INTO `t` (`name`, `qty`) VALUES ('O\\'Brien', 3);\n"
                    "SELECT * FROM `t` WHERE `id` = LAST_INSERT_ID() LIMIT 1");
    free(sql);
    unsigned char set2[] = { 1, 0, 0, 0 };
    sql = row_edit_insert_sql("t", &ti, &rows, 1, set2, &esc, err, sizeof err);
    EXPECT_STR(sql, "INSERT INTO `t` (`id`) VALUES (2);\n"
                    "SELECT * FROM `t` WHERE `id` = '2' LIMIT 1");
    free(sql);
    unsigned char none[] = { 0, 0, 0, 0 };
    sql = row_edit_insert_sql("t", &ti, &rows, 1, none, &esc, err, sizeof err);
    EXPECT_STR(sql, "INSERT INTO `t` () VALUES ();\n"
                    "SELECT * FROM `t` WHERE `id` = LAST_INSERT_ID() LIMIT 1");
    free(sql);

    size_t which[] = { 0, 1 };
    sql = row_edit_delete_sql("t", &ti, &rows, which, 2, &esc, err, sizeof err);
    EXPECT_STR(sql, "START TRANSACTION;\n"
                    "DELETE FROM `t` WHERE `id` = '1' LIMIT 1;\n"
                    "DELETE FROM `t` WHERE `id` = '2' LIMIT 1;\n"
                    "COMMIT");
    free(sql);

    table_info_free(&ti);
    free_set(&rows);
}

static void test_sql_no_pk(void) {
    const char *names[] = { "a", "b", "f", "bin", NULL };
    const char *cells[] = { "1", N, "1.1", "x" };
    db_set rows = make_set(names, 1, cells);
    const char *cols[] = {
        "a", "int", "YES", "", "",
        "b", "varchar", "YES", "MUL", "",
        "f", "float", "YES", "", "",
        "bin", "varbinary", "YES", "", "",
    };
    table_info ti;
    load(&ti, &rows, cols, 4);
    char err[256] = "";
    char *sql = row_edit_update_sql("t", &ti, &rows, 0, 1, "x", &esc, err, sizeof err);
    /* float and binary columns can't be matched by their text */
    EXPECT_STR(sql, "UPDATE `t` SET `b` = 'x' WHERE `a` <=> '1' AND `b` <=> NULL LIMIT 1;\n"
                    "SELECT * FROM `t` WHERE `a` <=> '1' AND `b` <=> 'x' LIMIT 1");
    free(sql);

    unsigned char set[] = { 1, 0, 0, 0 };
    sql = row_edit_insert_sql("t", &ti, &rows, 0, set, &esc, err, sizeof err);
    EXPECT_STR(sql, "INSERT INTO `t` (`a`) VALUES (1)"); /* no way to read it back */
    free(sql);

    /* the binary column can't be edited */
    sql = row_edit_update_sql("t", &ti, &rows, 0, 3, "y", &esc, err, sizeof err);
    CHECK(sql == NULL && err[0]);
    table_info_free(&ti);
    free_set(&rows);

    /* nothing to match on at all */
    const char *only[] = { "f", NULL };
    const char *fcell[] = { "2.5" };
    db_set frow = make_set(only, 1, fcell);
    const char *fcols[] = { "f", "double", "YES", "", "" };
    load(&ti, &frow, fcols, 1);
    err[0] = 0;
    sql = row_edit_update_sql("t", &ti, &frow, 0, 0, "3", &esc, err, sizeof err);
    CHECK(sql == NULL && strstr(err, "primary key"));
    size_t r0 = 0;
    sql = row_edit_delete_sql("t", &ti, &frow, &r0, 1, &esc, err, sizeof err);
    CHECK(sql == NULL);
    table_info_free(&ti);
    free_set(&frow);
}

int main(void) {
    test_kinds();
    test_normalize();
    test_info();
    test_sql_pk();
    test_sql_no_pk();
    if (failures) { fprintf(stderr, "row_edit_test: %d failure(s)\n", failures); return 1; }
    printf("row_edit_test: all passed\n");
    return 0;
}
