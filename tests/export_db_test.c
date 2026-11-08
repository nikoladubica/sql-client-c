/* Driver for tests/export_roundtrip.sh: export one table of a database in
 * every format, using the same code as the GUI.
 *
 *   export_db_test <database> <table> <outdir>
 *
 * Connects as the current user over the default socket. The session time
 * zone is set to +05:00 first, to check that SQL exports don't depend on it
 * (and that it's restored afterwards). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mysql.h>

#include "export_db.h"

static int run(MYSQL *conn, const char *table, export_format fmt, export_opts o,
               const char *outdir, const char *suffix) {
    char path[4096];
    snprintf(path, sizeof path, "%s/%s%s.%s", outdir, "out", suffix, export_extension(fmt));
    unsigned long long rows = 0;
    char *err = NULL;
    int rc = export_table(conn, table, 0, fmt, &o, path, NULL, NULL, &rows, &err);
    if (rc != 0) fprintf(stderr, "export %s failed: %s\n", path, err);
    else printf("%s: %llu rows\n", path, rows);
    free(err);
    return rc;
}

int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: %s <database> <table> <outdir>\n", argv[0]); return 2; }
    MYSQL *conn = mysql_init(NULL);
    mysql_options(conn, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    if (!mysql_real_connect(conn, NULL, getenv("USER"), NULL, argv[1], 0, NULL, 0)) {
        fprintf(stderr, "connect: %s\n", mysql_error(conn));
        return 1;
    }
    mysql_query(conn, "SET time_zone = '+05:00'");

    export_opts o = export_default_opts();
    int rc = 0;
    rc |= run(conn, argv[2], EXPORT_SQL, o, argv[3], "");
    rc |= run(conn, argv[2], EXPORT_CSV, o, argv[3], "");
    rc |= run(conn, argv[2], EXPORT_XML, o, argv[3], "");
    o.csv_null_as_n = 1;
    o.csv_bom = 0;
    rc |= run(conn, argv[2], EXPORT_CSV, o, argv[3], "-nullN");

    /* a missing table is an error, and leaves no file behind */
    char *err = NULL;
    char missing[4096];
    snprintf(missing, sizeof missing, "%s/missing.sql", argv[3]);
    if (export_table(conn, "no_such_table", 0, EXPORT_SQL, &o, missing, NULL, NULL, NULL, &err) == 0) {
        fprintf(stderr, "exporting a missing table succeeded\n");
        rc = 1;
    } else {
        printf("missing table: %s\n", err);
    }
    free(err);

    /* an unwritable destination is an error */
    err = NULL;
    if (export_table(conn, argv[2], 0, EXPORT_CSV, &o, "/no/such/dir/out.csv", NULL, NULL, NULL, &err) == 0) {
        fprintf(stderr, "exporting to a missing directory succeeded\n");
        rc = 1;
    } else {
        printf("bad path: %s\n", err);
    }
    free(err);

    /* the session time zone is back to what it was */
    if (mysql_query(conn, "SELECT @@session.time_zone") == 0) {
        MYSQL_RES *r = mysql_store_result(conn);
        MYSQL_ROW row = r ? mysql_fetch_row(r) : NULL;
        printf("time zone after export: %s\n", row && row[0] ? row[0] : "?");
        if (!row || !row[0] || strcmp(row[0], "+05:00") != 0) rc = 1;
        if (r) mysql_free_result(r);
    }
    mysql_close(conn);
    return rc ? 1 : 0;
}
