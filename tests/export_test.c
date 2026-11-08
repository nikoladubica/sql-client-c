/* Unit tests for the export writers (no database needed). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int main(void) {
    test_sql_escaping();
    test_csv_escaping();
    test_xml_escaping();
    test_csv_writer();
    test_xml_writer();
    test_sql_writer();
    free(buf);
    if (failures) { fprintf(stderr, "export_test: %d failure(s)\n", failures); return 1; }
    printf("export_test: all checks passed\n");
    return 0;
}
