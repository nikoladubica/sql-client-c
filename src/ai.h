#ifndef SQLC_AI_H
#define SQLC_AI_H

/*
 * AI query assistant: describe what you need, Claude writes the SQL.
 *
 * The request carries the task, the server version and the current
 * database's schema (table/column names and types, foreign keys; never row
 * data). Claude answers with structured output {explanation, sql}, which is
 * turned into SQL with the explanation as `--` comments above it. Nothing is
 * run automatically.
 *
 * No UI or database code here (see ai_db.h for the schema), so the request
 * and response handling can be unit tested.
 */

#include <stddef.h>

#define AI_MODEL "claude-opus-5-5"
#define AI_API_URL "https://api.anthropic.com/v1/messages"

typedef struct {
    char *explanation;   /* may be empty */
    char *sql;           /* may be empty if Claude couldn't write a query */
    char *model;         /* model that answered (differs after a fallback) */
    char *error;         /* user-facing message on failure, else NULL */
} ai_result;

void ai_result_free(ai_result *r);

/* JSON body for POST /v1/messages (malloc'd). server is the server version
 * string (e.g. "10.11.6-MariaDB"), database the current database or NULL,
 * schema the text from ai_schema() or NULL. */
char *ai_build_request(const char *model, const char *server, const char *database,
                       const char *schema, const char *task);

/* Reads an HTTP response from the Messages API. Returns 0 and fills
 * explanation/sql, or -1 with out->error set. */
int ai_parse_response(long http_status, const char *body, ai_result *out);

/* Sends the request (blocking; call from a worker thread) and parses the
 * reply. Returns 0 or -1 with out->error set. Call curl_global_init() once
 * before starting threads. */
int ai_generate(const char *api_key, const char *request_json, ai_result *out);

/* The editor text: the request and explanation as `--` comments, then the
 * SQL (malloc'd). */
char *ai_format(const char *task, const ai_result *r);

/* If sql contains statements that destroy data or structure (DROP,
 * TRUNCATE, DELETE, UPDATE without WHERE, ALTER ... DROP), a malloc'd,
 * comma-separated list of them, e.g. "DROP TABLE, UPDATE without WHERE";
 * NULL if none. Comments and quoted strings are ignored. */
char *ai_destructive(const char *sql);

#endif
