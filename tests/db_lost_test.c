/* Tests for db_errno_lost (src/db.h): which errors mean the connection is
 * gone, so the GUI offers Reconnect. Header only: no server, no network. */
#include <stdio.h>

#include "db.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", \
                                          __FILE__, __LINE__, #c); failures++; } } while (0)

int main(void) {
    /* the connection is gone */
    CHECK(db_errno_lost(2006)); /* CR_SERVER_GONE_ERROR: Server has gone away */
    CHECK(db_errno_lost(2013)); /* CR_SERVER_LOST: Lost connection during query */
    CHECK(db_errno_lost(1927)); /* ER_CONNECTION_KILLED (MariaDB) */
    CHECK(db_errno_lost(4031)); /* ER_CLIENT_INTERACTION_TIMEOUT (MySQL 8) */
    /* the connection is fine, the statement failed */
    CHECK(!db_errno_lost(0));
    CHECK(!db_errno_lost(1064)); /* syntax error */
    CHECK(!db_errno_lost(1146)); /* table doesn't exist */
    CHECK(!db_errno_lost(1062)); /* duplicate key */
    CHECK(!db_errno_lost(1205)); /* lock wait timeout: the connection stays */
    CHECK(!db_errno_lost(2003)); /* can't connect: no connection to lose */

    if (failures) { fprintf(stderr, "db_lost_test: %d failure(s)\n", failures); return 1; }
    printf("db_lost_test: all checks passed\n");
    return 0;
}
