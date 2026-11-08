/* Round-trip tests for src/profiles.c. Usage: profiles_test <scratch-dir> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "profiles.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", \
                                          __FILE__, __LINE__, #c); failures++; } } while (0)

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s DIR\n", argv[0]); return 2; }
    char path[1024];
    snprintf(path, sizeof path, "%s/nested/connections", argv[1]);
    remove(path);

    /* missing file -> empty list */
    profile_list pl;
    CHECK(profiles_load(path, &pl) == 0);
    CHECK(pl.n == 0 && pl.last == NULL);

    /* names */
    CHECK(profile_name_valid("prod"));
    CHECK(profile_name_valid("my db (local)"));
    CHECK(!profile_name_valid(""));
    CHECK(!profile_name_valid(" lead"));
    CHECK(!profile_name_valid("trail "));
    CHECK(!profile_name_valid("a]b"));
    CHECK(!profile_name_valid("a\nb"));

    profile a = { .name = "zeta", .host = "db.example.com", .port = 3307, .user = "app",
                  .database = "shop", .remember_password = 1, .hide_views = 1,
                  .no_auto_rehash = 1 };
    profile b = { .name = "Alpha", .host = "localhost", .port = 3306, .user = "root",
                  .database = "", .remember_password = 0 };
    profile c = { .name = "bad\nname", .host = "x", .user = "x", .database = "x" };
    CHECK(profiles_put(&pl, &a) >= 0);
    CHECK(profiles_put(&pl, &b) == 0); /* sorted case-insensitively: Alpha first */
    CHECK(profiles_put(&pl, &c) == -1);
    CHECK(pl.n == 2);

    /* replace keeps one entry */
    a.user = "app2";
    CHECK(profiles_put(&pl, &a) == 1);
    CHECK(pl.n == 2);
    CHECK(profiles_set_last(&pl, "zeta") == 0);

    CHECK(profiles_save(path, &pl) == 0);
    struct stat st;
    CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);

    profile_list back;
    CHECK(profiles_load(path, &back) == 0);
    CHECK(back.n == 2);
    CHECK(back.last && strcmp(back.last, "zeta") == 0);
    profile *z = profiles_find(&back, "zeta");
    CHECK(z && strcmp(z->host, "db.example.com") == 0 && z->port == 3307 &&
          strcmp(z->user, "app2") == 0 && strcmp(z->database, "shop") == 0 &&
          z->remember_password == 1 && z->hide_views == 1 &&
          z->no_auto_rehash == 1);
    profile *al = profiles_find(&back, "Alpha");
    CHECK(al && strcmp(al->database, "") == 0 && al->remember_password == 0 &&
          al->no_auto_rehash == 0);
    CHECK(strcmp(back.items[0].name, "Alpha") == 0);

    /* removing the last-used profile clears last */
    CHECK(profiles_remove(&back, "zeta") == 0);
    CHECK(profiles_remove(&back, "zeta") == -1);
    CHECK(back.last == NULL && back.n == 1);
    profiles_free(&back);
    profiles_free(&pl);

    /* hand-written file: comments, CRLF, unknown keys, dangling last */
    FILE *f = fopen(path, "w");
    fputs("# hi\r\nlast=gone\r\n\r\n[ spaced ]\r\nhost=h\r\nport=abc\r\nfoo=bar\r\n"
          "user=u=with=equals\r\n", f);
    fclose(f);
    CHECK(profiles_load(path, &back) == 0);
    CHECK(back.n == 1 && back.last == NULL);
    profile *sp = profiles_find(&back, "spaced");
    CHECK(sp && strcmp(sp->host, "h") == 0 && sp->port == 3306 &&
          strcmp(sp->user, "u=with=equals") == 0 && strcmp(sp->database, "") == 0);
    profiles_free(&back);

    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("profiles_test: all checks passed\n");
    return 0;
}
