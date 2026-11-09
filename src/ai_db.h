#ifndef SQLC_AI_DB_H
#define SQLC_AI_DB_H

#include <stddef.h>
#include <mysql.h>

/*
 * The current database's schema as compact text for the AI prompt: each
 * table/view with its columns (type, NULL-ability, key, extra), then the
 * foreign keys. Names and types only, never row data.
 *
 * Stops adding tables once the text reaches about max_bytes and says how many
 * were left out. Returns malloc'd text ("" when no database is selected or it
 * has no tables), or NULL on a query error.
 */
char *ai_schema(MYSQL *conn, size_t max_bytes);

#endif
