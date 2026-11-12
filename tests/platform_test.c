/* Unit tests for platform.c: the file calls that differ between macOS and
 * Windows. Works in a scratch folder given as argv[1]; no database, no
 * network. Runs on both (make test, make win-test). */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

static void join(char *buf, size_t size, const char *dir, const char *name) {
    snprintf(buf, size, "%s%c%s", dir, PLAT_SEP, name);
}

static int write_file(const char *path, const char *text) {
    FILE *f = plat_fopen(path, "wb");
    if (!f) return -1;
    fputs(text, f);
    return fclose(f);
}

/* The whole file (malloc'd), or NULL. */
static char *read_file(const char *path) {
    FILE *f = plat_fopen(path, "rb");
    if (!f) return NULL;
    char *buf = calloc(1, 4096);
    if (buf) fread(buf, 1, 4095, f);
    fclose(f);
    return buf;
}

static void test_replace(const char *dir) {
    char a[1024], b[1024];
    join(a, sizeof a, dir, "replace-from.txt");
    join(b, sizeof b, dir, "replace-to.txt");
    CHECK(write_file(b, "old") == 0);
    CHECK(write_file(a, "new") == 0);
    CHECK(plat_replace(a, b) == 0); /* over an existing file: rename() refuses on Windows */
    char *got = read_file(b);
    CHECK(got && strcmp(got, "new") == 0);
    free(got);
    CHECK(plat_fopen(a, "rb") == NULL && errno == ENOENT);
    CHECK(plat_unlink(b) == 0);
    CHECK(plat_unlink(b) != 0 && errno == ENOENT);
}

static void test_mkstemp(const char *dir) {
    char t1[1024], t2[1024];
    join(t1, sizeof t1, dir, "tmp-XXXXXX");
    join(t2, sizeof t2, dir, "tmp-XXXXXX");
    FILE *f1 = plat_mkstemp(t1, 1), *f2 = plat_mkstemp(t2, 0);
    CHECK(f1 && f2);
    CHECK(strcmp(t1, t2) != 0);
    CHECK(strstr(t1, "XXXXXX") == NULL);
    if (f1) {
        fputs("line one\nline two\r\n", f1); /* binary: no \n -> \r\n */
        CHECK(plat_fsync(f1) == 0);
        fclose(f1);
        char *got = read_file(t1);
        CHECK(got && strcmp(got, "line one\nline two\r\n") == 0);
        free(got);
    }
    if (f2) fclose(f2);
    plat_unlink(t1);
    plat_unlink(t2);
    char bad[] = "no-placeholder";
#ifdef _WIN32
    CHECK(plat_mkstemp(bad, 0) == NULL);
#else
    (void)bad; /* mkstemp's own check */
#endif
}

static void test_getline(const char *dir) {
    char p[1024];
    join(p, sizeof p, dir, "lines.txt");
    char longline[600];
    memset(longline, 'x', sizeof longline - 1);
    longline[sizeof longline - 1] = '\0';
    char text[800];
    snprintf(text, sizeof text, "first\n%s\nlast", longline);
    CHECK(write_file(p, text) == 0);
    FILE *f = plat_fopen(p, "rb");
    CHECK(f != NULL);
    if (!f) return;
    char *line = NULL;
    size_t cap = 0;
    CHECK(plat_getline(&line, &cap, f) == 6 && strcmp(line, "first\n") == 0);
    CHECK(plat_getline(&line, &cap, f) == (long)sizeof longline && line[0] == 'x' && line[598] == 'x' && line[599] == '\n');
    CHECK(plat_getline(&line, &cap, f) == 4 && strcmp(line, "last") == 0); /* no newline at the end */
    CHECK(plat_getline(&line, &cap, f) == -1);
    free(line);
    fclose(f);
    plat_unlink(p);
}

static void test_utf8_names(const char *dir) {
    char p[1024];
    join(p, sizeof p, dir, "\xC5\xA1ifra-\xE6\x97\xA5\xE6\x9C\xAC.txt"); /* "šifra-日本.txt" */
    CHECK(write_file(p, "ok") == 0);
    char *got = read_file(p);
    CHECK(got && strcmp(got, "ok") == 0);
    free(got);
    CHECK(plat_unlink(p) == 0);

    char sub[1024];
    join(sub, sizeof sub, dir, "fold\xC3\xA9r");
    CHECK(plat_mkdir(sub) == 0);
    CHECK(plat_mkdir(sub) == 0); /* exists already: fine */
}

static void test_config_dir(void) {
    char buf[1024];
    CHECK(plat_config_dir(buf, sizeof buf) == 0);
    size_t n = strlen(buf);
    CHECK(n > 5 && strcmp(buf + n - 4, "sqlc") == 0 && plat_is_sep(buf[n - 5]));
    char tiny[4];
    CHECK(plat_config_dir(tiny, sizeof tiny) != 0 && errno == ENAMETOOLONG);
    CHECK(plat_home() != NULL);
    CHECK(plat_is_sep('/'));
#ifdef _WIN32
    CHECK(plat_is_sep('\\'));
#else
    CHECK(!plat_is_sep('\\'));
#endif
}

static void test_random(void) {
    unsigned long a = plat_random(), b = plat_random(), c = plat_random();
    CHECK(!(a == b && b == c));
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: platform_test <scratch folder>\n");
        return 2;
    }
    const char *dir = argv[1];
    CHECK(plat_mkdir(dir) == 0);
    test_replace(dir);
    test_mkstemp(dir);
    test_getline(dir);
    test_utf8_names(dir);
    test_config_dir();
    test_random();
    if (failures) {
        fprintf(stderr, "platform_test: %d check(s) failed\n", failures);
        return 1;
    }
    printf("platform_test: all checks passed\n");
    return 0;
}
