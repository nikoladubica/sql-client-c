#ifndef SQLC_COMPLETE_H
#define SQLC_COMPLETE_H

#include <mysql.h>

/*
 * Tab completion of SQL keywords, database names, and the table and
 * column names of the current database. Views are deliberately skipped.
 */

/* Install the completion hook. If enabled is 0, Tab completes nothing (-A). */
void completion_init(int enabled);

/* (Re)load database, table and column names from the server. */
void completion_rehash(MYSQL *conn);

void completion_free(void);

#endif
