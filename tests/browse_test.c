/* Unit tests for src/browse.c: page SQL, filter, key columns, page labels (no database). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "browse.h"

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

/* browse_select_sql, compared and freed */
#define SQL_IS(want, table, f, order, norder, offset, limit) do { \
    char err_[256] = ""; \
    char *s_ = browse_select_sql(table, f, order, norder, offset, limit, &esc, err_, sizeof err_); \
    EXPECT_STR(s_, want); \
    if (!s_) fprintf(stderr, "  (%s)\n", err_); \
    free(s_); \
} while (0)

static void test_ops(void) {
    EXPECT_STR(browse_op_label(BROWSE_EQ), "=");
    EXPECT_STR(browse_op_label(BROWSE_NE), "!=");
    EXPECT_STR(browse_op_label(BROWSE_NOT_LIKE), "NOT LIKE");
    EXPECT_STR(browse_op_label(BROWSE_IS_NOT_NULL), "IS NOT NULL");
    CHECK(browse_op_label(BROWSE_NOPS) == NULL);
    CHECK(browse_op_label((browse_op)-1) == NULL);
    CHECK(browse_op_takes_value(BROWSE_EQ));
    CHECK(browse_op_takes_value(BROWSE_LIKE));
    CHECK(!browse_op_takes_value(BROWSE_IS_NULL));
    CHECK(!browse_op_takes_value(BROWSE_IS_NOT_NULL));
    CHECK(!browse_op_takes_value(BROWSE_NOPS));
}

static void test_active(void) {
    browse_filter f = { "status", BROWSE_EQ, "paid" };
    CHECK(browse_filter_active(&f));
    f.value = "";
    CHECK(!browse_filter_active(&f));   /* empty value: no filter */
    f.value = NULL;
    CHECK(!browse_filter_active(&f));
    f.op = BROWSE_IS_NULL;              /* no value needed */
    CHECK(browse_filter_active(&f));
    f.column = NULL;
    CHECK(!browse_filter_active(&f));
    f.column = "";
    CHECK(!browse_filter_active(&f));
    CHECK(!browse_filter_active(NULL));
    browse_filter bad = { "c", BROWSE_NOPS, "x" };
    CHECK(!browse_filter_active(&bad));
}

static void test_sql(void) {
    const char *id[] = { "id" };
    const char *ab[] = { "a", "b" };

    /* no filter, page 1 and page 3, run (1001) and editor (1000) limits */
    SQL_IS("SELECT * FROM `orders` ORDER BY `id` LIMIT 1000;", "orders", NULL, id, 1, 0, 1000);
    SQL_IS("SELECT * FROM `orders` ORDER BY `id` LIMIT 1001;", "orders", NULL, id, 1, 0, 1001);
    SQL_IS("SELECT * FROM `orders` ORDER BY `id` LIMIT 1000 OFFSET 2000;", "orders", NULL, id, 1,
           2000, 1000);

    /* the ticket's examples */
    browse_filter paid = { "status", BROWSE_EQ, "paid" };
    SQL_IS("SELECT * FROM `orders` WHERE `status` = 'paid' ORDER BY `id` LIMIT 1000 OFFSET 2000;",
           "orders", &paid, id, 1, 2000, 1000);
    browse_filter null = { "note", BROWSE_IS_NULL, "ignored" };
    SQL_IS("SELECT * FROM `orders` WHERE `note` IS NULL ORDER BY `a`, `b` LIMIT 1000;",
           "orders", &null, ab, 2, 0, 1000);
    SQL_IS("SELECT * FROM `log` LIMIT 1000 OFFSET 1000;", "log", NULL, NULL, 0, 1000, 1000);

    /* every operator */
    static const char *const want[BROWSE_NOPS] = {
        "SELECT * FROM `t` WHERE `c` = 'v' LIMIT 1000;",
        "SELECT * FROM `t` WHERE `c` <> 'v' LIMIT 1000;",
        "SELECT * FROM `t` WHERE `c` < 'v' LIMIT 1000;",
        "SELECT * FROM `t` WHERE `c` > 'v' LIMIT 1000;",
        "SELECT * FROM `t` WHERE `c` <= 'v' LIMIT 1000;",
        "SELECT * FROM `t` WHERE `c` >= 'v' LIMIT 1000;",
        "SELECT * FROM `t` WHERE `c` LIKE '%v%' LIMIT 1000;",
        "SELECT * FROM `t` WHERE `c` NOT LIKE '%v%' LIMIT 1000;",
        "SELECT * FROM `t` WHERE `c` IS NULL LIMIT 1000;",
        "SELECT * FROM `t` WHERE `c` IS NOT NULL LIMIT 1000;",
    };
    for (int op = 0; op < BROWSE_NOPS; op++) {
        browse_filter f = { "c", (browse_op)op, "v" };
        SQL_IS(want[op], "t", &f, NULL, 0, 0, 1000);
    }

    /* NULL operators ignore the value, even an empty or missing one */
    browse_filter nn = { "c", BROWSE_IS_NOT_NULL, NULL };
    SQL_IS("SELECT * FROM `t` WHERE `c` IS NOT NULL LIMIT 1000;", "t", &nn, NULL, 0, 0, 1000);
    nn.value = "x' OR 1 --";
    SQL_IS("SELECT * FROM `t` WHERE `c` IS NOT NULL LIMIT 1000;", "t", &nn, NULL, 0, 0, 1000);

    /* empty value: no WHERE */
    browse_filter empty = { "c", BROWSE_LIKE, "" };
    SQL_IS("SELECT * FROM `t` ORDER BY `id` LIMIT 1000;", "t", &empty, id, 1, 0, 1000);
    browse_filter none = { NULL, BROWSE_EQ, "x" };
    SQL_IS("SELECT * FROM `t` LIMIT 1000;", "t", &none, NULL, 0, 0, 1000);

    /* quotes and backslashes go through the escaper; LIKE wildcards stay */
    browse_filter q = { "name", BROWSE_EQ, "O'Brien \\ x" };
    SQL_IS("SELECT * FROM `t` WHERE `name` = 'O\\'Brien \\\\ x' LIMIT 1000;", "t", &q, NULL, 0, 0, 1000);
    browse_filter like = { "name", BROWSE_LIKE, "a%_" };
    SQL_IS("SELECT * FROM `t` WHERE `name` LIKE 'a%_' LIMIT 1000;", "t", &like, NULL, 0, 0, 1000);
    /* LIKE without a wildcard becomes "contains"; with one it is used as typed */
    browse_filter contains = { "name", BROWSE_LIKE, "john" };
    SQL_IS("SELECT * FROM `t` WHERE `name` LIKE '%john%' LIMIT 1000;", "t", &contains, NULL, 0, 0, 1000);
    contains.value = "john%";
    SQL_IS("SELECT * FROM `t` WHERE `name` LIKE 'john%' LIMIT 1000;", "t", &contains, NULL, 0, 0, 1000);
    contains.value = "user_id";
    SQL_IS("SELECT * FROM `t` WHERE `name` LIKE 'user_id' LIMIT 1000;", "t", &contains, NULL, 0, 0, 1000);
    contains.op = BROWSE_NOT_LIKE;
    contains.value = "O'B";
    SQL_IS("SELECT * FROM `t` WHERE `name` NOT LIKE '%O\\'B%' LIMIT 1000;", "t", &contains, NULL, 0, 0, 1000);
    contains.op = BROWSE_EQ;
    contains.value = "john";
    SQL_IS("SELECT * FROM `t` WHERE `name` = 'john' LIMIT 1000;", "t", &contains, NULL, 0, 0, 1000);
    browse_filter utf = { "name", BROWSE_EQ, "Ž\xC3\xA9" };
    SQL_IS("SELECT * FROM `t` WHERE `name` = 'Ž\xC3\xA9' LIMIT 1000;", "t", &utf, NULL, 0, 0, 1000);

    /* backticks in table, column and key names are doubled */
    const char *odd[] = { "k`1", "k2" };
    browse_filter bt = { "c`x", BROWSE_GT, "5" };
    SQL_IS("SELECT * FROM `a``b` WHERE `c``x` > '5' ORDER BY `k``1`, `k2` LIMIT 1001 OFFSET 1000;",
           "a`b", &bt, odd, 2, 1000, 1001);

    /* large offset */
    SQL_IS("SELECT * FROM `t` LIMIT 1000 OFFSET 18446744073709551000;", "t", NULL, NULL, 0,
           18446744073709551000ULL, 1000);

    /* errors */
    char err[256] = "";
    CHECK(browse_select_sql(NULL, NULL, NULL, 0, 0, 1000, &esc, err, sizeof err) == NULL && err[0]);
    err[0] = 0;
    CHECK(browse_select_sql("", NULL, NULL, 0, 0, 1000, &esc, err, sizeof err) == NULL && err[0]);
    err[0] = 0;
    browse_filter bad = { "c", BROWSE_NOPS, "v" };
    CHECK(browse_select_sql("t", &bad, NULL, 0, 0, 1000, &esc, err, sizeof err) == NULL && err[0]);
    err[0] = 0;
    CHECK(browse_select_sql("t", &paid, NULL, 0, 0, 1000, NULL, err, sizeof err) == NULL && err[0]);
    /* no escaper needed without a value */
    char *s = browse_select_sql("t", &null, NULL, 0, 0, 1000, NULL, err, sizeof err);
    EXPECT_STR(s, "SELECT * FROM `t` WHERE `note` IS NULL LIMIT 1000;");
    free(s);
}

static void test_keys(void) {
    const char **keys;
    size_t n;

    /* one key column, not the first */
    const char *one[] = { "name", "varchar", "YES", "", "",
                          "id", "int", "NO", "PRI", "auto_increment",
                          "qty", "int", "YES", "MUL", "" };
    db_set s1 = make_set(COLNAMES, 3, one);
    CHECK(browse_key_columns(&s1, &keys, &n) == 0 && n == 1 && keys);
    if (n == 1) EXPECT_STR(keys[0], "id");
    free(keys);
    CHECK(browse_default_column(&s1) == 1);
    free_set(&s1);

    /* composite key, in ORDINAL_POSITION order */
    const char *two[] = { "a", "int", "NO", "PRI", "",
                          "x", "text", "YES", "", "",
                          "b", "int", "NO", "PRI", "" };
    db_set s2 = make_set(COLNAMES, 3, two);
    CHECK(browse_key_columns(&s2, &keys, &n) == 0 && n == 2 && keys);
    if (n == 2) { EXPECT_STR(keys[0], "a"); EXPECT_STR(keys[1], "b"); }
    free(keys);
    CHECK(browse_default_column(&s2) == 0);
    free_set(&s2);

    /* no key: no ORDER BY, the filter starts on the first column */
    const char *nk[] = { "a", "int", "YES", "", "",
                         "b", "varchar", "YES", "UNI", "",
                         "c", "int", "YES", "\x01", "" };
    db_set s3 = make_set(COLNAMES, 3, nk);
    keys = (const char **)1;
    CHECK(browse_key_columns(&s3, &keys, &n) == 0 && n == 0 && keys == NULL);
    CHECK(browse_default_column(&s3) == 0);
    free_set(&s3);

    /* empty set (table gone), NULL, too few columns */
    db_set s4 = make_set(COLNAMES, 0, NULL);
    CHECK(browse_key_columns(&s4, &keys, &n) == 0 && n == 0 && keys == NULL);
    CHECK(browse_default_column(&s4) == 0);
    free_set(&s4);
    CHECK(browse_key_columns(NULL, &keys, &n) == -1 && n == 0 && keys == NULL);
    CHECK(browse_default_column(NULL) == 0);
    const char *const narrow[] = { "COLUMN_NAME", "DATA_TYPE", NULL };
    const char *ncells[] = { "id", "int" };
    db_set s5 = make_set(narrow, 1, ncells);
    CHECK(browse_key_columns(&s5, &keys, &n) == -1 && keys == NULL);
    CHECK(browse_default_column(&s5) == 0);
    free_set(&s5);
}

static void test_labels(void) {
    char buf[64];
    browse_page_label(buf, sizeof buf, 0, 0);
    EXPECT_STR(buf, "No rows");
    browse_page_label(buf, sizeof buf, 3000, 0);
    EXPECT_STR(buf, "No rows");
    browse_page_label(buf, sizeof buf, 0, 1);
    EXPECT_STR(buf, "Rows 1\xE2\x80\x93" "1");
    browse_page_label(buf, sizeof buf, 0, 1000);
    EXPECT_STR(buf, "Rows 1\xE2\x80\x93" "1,000");
    browse_page_label(buf, sizeof buf, 1000, 234);
    EXPECT_STR(buf, "Rows 1,001\xE2\x80\x93" "1,234");
    browse_page_label(buf, sizeof buf, 1000, 1000);
    EXPECT_STR(buf, "Rows 1,001\xE2\x80\x93" "2,000");
    browse_page_label(buf, sizeof buf, 1234567000ULL, 1000);
    EXPECT_STR(buf, "Rows 1,234,567,001\xE2\x80\x93" "1,234,568,000");
    browse_page_label(buf, sizeof buf, 18446744073709551615ULL, 1000); /* saturates */
    EXPECT_STR(buf, "Rows 18,446,744,073,709,551,615\xE2\x80\x93" "18,446,744,073,709,551,615");
    /* too small a buffer: cut short, still terminated */
    char tiny[6];
    browse_page_label(tiny, sizeof tiny, 0, 1000);
    CHECK(strlen(tiny) == 5 && strncmp(tiny, "Rows ", 5) == 0);
    browse_page_label(NULL, 0, 0, 1); /* no crash */
}

int main(void) {
    test_ops();
    test_active();
    test_sql();
    test_keys();
    test_labels();
    if (failures) { fprintf(stderr, "browse_test: %d failure(s)\n", failures); return 1; }
    printf("browse_test: all passed\n");
    return 0;
}
