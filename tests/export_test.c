/* Unit tests for the export writers (no database needed). */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "export.h"

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

/* Output capture */
static char *buf;
static size_t buflen;
static FILE *out;

static void start(void) {
    free(buf);
    buf = NULL;
    buflen = 0;
    out = open_memstream(&buf, &buflen);
}

static const char *finish(void) {
    fclose(out);
    out = NULL;
    return buf;
}

#define EXPECT(expected) do { \
    const char *got_ = finish(); \
    if (strcmp(got_, expected) != 0) { \
        fprintf(stderr, "%s:%d: expected [%s]\n  got [%s]\n", __FILE__, __LINE__, expected, got_); \
        failures++; \
    } \
} while (0)

static size_t count(const char *hay, const char *needle) {
    size_t n = 0;
    for (const char *p = hay; (p = strstr(p, needle)); p += strlen(needle)) n++;
    return n;
}

static void test_sql_escaping(void) {
    start(); export_sql_string(out, "it's \"q\" \\ x", 12); EXPECT("'it\\'s \\\"q\\\" \\\\ x'");
    start(); export_sql_string(out, "a\0b\nc\rd\032e", 9); EXPECT("'a\\0b\\nc\\rd\\Ze'");
    start(); export_sql_string(out, "", 0); EXPECT("''");
    start(); export_sql_string(out, "\xC3\xA9t\xC3\xA9", 5); EXPECT("'\xC3\xA9t\xC3\xA9'"); /* UTF-8 as is */
    start(); export_sql_ident(out, "plain"); EXPECT("`plain`");
    start(); export_sql_ident(out, "we`ird name"); EXPECT("`we``ird name`");
}

static void test_csv_escaping(void) {
    start(); export_csv_field(out, "plain", 5); EXPECT("plain");
    start(); export_csv_field(out, "a,b", 3); EXPECT("\"a,b\"");
    start(); export_csv_field(out, "say \"hi\"", 8); EXPECT("\"say \"\"hi\"\"\"");
    start(); export_csv_field(out, "two\nlines", 9); EXPECT("\"two\nlines\"");
    start(); export_csv_field(out, "cr\rhere", 7); EXPECT("\"cr\rhere\"");
    start(); export_csv_field(out, "", 0); EXPECT("\"\""); /* empty string, not NULL */
    start(); export_csv_field(out, " lead", 5); EXPECT(" lead");
}

static void test_xml_escaping(void) {
    start(); export_xml_text(out, "a&b<c>d\"e'f", 11); EXPECT("a&amp;b&lt;c&gt;d&quot;e&apos;f");
    start(); export_xml_text(out, "x\r\ny\tz", 6); EXPECT("x&#13;&#10;y&#9;z");
    start(); export_xml_text(out, "bell\x01!", 6); EXPECT("bell\xEF\xBF\xBD!");      /* not allowed in XML */
    start(); export_xml_text(out, "\xC3(", 2); EXPECT("\xEF\xBF\xBD(");               /* broken sequence */
    start(); export_xml_text(out, "\xC0\xAF", 2); EXPECT("\xEF\xBF\xBD\xEF\xBF\xBD"); /* overlong */
    start(); export_xml_text(out, "\xED\xA0\x80", 3); EXPECT("\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"); /* surrogate, per byte */
    start(); export_xml_text(out, "\xEF\xBF\xBE", 3); EXPECT("\xEF\xBF\xBD");         /* U+FFFE */
    start(); export_xml_text(out, "caf\xC3\xA9 \xF0\x9F\x98\x80", 10); EXPECT("caf\xC3\xA9 \xF0\x9F\x98\x80");
    start(); export_xml_text(out, "a\0b", 3); EXPECT("a\xEF\xBF\xBD" "b");
}

static const export_col cols3[] = {
    { .name = "id", .numeric = 1 },
    { .name = "na,me" },
    { .name = "data", .binary = 1 },
};

static void test_csv_writer(void) {
    export_opts o = export_default_opts();
    char *r1[] = { "1", "x", "\x01\xFF" };
    char *r2[] = { "2", NULL, NULL };
    char *r3[] = { "3", "", "" };
    unsigned long l1[] = { 1, 1, 2 }, l2[] = { 1, 0, 0 }, l3[] = { 1, 0, 0 };

    start();
    export_writer *w = export_begin(out, EXPORT_CSV, &o, "t", NULL, 0, 3, cols3);
    CHECK(w != NULL);
    CHECK(export_row(w, r1, l1) == 0);
    CHECK(export_row(w, r2, l2) == 0);
    CHECK(export_row(w, r3, l3) == 0);
    CHECK(export_end(w) == 0);
    EXPECT("\xEF\xBB\xBFid,\"na,me\",data\r\n"
           "1,x,0x01FF\r\n"
           "2,,\r\n"
           "3,\"\",\"\"\r\n");

    o.csv_bom = 0;
    o.csv_null_as_n = 1;
    start();
    w = export_begin(out, EXPORT_CSV, &o, "t", NULL, 0, 3, cols3);
    export_row(w, r2, l2);
    CHECK(export_end(w) == 0);
    EXPECT("id,\"na,me\",data\r\n2,\\N,\\N\r\n");
}

static void test_xml_writer(void) {
    char *r1[] = { "1", "", NULL };
    char *r2[] = { "2", "<b>", "\xAB" };
    unsigned long l1[] = { 1, 0, 0 }, l2[] = { 1, 3, 1 };
    start();
    export_writer *w = export_begin(out, EXPORT_XML, NULL, "us\"ers", NULL, 0, 3, cols3);
    export_row(w, r1, l1);
    export_row(w, r2, l2);
    CHECK(export_end(w) == 0);
    EXPECT("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<table name=\"us&quot;ers\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">\n"
           "  <row>\n"
           "    <field name=\"id\">1</field>\n"
           "    <field name=\"na,me\"></field>\n"
           "    <field name=\"data\" xsi:nil=\"true\"/>\n"
           "  </row>\n"
           "  <row>\n"
           "    <field name=\"id\">2</field>\n"
           "    <field name=\"na,me\">&lt;b&gt;</field>\n"
           "    <field name=\"data\" encoding=\"hex\">AB</field>\n"
           "  </row>\n"
           "</table>\n");
}

static void test_sql_writer(void) {
    export_opts o = export_default_opts();
    char *r1[] = { "1", "O'Brien", "\x00\x10" };
    char *r2[] = { "2", NULL, "" };
    unsigned long l1[] = { 1, 7, 2 }, l2[] = { 1, 0, 0 };

    start();
    export_writer *w = export_begin(out, EXPORT_SQL, &o, "t`x", "CREATE TABLE `t``x` (...)", 0, 3, cols3);
    export_row(w, r1, l1);
    export_row(w, r2, l2);
    CHECK(export_end(w) == 0);
    const char *s = finish();
    CHECK(strstr(s, "SET NAMES utf8mb4;\n") != NULL);
    CHECK(strstr(s, "DROP TABLE IF EXISTS `t``x`;\nCREATE TABLE `t``x` (...);\n") != NULL);
    CHECK(strstr(s, "INSERT INTO `t``x` (`id`, `na,me`, `data`) VALUES\n"
                    "(1,'O\\'Brien',0x0010),\n"
                    "(2,NULL,'');\n") != NULL);
    CHECK(strstr(s, "SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS;") != NULL);

    /* a numeric column holding something odd still gets quoted */
    char *odd[] = { "1 OR 1", "a", NULL };
    unsigned long lodd[] = { 6, 1, 0 };
    start();
    w = export_begin(out, EXPORT_SQL, &o, "t", NULL, 0, 3, cols3);
    export_row(w, odd, lodd);
    export_end(w);
    s = finish();
    CHECK(strstr(s, "('1 OR 1','a',NULL)") != NULL);

    /* batching: 2500 rows -> 3 INSERT statements */
    start();
    w = export_begin(out, EXPORT_SQL, &o, "t", NULL, 0, 3, cols3);
    for (int i = 0; i < 2500; i++) export_row(w, r2, l2);
    export_end(w);
    s = finish();
    CHECK(count(s, "INSERT INTO") == 3);
    CHECK(count(s, "(2,NULL,'')") == 2500);

    /* data only: no DROP/CREATE */
    o.sql_structure = 0;
    start();
    w = export_begin(out, EXPORT_SQL, &o, "t", "CREATE TABLE t (...)", 0, 3, cols3);
    export_row(w, r2, l2);
    export_end(w);
    s = finish();
    CHECK(strstr(s, "DROP") == NULL && strstr(s, "CREATE") == NULL && strstr(s, "INSERT") != NULL);

    /* structure only */
    o = export_default_opts();
    o.sql_data = 0;
    start();
    w = export_begin(out, EXPORT_SQL, &o, "t", "CREATE TABLE t (...)", 0, 3, cols3);
    export_row(w, r2, l2);
    export_end(w);
    s = finish();
    CHECK(strstr(s, "CREATE TABLE t (...);") != NULL && strstr(s, "INSERT") == NULL);

    /* a view: DROP VIEW + CREATE VIEW, never rows */
    o = export_default_opts();
    start();
    w = export_begin(out, EXPORT_SQL, &o, "v", "CREATE VIEW `v` AS select 1", 1, 3, cols3);
    export_row(w, r2, l2);
    export_end(w);
    s = finish();
    CHECK(strstr(s, "DROP VIEW IF EXISTS `v`;\nCREATE VIEW `v` AS select 1;") != NULL);
    CHECK(strstr(s, "INSERT") == NULL);
}

/* A table and a view as parts of a database dump: no SET header/footer of
 * their own, a short comment instead. */
static void test_sql_in_dump(void) {
    export_opts o = export_default_opts();
    o.sql_in_dump = 1;
    char *r1[] = { "1", "x", NULL };
    unsigned long l1[] = { 1, 1, 0 };

    start();
    export_writer *w = export_begin(out, EXPORT_SQL, &o, "t", "CREATE TABLE `t` (...)", 0, 3, cols3);
    export_row(w, r1, l1);
    CHECK(export_end(w) == 0);
    EXPECT("-- Table `t`\n"
           "DROP TABLE IF EXISTS `t`;\n"
           "CREATE TABLE `t` (...);\n\n"
           "INSERT INTO `t` (`id`, `na,me`, `data`) VALUES\n"
           "(1,'x',NULL);\n\n");

    /* no rows: nothing after the CREATE */
    start();
    w = export_begin(out, EXPORT_SQL, &o, "empty", "CREATE TABLE `empty` (...)", 0, 3, cols3);
    export_end(w);
    EXPECT("-- Table `empty`\nDROP TABLE IF EXISTS `empty`;\nCREATE TABLE `empty` (...);\n\n");

    start();
    w = export_begin(out, EXPORT_SQL, &o, "v", "CREATE VIEW `v` AS select 1", 1, 0, NULL);
    export_end(w);
    EXPECT("-- View `v`\nDROP VIEW IF EXISTS `v`;\nCREATE VIEW `v` AS select 1;\n\n");

    /* the standalone file keeps its header and footer */
    o.sql_in_dump = 0;
    start();
    w = export_begin(out, EXPORT_SQL, &o, "t", "CREATE TABLE `t` (...)", 0, 3, cols3);
    export_end(w);
    const char *s = finish();
    CHECK(strncmp(s, "-- sqlc export of table `t`\n\nSET NAMES utf8mb4;\n", 48) == 0);
    CHECK(strstr(s, "SET TIME_ZONE=@OLD_TIME_ZONE;\n") != NULL);
}

static void test_dump_header_footer(void) {
    start();
    export_sql_dump_header(out, "shop", 2, 1, 0);
    EXPECT("-- sqlc export of database `shop`: 2 tables, 1 view\n\n"
           "SET NAMES utf8mb4;\n"
           "SET @OLD_TIME_ZONE=@@TIME_ZONE, TIME_ZONE='+00:00';\n"
           "SET @OLD_SQL_MODE=@@SQL_MODE, SQL_MODE='NO_AUTO_VALUE_ON_ZERO';\n"
           "SET @OLD_FOREIGN_KEY_CHECKS=@@FOREIGN_KEY_CHECKS, FOREIGN_KEY_CHECKS=0;\n\n");
    start();
    export_sql_dump_header(out, "x", 1, 0, 0);
    CHECK(strncmp(finish(), "-- sqlc export of database `x`: 1 table, 0 views\n", 49) == 0);
    /* a line break in a name must not end the comment and start SQL */
    start();
    export_sql_dump_header(out, "a`b\nSELECT 1;", 0, 2, 0);
    CHECK(strncmp(finish(), "-- sqlc export of database `a``b?SELECT 1;`: 0 tables, 2 views\n", 63) == 0);
    /* a dump of selected objects says so */
    start();
    export_sql_dump_header(out, "shop", 3, 0, 1);
    CHECK(strncmp(finish(), "-- sqlc export of selected tables from database `shop`: 3 tables, 0 views\n\n"
                            "SET NAMES utf8mb4;\n", 92) == 0);
    start();
    export_sql_dump_header(out, "s\nq", 1, 1, 1);
    CHECK(strncmp(finish(), "-- sqlc export of selected tables from database `s?q`: 1 table, 1 view\n", 70) == 0);
    start();
    export_sql_dump_footer(out);
    EXPECT("SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS;\n"
           "SET SQL_MODE=@OLD_SQL_MODE;\n"
           "SET TIME_ZONE=@OLD_TIME_ZONE;\n");
    start();
    export_sql_dump_skipped(out, "seq", "SEQUENCE");
    EXPECT("-- Skipped `seq` (SEQUENCE): not exported\n\n");
    start();
    export_sql_dump_skipped(out, "s\r\nq", NULL);
    EXPECT("-- Skipped `s??q` (unknown type): not exported\n\n");
    /* the table writer's own comment is guarded the same way */
    export_opts o = export_default_opts();
    start();
    export_writer *w = export_begin(out, EXPORT_SQL, &o, "t\nx", NULL, 0, 0, NULL);
    export_end(w);
    CHECK(strncmp(finish(), "-- sqlc export of table `t?x`\n", 30) == 0);
    start();
    export_sql_comment_ident(out, "caf\xC3\xA9 tab\there");
    EXPECT("`caf\xC3\xA9 tab?here`");
}

static void test_object_kind(void) {
    CHECK(export_object_kind("BASE TABLE") == EXPORT_OBJ_TABLE);
    CHECK(export_object_kind("SYSTEM VERSIONED") == EXPORT_OBJ_TABLE);
    CHECK(export_object_kind("VIEW") == EXPORT_OBJ_VIEW);
    CHECK(export_object_kind("SYSTEM VIEW") == EXPORT_OBJ_SKIP);
    CHECK(export_object_kind("SEQUENCE") == EXPORT_OBJ_SKIP);
    CHECK(export_object_kind("TEMPORARY") == EXPORT_OBJ_SKIP);
    CHECK(export_object_kind("view") == EXPORT_OBJ_SKIP);
    CHECK(export_object_kind("") == EXPORT_OBJ_SKIP);
    CHECK(export_object_kind(NULL) == EXPORT_OBJ_SKIP);
}

/* order[] as "a,b,c" */
static const char *order_str(size_t n, const char *const *names, const size_t *order) {
    static char s[256];
    s[0] = '\0';
    for (size_t i = 0; i < n; i++) {
        if (i) strncat(s, ",", sizeof s - strlen(s) - 1);
        strncat(s, names[order[i]], sizeof s - strlen(s) - 1);
    }
    return s;
}

#define EXPECT_ORDER(n, names, defs, expected) do { \
    size_t ord_[16]; \
    CHECK(export_view_order(n, names, defs, ord_) == 0); \
    const char *got_ = order_str(n, names, ord_); \
    if (strcmp(got_, expected) != 0) { \
        fprintf(stderr, "%s:%d: expected order [%s]\n  got [%s]\n", __FILE__, __LINE__, expected, got_); \
        failures++; \
    } \
} while (0)

static void test_view_order(void) {
    /* independent views keep their (name) order */
    const char *n1[] = { "a", "b", "c" };
    const char *d1[] = { "CREATE VIEW `a` AS select `t`.`x` from `t`",
                         "CREATE VIEW `b` AS select 1", NULL };
    EXPECT_ORDER(3, n1, d1, "a,b,c");

    /* a uses c, c uses b: b, c, a */
    const char *d2[] = { "CREATE VIEW `a` AS select * from `c`",
                         "CREATE VIEW `b` AS select 1",
                         "CREATE VIEW `c` AS select * from `db`.`b`" };
    EXPECT_ORDER(3, n1, d2, "b,c,a");

    /* a name inside another doesn't count: `ab` is not `a` */
    const char *n3[] = { "ab", "a" };
    const char *d3[] = { "CREATE VIEW `ab` AS select 1", "CREATE VIEW `a` AS select * from `ab`" };
    EXPECT_ORDER(2, n3, d3, "ab,a");
    const char *d3b[] = { "CREATE VIEW `ab` AS select * from `abc`", "CREATE VIEW `a` AS select 1" };
    EXPECT_ORDER(2, n3, d3b, "ab,a");

    /* backticks in names are matched doubled, as SHOW CREATE writes them */
    const char *n4[] = { "x", "we`ird" };
    const char *d4[] = { "CREATE VIEW `x` AS select * from `we``ird`", "CREATE VIEW `we``ird` AS select 1" };
    EXPECT_ORDER(2, n4, d4, "we`ird,x");

    /* a cycle (or false match) still yields every view once */
    const char *n5[] = { "p", "q", "r" };
    const char *d5[] = { "CREATE VIEW `p` AS select * from `q`",
                         "CREATE VIEW `q` AS select * from `p`",
                         "CREATE VIEW `r` AS select * from `q`" };
    EXPECT_ORDER(3, n5, d5, "p,q,r");

    /* non-ASCII names */
    const char *n6[] = { "caf\xC3\xA9", "z" };
    const char *d6[] = { "CREATE VIEW `caf\xC3\xA9` AS select * from `z`", "CREATE VIEW `z` AS select 1" };
    EXPECT_ORDER(2, n6, d6, "z,caf\xC3\xA9");

    /* empty and single */
    size_t one[1] = { 99 };
    CHECK(export_view_order(0, NULL, NULL, one) == 0 && one[0] == 99);
    EXPECT_ORDER(1, n1, d1, "a");
}

/* keep[] as "0110" */
static const char *keep_str(size_t n, const unsigned char *keep) {
    static char s[64];
    for (size_t i = 0; i < n && i + 1 < sizeof s; i++) s[i] = keep[i] ? '1' : '0';
    s[n < sizeof s ? n : sizeof s - 1] = '\0';
    return s;
}

static void test_select_names(void) {
    const char *names[] = { "orders", "customers", "a`b", "caf\xC3\xA9", NULL, "Orders", "v" };
    size_t n = sizeof names / sizeof *names;
    unsigned char keep[7];
    size_t missing = 99;

    /* nothing selected: nothing kept, nothing missing */
    memset(keep, 1, sizeof keep);
    CHECK(export_select_names(n, names, 0, NULL, keep, &missing) == 0);
    CHECK(strcmp(keep_str(n, keep), "0000000") == 0 && missing == 99);

    /* exact, case-sensitive; odd characters and non-ASCII; selection order irrelevant */
    const char *s1[] = { "v", "a`b", "caf\xC3\xA9", "orders" };
    CHECK(export_select_names(n, names, 4, s1, keep, &missing) == 0);
    CHECK(strcmp(keep_str(n, keep), "1011001") == 0);
    CHECK(missing == 99);

    /* duplicates in the selection */
    const char *s2[] = { "Orders", "Orders" };
    CHECK(export_select_names(n, names, 2, s2, keep, NULL) == 0);
    CHECK(strcmp(keep_str(n, keep), "0000010") == 0);

    /* a name that is gone: the first missing one is reported, the rest kept */
    const char *s3[] = { "customers", "gone", "ORDERS", "v" };
    CHECK(export_select_names(n, names, 4, s3, keep, &missing) == 1);
    CHECK(missing == 1);
    CHECK(strcmp(keep_str(n, keep), "0100001") == 0);

    /* a NULL or empty selected name is not found (the NULL name never matches) */
    const char *s4[] = { NULL };
    CHECK(export_select_names(n, names, 1, s4, keep, &missing) == 1 && missing == 0);
    CHECK(strcmp(keep_str(n, keep), "0000000") == 0);
    const char *s5[] = { "" };
    CHECK(export_select_names(n, names, 1, s5, keep, &missing) == 1 && missing == 0);

    /* no objects at all */
    const char *s6[] = { "t" };
    CHECK(export_select_names(0, NULL, 1, s6, keep, &missing) == 1 && missing == 0);

    /* equal names in the list (not possible in one schema, but robust) */
    const char *dup[] = { "t", "u", "t" };
    unsigned char k3[3];
    CHECK(export_select_names(3, dup, 1, s6, k3, NULL) == 0);
    CHECK(strcmp(keep_str(3, k3), "101") == 0);
}

static void test_dump_basename(void) {
    char b[64];
    export_dump_basename(b, sizeof b, "shop", 0);
    CHECK(strcmp(b, "shop") == 0);
    export_dump_basename(b, sizeof b, "shop", 3);
    CHECK(strcmp(b, "shop-3-tables") == 0);
    export_dump_basename(b, sizeof b, "shop", 1);
    CHECK(strcmp(b, "shop-1-table") == 0);
    export_dump_basename(b, sizeof b, NULL, 2);
    CHECK(strcmp(b, "database-2-tables") == 0);
    export_dump_basename(b, sizeof b, "", 0);
    CHECK(strcmp(b, "database") == 0);
    export_dump_basename(b, 8, "a_long_database_name", 12);
    CHECK(strcmp(b, "a_long_") == 0);
    b[0] = 'x';
    export_dump_basename(b, 0, "shop", 0); /* no room: untouched */
    CHECK(b[0] == 'x');
}

static int pct(size_t index, size_t count, unsigned long long rows, unsigned long long est,
               unsigned long long est_before, unsigned long long est_total) {
    export_progress p = { "t", index, count, rows, est, est_before, est_total };
    return export_progress_percent(&p);
}

static void test_progress_percent(void) {
    /* one table: rows against the estimate, never 100 */
    CHECK(pct(1, 1, 0, 1000, 0, 1000) == 0);
    CHECK(pct(1, 1, 500, 1000, 0, 1000) == 50);
    CHECK(pct(1, 1, 999, 1000, 0, 1000) == 99);
    CHECK(pct(1, 1, 1000, 1000, 0, 1000) == 99);
    CHECK(pct(1, 1, 5000, 1000, 0, 1000) == 99); /* estimate too low */
    /* no estimate (a view, an empty or unknown table): indeterminate */
    CHECK(pct(1, 1, 0, 0, 0, 0) == -1);
    CHECK(pct(1, 1, 12345, 0, 0, 0) == -1);
    CHECK(export_progress_percent(NULL) == -1);
    CHECK(pct(0, 0, 0, 0, 0, 0) == -1);

    /* several: weighted by estimate, a huge table doesn't leave it at 90% */
    /* tables a (10 rows), b (1,000,000), c (10): weights 11, 1000001, 11 */
    CHECK(pct(1, 3, 0, 10, 0, 1000020) == 0);
    CHECK(pct(2, 3, 0, 1000000, 10, 1000020) == 0);           /* 11 / 1000023 */
    CHECK(pct(2, 3, 500100, 1000000, 10, 1000020) == 50);     /* moves within the table */
    CHECK(pct(2, 3, 2000000, 1000000, 10, 1000020) == 99);    /* capped at its estimate */
    CHECK(pct(3, 3, 0, 10, 1000010, 1000020) == 99);
    /* no estimates at all: "i of n" steps */
    CHECK(pct(1, 4, 0, 0, 0, 0) == 0);
    CHECK(pct(2, 4, 0, 0, 0, 0) == 25);
    CHECK(pct(3, 4, 70000, 0, 0, 0) == 50);
    CHECK(pct(4, 4, 0, 0, 0, 0) == 75);
    /* out-of-range index is clamped, still below 100 */
    CHECK(pct(9, 4, 0, 0, 0, 0) == 75);
    /* huge numbers don't overflow */
    CHECK(pct(2, 2, 0, 18000000000000000000ULL, 0, 18000000000000000000ULL) < 100);
}

static void test_safe_filename(void) {
    char b[64];
    export_safe_filename(b, sizeof b, "orders");
    CHECK(strcmp(b, "orders") == 0);
    export_safe_filename(b, sizeof b, "a/b\\c:d*e?f\"g<h>i|j");
    CHECK(strcmp(b, "a-b-c-d-e-f-g-h-i-j") == 0);
    export_safe_filename(b, sizeof b, "tab\there");
    CHECK(strcmp(b, "tab-here") == 0);
    export_safe_filename(b, sizeof b, "trailing. . ");
    CHECK(strcmp(b, "trailing") == 0);
    export_safe_filename(b, sizeof b, "con");
    CHECK(strcmp(b, "_con") == 0);
    export_safe_filename(b, sizeof b, "NUL.sql");
    CHECK(strcmp(b, "_NUL.sql") == 0);
    export_safe_filename(b, sizeof b, "com1");
    CHECK(strcmp(b, "_com1") == 0);
    export_safe_filename(b, sizeof b, "console");
    CHECK(strcmp(b, "console") == 0);
    export_safe_filename(b, sizeof b, "com0");
    CHECK(strcmp(b, "com0") == 0);
    export_safe_filename(b, sizeof b, "");
    CHECK(strcmp(b, "export") == 0);
    export_safe_filename(b, sizeof b, NULL);
    CHECK(strcmp(b, "export") == 0);
    export_safe_filename(b, sizeof b, "...");
    CHECK(strcmp(b, "export") == 0);
    export_safe_filename(b, sizeof b, "\xC5\xA1ifre"); /* "šifre": kept */
    CHECK(strcmp(b, "\xC5\xA1ifre") == 0);
    export_safe_filename(b, 4, "a\xC5\xA1" "b"); /* room for "a" + 2 bytes: "aš" */
    CHECK(strcmp(b, "a\xC5\xA1") == 0);
    export_safe_filename(b, 3, "a\xC5\xA1" "b"); /* "š" doesn't fit whole: "a" */
    CHECK(strcmp(b, "a") == 0);
}

static void test_format_duration(void) {
    char b[32];
    export_format_duration(b, sizeof b, 0); CHECK(strcmp(b, "0s") == 0);
    export_format_duration(b, sizeof b, -3); CHECK(strcmp(b, "0s") == 0);
    export_format_duration(b, sizeof b, 14.7); CHECK(strcmp(b, "14s") == 0);
    export_format_duration(b, sizeof b, 59.9); CHECK(strcmp(b, "59s") == 0);
    export_format_duration(b, sizeof b, 60); CHECK(strcmp(b, "1m 00s") == 0);
    export_format_duration(b, sizeof b, 134); CHECK(strcmp(b, "2m 14s") == 0);
    export_format_duration(b, sizeof b, 3599); CHECK(strcmp(b, "59m 59s") == 0);
    export_format_duration(b, sizeof b, 3600); CHECK(strcmp(b, "1h 00m") == 0);
    export_format_duration(b, sizeof b, 7384); CHECK(strcmp(b, "2h 03m") == 0);
    export_format_duration(b, 3, 134); CHECK(strcmp(b, "2m") == 0); /* truncated */
}

static void test_format_count(void) {
    char b[32];
    export_format_count(b, sizeof b, 0); CHECK(strcmp(b, "0") == 0);
    export_format_count(b, sizeof b, 999); CHECK(strcmp(b, "999") == 0);
    export_format_count(b, sizeof b, 1000); CHECK(strcmp(b, "1,000") == 0);
    export_format_count(b, sizeof b, 1204331); CHECK(strcmp(b, "1,204,331") == 0);
    export_format_count(b, sizeof b, 18446744073709551615ULL);
    CHECK(strcmp(b, "18,446,744,073,709,551,615") == 0);
    export_format_count(b, 4, 1204331); CHECK(strcmp(b, "1,2") == 0);
}

static void test_tilde_path(void) {
    char b[64];
    export_tilde_path(b, sizeof b, "/Users/me/dump.sql", "/Users/me"); CHECK(strcmp(b, "~/dump.sql") == 0);
    export_tilde_path(b, sizeof b, "/Users/me/dump.sql", "/Users/me/"); CHECK(strcmp(b, "~/dump.sql") == 0);
    export_tilde_path(b, sizeof b, "/Users/me", "/Users/me"); CHECK(strcmp(b, "~") == 0);
    export_tilde_path(b, sizeof b, "/Users/meg/x.sql", "/Users/me"); CHECK(strcmp(b, "/Users/meg/x.sql") == 0);
    export_tilde_path(b, sizeof b, "/tmp/x.sql", "/Users/me"); CHECK(strcmp(b, "/tmp/x.sql") == 0);
    export_tilde_path(b, sizeof b, "/tmp/x.sql", NULL); CHECK(strcmp(b, "/tmp/x.sql") == 0);
    export_tilde_path(b, sizeof b, "/tmp/x.sql", "/"); CHECK(strcmp(b, "/tmp/x.sql") == 0);
}

/* Files in dir other than . and .. */
static int count_files(const char *dir, const char *containing) {
    DIR *d = opendir(dir);
    int n = 0;
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..") &&
            (!containing || strstr(e->d_name, containing)))
            n++;
    if (d) closedir(d);
    return n;
}

static char *slurp(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char *s = calloc(1, 256);
    if (s) fread(s, 1, 255, f);
    fclose(f);
    return s;
}

static void test_temp_file(void) {
    const char *dir = "build/export-temp-test";
    const char *path = "build/export-temp-test/out.sql";
    mkdir("build", 0755);
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) { CHECK(!"mkdir"); return; }
    remove(path);

    /* finished: the target only, no temp file left */
    char *tmp = NULL, *err = NULL;
    FILE *f = export_temp_open(path, &tmp, &err);
    CHECK(f && tmp && !err);
    if (!f) return;
    CHECK(strstr(tmp, "out.sql.sqlc-") != NULL);
    fputs("first", f);
    CHECK(export_temp_finish(f, tmp, path, 1, 0, &err) == 0 && !err);
    char *got = slurp(path);
    CHECK(got && strcmp(got, "first") == 0);
    free(got);
    CHECK(count_files(dir, NULL) == 1);

    /* cancelled with a target already there: target untouched, temp gone */
    f = export_temp_open(path, &tmp, &err);
    CHECK(f != NULL);
    if (!f) return;
    fputs("partial", f);
    CHECK(export_temp_finish(f, tmp, path, 0, 0, &err) == -1);
    CHECK(err == NULL); /* a cancel has nothing to report */
    got = slurp(path);
    CHECK(got && strcmp(got, "first") == 0);
    free(got);
    CHECK(count_files(dir, ".sqlc-") == 0);

    /* cancelled with no target: nothing at all */
    remove(path);
    f = export_temp_open(path, &tmp, &err);
    CHECK(f != NULL);
    if (!f) return;
    fputs("partial", f);
    CHECK(export_temp_finish(f, tmp, path, 0, 0, &err) == -1 && !err);
    CHECK(count_files(dir, NULL) == 0);

    /* a write error: reported, nothing left */
    f = export_temp_open(path, &tmp, &err);
    CHECK(f != NULL);
    if (!f) return;
    CHECK(export_temp_finish(f, tmp, path, 1, ENOSPC, &err) == -1);
    CHECK(err && strstr(err, "Could not write") != NULL);
    free(err);
    err = NULL;
    CHECK(count_files(dir, NULL) == 0);

    /* a directory that doesn't exist: an error, no FILE */
    f = export_temp_open("build/export-temp-test/no/such/out.sql", &tmp, &err);
    CHECK(f == NULL && tmp == NULL && err && strstr(err, "Could not create") != NULL);
    free(err);
    rmdir(dir);
}

int main(void) {
    test_sql_escaping();
    test_csv_escaping();
    test_xml_escaping();
    test_csv_writer();
    test_xml_writer();
    test_sql_writer();
    test_sql_in_dump();
    test_dump_header_footer();
    test_object_kind();
    test_view_order();
    test_select_names();
    test_dump_basename();
    test_safe_filename();
    test_progress_percent();
    test_format_duration();
    test_format_count();
    test_tilde_path();
    test_temp_file();
    free(buf);
    if (failures) { fprintf(stderr, "export_test: %d failure(s)\n", failures); return 1; }
    printf("export_test: all checks passed\n");
    return 0;
}
