/* Tests for src/ai.c and src/json.c: request body, response parsing, editor
 * text and the destructive-statement check. No network. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ai.h"
#include "json.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", \
                                          __FILE__, __LINE__, #c); failures++; } } while (0)

static int streq(const char *a, const char *b) { return a && b && strcmp(a, b) == 0; }

static void test_json(void) {
    json_value *v = json_parse(" {\"a\": [1, -2.5e1, true, false, null], "
                               "\"s\": \"q\\\"\\\\\\/\\n\\u00e9\\ud83d\\ude00\", \"o\": {}} ");
    CHECK(v && v->type == JSON_OBJECT && v->n == 3);
    const json_value *a = json_get(v, "a");
    CHECK(a && a->type == JSON_ARRAY && a->n == 5);
    if (a && a->n == 5) {
        CHECK(a->items[0].number == 1);
        CHECK(a->items[1].number == -25);
        CHECK(a->items[2].type == JSON_BOOL && a->items[2].boolean);
        CHECK(a->items[4].type == JSON_NULL);
    }
    CHECK(streq(json_get_str(v, "s"), "q\"\\/\n\xC3\xA9\xF0\x9F\x98\x80"));
    CHECK(json_get(v, "missing") == NULL);
    CHECK(json_get_str(v, "a") == NULL);
    json_free(v);

    CHECK(json_parse("") == NULL);
    CHECK(json_parse("{\"a\":1,}") == NULL);
    CHECK(json_parse("[1 2]") == NULL);
    CHECK(json_parse("\"unterminated") == NULL);
    CHECK(json_parse("{} x") == NULL);
    CHECK(json_parse("\"tab\there\"") == NULL); /* raw control character */
    char deep[200];
    memset(deep, '[', 100); memset(deep + 100, ']', 99); deep[199] = '\0';
    CHECK(json_parse(deep) == NULL); /* too deep (and unbalanced) */

    strbuf b = { 0 };
    sb_add_json_string(&b, "a\"b\\c\n\x01\xC3\xA9");
    char *s = sb_take(&b);
    CHECK(streq(s, "\"a\\\"b\\\\c\\n\\u0001\xC3\xA9\""));
    free(s);
}

static void test_request(void) {
    char *body = ai_build_request(AI_MODEL, "10.11.6-MariaDB", "shop",
                                  "TABLE customers\n  id int NOT NULL PRI\n",
                                  "top 10 \"customers\"\nby revenue");
    json_value *v = json_parse(body);
    CHECK(v != NULL);
    CHECK(streq(json_get_str(v, "model"), "claude-opus-5-5"));
    CHECK(streq(json_get_str(v, "fallbacks"), "default"));
    const json_value *mt = json_get(v, "max_tokens");
    CHECK(mt && mt->type == JSON_NUMBER && mt->number >= 1024);
    CHECK(json_get(v, "temperature") == NULL);
    CHECK(json_get(v, "thinking") == NULL);

    const json_value *sys = json_get(v, "system");
    CHECK(sys && sys->type == JSON_ARRAY && sys->n == 2);
    if (sys && sys->n == 2) {
        const char *ctx = json_get_str(&sys->items[1], "text");
        CHECK(ctx && strstr(ctx, "10.11.6-MariaDB") && strstr(ctx, "Current database: shop") &&
              strstr(ctx, "TABLE customers"));
        CHECK(streq(json_get_str(json_get(&sys->items[1], "cache_control"), "type"), "ephemeral"));
    }
    const json_value *msgs = json_get(v, "messages");
    CHECK(msgs && msgs->type == JSON_ARRAY && msgs->n == 1);
    if (msgs && msgs->n == 1) {
        CHECK(streq(json_get_str(&msgs->items[0], "role"), "user"));
        CHECK(streq(json_get_str(&msgs->items[0], "content"), "top 10 \"customers\"\nby revenue"));
    }
    const json_value *oc = json_get(v, "output_config");
    CHECK(streq(json_get_str(oc, "effort"), "medium"));
    const json_value *fmt = json_get(oc, "format");
    CHECK(streq(json_get_str(fmt, "type"), "json_schema"));
    const json_value *schema = json_get(fmt, "schema");
    CHECK(json_get(schema, "additionalProperties") &&
          json_get(schema, "additionalProperties")->type == JSON_BOOL);
    json_free(v);
    free(body);

    /* no database */
    body = ai_build_request(AI_MODEL, "8.0.36", NULL, NULL, "list databases");
    v = json_parse(body);
    CHECK(v != NULL);
    const json_value *sys2 = json_get(v, "system");
    if (sys2 && sys2->n == 2)
        CHECK(strstr(json_get_str(&sys2->items[1], "text"), "(none selected)") != NULL);
    json_free(v);
    free(body);
}

static void test_response(void) {
    ai_result r = { 0 };
    const char *ok =
        "{\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-opus-5-5\","
        "\"content\":[{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"x\"},"
        "{\"type\":\"text\",\"text\":\"{\\\"explanation\\\":\\\"Sums orders.\\\","
        "\\\"sql\\\":\\\"SELECT 1;\\\"}\"}],"
        "\"stop_reason\":\"end_turn\",\"stop_details\":null,\"usage\":{\"input_tokens\":10}}";
    CHECK(ai_parse_response(200, ok, &r) == 0);
    CHECK(streq(r.explanation, "Sums orders."));
    CHECK(streq(r.sql, "SELECT 1;"));
    CHECK(streq(r.model, "claude-opus-5-5"));
    CHECK(r.error == NULL);
    ai_result_free(&r);

    /* after a fallback, only the model that finished counts */
    const char *fb =
        "{\"model\":\"claude-opus-4-8\",\"stop_reason\":\"end_turn\",\"content\":["
        "{\"type\":\"text\",\"text\":\"{\\\"expl\"},"
        "{\"type\":\"fallback\",\"from\":{\"model\":\"claude-opus-5-5\"},\"to\":{\"model\":\"claude-opus-4-8\"}},"
        "{\"type\":\"text\",\"text\":\"{\\\"explanation\\\":\\\"\\\",\\\"sql\\\":\\\"SELECT 2;\\\"}\"}]}";
    CHECK(ai_parse_response(200, fb, &r) == 0);
    CHECK(streq(r.sql, "SELECT 2;"));
    CHECK(streq(r.model, "claude-opus-4-8"));
    ai_result_free(&r);

    const char *refusal =
        "{\"model\":\"claude-opus-5-5\",\"content\":[],\"stop_reason\":\"refusal\","
        "\"stop_details\":{\"type\":\"refusal\",\"category\":null,\"explanation\":\"Policy.\"}}";
    CHECK(ai_parse_response(200, refusal, &r) == -1);
    CHECK(r.error && strstr(r.error, "declined") && strstr(r.error, "Policy."));
    ai_result_free(&r);

    const char *cut = "{\"content\":[{\"type\":\"text\",\"text\":\"{\\\"expl\"}],\"stop_reason\":\"max_tokens\"}";
    CHECK(ai_parse_response(200, cut, &r) == -1);
    CHECK(r.error && strstr(r.error, "cut off"));
    ai_result_free(&r);

    const char *err401 =
        "{\"type\":\"error\",\"error\":{\"type\":\"authentication_error\",\"message\":\"invalid x-api-key\"}}";
    CHECK(ai_parse_response(401, err401, &r) == -1);
    CHECK(r.error && strstr(r.error, "API key") && strstr(r.error, "401") &&
          strstr(r.error, "invalid x-api-key"));
    ai_result_free(&r);

    CHECK(ai_parse_response(529, "<html>overloaded</html>", &r) == -1);
    CHECK(r.error && strstr(r.error, "overloaded"));
    ai_result_free(&r);

    CHECK(ai_parse_response(200, "{\"content\":[{\"type\":\"text\",\"text\":\"SELECT 1\"}]}", &r) == -1);
    CHECK(r.error && strstr(r.error, "format"));
    ai_result_free(&r);

    CHECK(ai_parse_response(200, "not json", &r) == -1);
    ai_result_free(&r);

    CHECK(ai_generate("", "{}", &r) == -1 && r.error);
    ai_result_free(&r);
}

static void test_format(void) {
    ai_result r = { 0 };
    r.explanation = strdup("Joins orders to customers and sums the order totals for last month, "
                           "then keeps the ten biggest spenders.\nAssumes orders.created_at.");
    r.sql = strdup("\nSELECT c.name\nFROM customers c\nLIMIT 10;");
    char *t = ai_format("top 10 customers  \n", &r);
    CHECK(t != NULL);
    if (t) {
        const char *head = "-- Request: top 10 customers\n--\n-- Joins orders";
        CHECK(strncmp(t, head, strlen(head)) == 0);
        CHECK(strstr(t, "\n-- Assumes orders.created_at.\n\nSELECT c.name\nFROM customers c\nLIMIT 10;\n"));
        /* every line before the SQL is a comment of at most ~80 columns */
        const char *sql = strstr(t, "\n\nSELECT");
        for (const char *line = t; line && line < sql; ) {
            const char *nl = strchr(line, '\n');
            CHECK(strncmp(line, "--", 2) == 0);
            CHECK(nl && nl - line <= 80);
            line = nl ? nl + 1 : NULL;
        }
        CHECK(t[strlen(t) - 1] == '\n');
    }
    free(t);

    free(r.sql);
    r.sql = strdup("");
    t = ai_format("impossible", &r);
    CHECK(t && strstr(t, "didn't write a query"));
    free(t);
    ai_result_free(&r);
}

static void expect_destructive(const char *sql, const char *want) {
    char *got = ai_destructive(sql);
    if (want == NULL ? got != NULL : !streq(got, want)) {
        fprintf(stderr, "ai_destructive(\"%s\") = %s, want %s\n", sql,
                got ? got : "NULL", want ? want : "NULL");
        failures++;
    }
    free(got);
}

static void test_destructive(void) {
    expect_destructive("SELECT * FROM t;", NULL);
    expect_destructive("SELECT 'DROP TABLE x; DELETE FROM y' AS s;", NULL);
    expect_destructive("-- DROP TABLE t\nSELECT 1; # TRUNCATE t\n/* DELETE FROM t */", NULL);
    expect_destructive("SELECT `drop` FROM `delete`;", NULL);
    expect_destructive("UPDATE t SET a = 1 WHERE id = 3;", NULL);
    expect_destructive("INSERT INTO t VALUES (1);", NULL);
    expect_destructive("ALTER TABLE t ADD COLUMN c INT;", NULL);
    expect_destructive("", NULL);

    expect_destructive("drop table if exists t;", "DROP TABLE");
    expect_destructive("DROP TEMPORARY TABLE t", "DROP TABLE");
    expect_destructive("DROP DATABASE shop;", "DROP DATABASE");
    expect_destructive("TRUNCATE TABLE t;", "TRUNCATE");
    expect_destructive("DELETE FROM t WHERE id = 1;", "DELETE");
    expect_destructive("DELETE FROM t;", "DELETE without WHERE");
    expect_destructive("UPDATE t SET a = 1;", "UPDATE without WHERE");
    expect_destructive("UPDATE t SET a = (SELECT max(b) FROM u WHERE u.id = 1);", "UPDATE without WHERE");
    expect_destructive("ALTER TABLE t DROP COLUMN c;", "ALTER ... DROP");
    expect_destructive("WITH old AS (SELECT id FROM t WHERE x < 1) DELETE FROM t;", "DELETE without WHERE");
    expect_destructive("SELECT 1; /*!50000 DROP TABLE t */;", "DROP TABLE");
    expect_destructive("SELECT 'it''s'; DELETE FROM t; DROP VIEW v; UPDATE t SET a=1;",
                       "DELETE without WHERE, DROP VIEW, UPDATE without WHERE");
    expect_destructive("DELETE FROM a WHERE x=1; DELETE FROM b WHERE y=2;", "DELETE");
    expect_destructive("SELECT 'a\\'; DROP TABLE t; --' AS s;", NULL);
}

int main(void) {
    test_json();
    test_request();
    test_response();
    test_format();
    test_destructive();
    if (failures) { fprintf(stderr, "ai_test: %d failure(s)\n", failures); return 1; }
    printf("ai_test: all passed\n");
    return 0;
}
