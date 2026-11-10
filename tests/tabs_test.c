/* Tests for src/tabs.c: tab titles and the running-jobs list. */
#include <stdio.h>
#include <string.h>

#include "tabs.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", \
                                          __FILE__, __LINE__, #c); failures++; } } while (0)
#define EXPECT_STR(got, want) do { \
    const char *g_ = (got), *w_ = (want); \
    if (!g_ || strcmp(g_, w_) != 0) { \
        fprintf(stderr, "%s:%d: got [%s], want [%s]\n", __FILE__, __LINE__, g_ ? g_ : "(null)", w_); \
        failures++; \
    } } while (0)

/* 1 if s is valid UTF-8 (no character cut short). */
static int utf8_ok(const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        int k = *p < 0x80 ? 0 : (*p & 0xE0) == 0xC0 ? 1 : (*p & 0xF0) == 0xE0 ? 2
              : (*p & 0xF8) == 0xF0 ? 3 : -1;
        if (k < 0) return 0;
        p++;
        for (; k > 0; k--, p++) if ((*p & 0xC0) != 0x80) return 0;
    }
    return 1;
}

static void test_titles(void) {
    char buf[128];
    EXPECT_STR(tab_title(buf, sizeof buf, "staging", "root", "db1", "shop"), "staging");
    EXPECT_STR(tab_title(buf, sizeof buf, "", "root", "localhost", "shop"), "root@localhost / shop");
    EXPECT_STR(tab_title(buf, sizeof buf, NULL, "root", "localhost", NULL), "root@localhost");
    EXPECT_STR(tab_title(buf, sizeof buf, NULL, "root", "localhost", ""), "root@localhost");
    EXPECT_STR(tab_title(buf, sizeof buf, NULL, "", "10.0.0.5", "shop"), "10.0.0.5 / shop");
    EXPECT_STR(tab_title(buf, sizeof buf, NULL, NULL, "h", NULL), "h");
    EXPECT_STR(tab_title(buf, sizeof buf, NULL, "root", NULL, "shop"), "New connection");
    EXPECT_STR(tab_title(buf, sizeof buf, NULL, NULL, "", NULL), "New connection");
    EXPECT_STR(tab_title(buf, sizeof buf, NULL, "o'brien", "host`x", "d\"b"), "o'brien@host`x / d\"b");

    /* long: cut to TAB_TITLE_MAX bytes with an ellipsis */
    char lng[300];
    memset(lng, 'a', sizeof lng - 1);
    lng[sizeof lng - 1] = '\0';
    tab_title(buf, sizeof buf, lng, NULL, NULL, NULL);
    CHECK(strlen(buf) == TAB_TITLE_MAX);
    CHECK(strcmp(buf + TAB_TITLE_MAX - 3, "\xE2\x80\xA6") == 0);
    CHECK(strncmp(buf, lng, TAB_TITLE_MAX - 3) == 0);

    /* non-ASCII: never cut inside a character */
    char uni[300] = "";
    for (int i = 0; i < 40; i++) strcat(uni, "\xC3\xA9"); /* é, 2 bytes */
    tab_title(buf, sizeof buf, uni, NULL, NULL, NULL);
    CHECK(strlen(buf) <= TAB_TITLE_MAX);
    CHECK(utf8_ok(buf));
    CHECK(strstr(buf, "\xE2\x80\xA6") != NULL);
    char uni3[300] = "";
    for (int i = 0; i < 40; i++) strcat(uni3, "\xE6\x97\xA5"); /* 日, 3 bytes */
    tab_title(buf, sizeof buf, uni3, NULL, NULL, NULL);
    CHECK(strlen(buf) <= TAB_TITLE_MAX);
    CHECK(utf8_ok(buf));
    EXPECT_STR(tab_title(buf, sizeof buf, "\xC3\xA9t\xC3\xA9", NULL, NULL, NULL), "\xC3\xA9t\xC3\xA9");

    /* small buffers */
    char small[8];
    tab_title(small, sizeof small, "abcdefghijkl", NULL, NULL, NULL);
    CHECK(strlen(small) == 7);
    CHECK(utf8_ok(small));
    CHECK(strncmp(small, "abcd", 4) == 0);
    char tiny[3];
    tab_title(tiny, sizeof tiny, "abcdef", NULL, NULL, NULL);
    EXPECT_STR(tiny, "ab");
    char one[1];
    tab_title(one, sizeof one, "abc", NULL, NULL, NULL);
    CHECK(one[0] == '\0');
    char exact[5];
    EXPECT_STR(tab_title(exact, sizeof exact, "abcd", NULL, NULL, NULL), "abcd");
}

static void test_jobs(void) {
    char buf[512];
    tab_job_info two[] = {
        { "root@localhost / shop", "export of shop" },
        { "staging", "query" },
    };
    CHECK(running_jobs_text(buf, sizeof buf, two, 2) == 2);
    EXPECT_STR(buf, "root@localhost / shop - export of shop\nstaging - query");

    CHECK(running_jobs_text(buf, sizeof buf, two, 0) == 0);
    EXPECT_STR(buf, "");

    tab_job_info nulls[] = { { NULL, NULL } };
    CHECK(running_jobs_text(buf, sizeof buf, nulls, 1) == 1);
    EXPECT_STR(buf, " - ");

    /* doesn't fit: "...and N more" */
    tab_job_info many[20];
    for (int i = 0; i < 20; i++) { many[i].tab = "a tab"; many[i].job = "query"; }
    char small[44];
    size_t listed = running_jobs_text(small, sizeof small, many, 20);
    CHECK(listed == 2);
    EXPECT_STR(small, "a tab - query\na tab - query\n...and 18 more");
    CHECK(strlen(small) < sizeof small);

    /* every size from 1 up: NUL-terminated, never overflowing, count right */
    for (size_t sz = 1; sz < 120; sz++) {
        char b[120];
        memset(b, 'x', sizeof b);
        size_t k = running_jobs_text(b, sz, many, 20);
        CHECK(memchr(b, '\0', sz) != NULL);
        CHECK(k <= 20);
        if (k < 20 && sz > 20) {
            char want[32];
            snprintf(want, sizeof want, "...and %zu more", (size_t)20 - k);
            CHECK(strstr(b, want) != NULL);
        }
    }

    /* long and non-ASCII parts are cut to 80 bytes on a character boundary */
    char lng[400] = "";
    for (int i = 0; i < 100; i++) strcat(lng, "\xC3\xA9");
    tab_job_info one[] = { { lng, lng } };
    CHECK(running_jobs_text(buf, sizeof buf, one, 1) == 1);
    CHECK(utf8_ok(buf));
    CHECK(strlen(buf) == 80 + 3 + 80);

    /* the first line alone doesn't fit */
    char tiny[16];
    CHECK(running_jobs_text(tiny, sizeof tiny, one, 1) == 0);
    EXPECT_STR(tiny, "...and 1 more");
}

int main(void) {
    test_titles();
    test_jobs();
    if (failures) {
        fprintf(stderr, "tabs_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("tabs_test: all checks passed\n");
    return 0;
}
