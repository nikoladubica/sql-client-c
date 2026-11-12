/* Tests for src/sidebar.c: the sidebar search field's name match. */
#include <stdio.h>

#include "sidebar.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", \
                                          __FILE__, __LINE__, #c); failures++; } } while (0)

static void test_empty_query(void) {
    CHECK(sidebar_name_matches("orders", NULL) == 1);
    CHECK(sidebar_name_matches("orders", "") == 1);
    CHECK(sidebar_name_matches("orders", "   ") == 1);
    CHECK(sidebar_name_matches("", "") == 1);
}

static void test_null_name(void) {
    CHECK(sidebar_name_matches(NULL, NULL) == 0);
    CHECK(sidebar_name_matches(NULL, "") == 0);
    CHECK(sidebar_name_matches(NULL, "a") == 0);
}

static void test_substring(void) {
    CHECK(sidebar_name_matches("orders", "ord") == 1);    /* start */
    CHECK(sidebar_name_matches("orders", "der") == 1);    /* middle */
    CHECK(sidebar_name_matches("orders", "ers") == 1);    /* end */
    CHECK(sidebar_name_matches("orders", "orders") == 1); /* whole */
    CHECK(sidebar_name_matches("orders", "ordersx") == 0);
    CHECK(sidebar_name_matches("orders", "xyz") == 0);
    CHECK(sidebar_name_matches("", "a") == 0);
    CHECK(sidebar_name_matches("order_items", "_it") == 1); /* no wildcards */
    CHECK(sidebar_name_matches("orders", "o%s") == 0);
    CHECK(sidebar_name_matches("aab", "ab") == 1);          /* retry after a partial match */
}

static void test_case(void) {
    CHECK(sidebar_name_matches("Orders", "ord") == 1);
    CHECK(sidebar_name_matches("orders", "ORD") == 1);
    CHECK(sidebar_name_matches("CustomerOrders", "rOrD") == 1);
}

static void test_spaces(void) {
    CHECK(sidebar_name_matches("orders", "  ord  ") == 1);
    CHECK(sidebar_name_matches("order items", "r i") == 1); /* inner space kept */
    CHECK(sidebar_name_matches("orderitems", "r i") == 0);
    CHECK(sidebar_name_matches("orders", " x ") == 0);
}

static void test_utf8(void) {
    /* bytes outside ASCII compared as they are: no case folding there */
    CHECK(sidebar_name_matches("caf\xC3\xA9_menu", "\xC3\xA9_m") == 1);
    CHECK(sidebar_name_matches("caf\xC3\xA9", "CAF\xC3\xA9") == 1);
    CHECK(sidebar_name_matches("caf\xC3\xA9", "caf\xC3\x89") == 0); /* É vs é */
}

int main(void) {
    test_empty_query();
    test_null_name();
    test_substring();
    test_case();
    test_spaces();
    test_utf8();
    if (failures) {
        fprintf(stderr, "sidebar_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("sidebar_test: all checks passed\n");
    return 0;
}
