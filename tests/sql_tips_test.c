/* Tests for src/sql_tips.c: the list's shape and the no-repeat pick. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sql_tips.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", \
                                          __FILE__, __LINE__, #c); failures++; } } while (0)

int main(void) {
    size_t n = sql_tip_count();
    CHECK(n == 50);
    CHECK(sql_tip(n) == NULL);
    CHECK(sql_tip(SIZE_MAX) == NULL);

    for (size_t i = 0; i < n; i++) {
        const char *t = sql_tip(i);
        CHECK(t != NULL);
        if (!t) continue;
        size_t len = strlen(t);
        if (len == 0 || len > SQL_TIP_MAX_LEN)
            fprintf(stderr, "tip %zu is %zu bytes (max %d)\n", i, len, SQL_TIP_MAX_LEN);
        CHECK(len > 0 && len <= SQL_TIP_MAX_LEN);
        if (len == 0) continue;
        CHECK(t[0] != ' ' && t[len - 1] == '.');
        CHECK(strchr(t, '\n') == NULL);
        for (size_t k = 0; k < i; k++) {
            const char *u = sql_tip(k);
            if (u && strcmp(t, u) == 0) fprintf(stderr, "tips %zu and %zu are the same\n", k, i);
            CHECK(!u || strcmp(t, u) != 0);
        }
    }

    /* nothing shown yet: any tip, in range */
    for (unsigned long r = 0; r < 200; r++) CHECK(sql_tip_pick(SIZE_MAX, r) < n);
    CHECK(sql_tip_pick(SIZE_MAX, 7) == 7);
    CHECK(sql_tip_pick(n, 3) == 3);

    /* after tip `last`: never the same one, and every other one is reachable */
    CHECK(n <= 64); /* seen[] below */
    for (size_t last = 0; last < n && n <= 64; last++) {
        unsigned char seen[64] = { 0 };
        for (unsigned long r = 0; r < n - 1; r++) {
            size_t p = sql_tip_pick(last, r);
            CHECK(p < n && p != last);
            if (p < n) seen[p] = 1;
        }
        size_t reached = 0;
        for (size_t i = 0; i < n; i++) reached += seen[i];
        CHECK(reached == n - 1 && !seen[last]);
    }
    CHECK(sql_tip_pick(0, (unsigned long)-1) < n); /* large r */

    if (failures) { fprintf(stderr, "sql_tips_test: %d failure(s)\n", failures); return 1; }
    printf("sql_tips_test: all checks passed\n");
    return 0;
}
