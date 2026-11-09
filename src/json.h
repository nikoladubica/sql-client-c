#ifndef SQLC_JSON_H
#define SQLC_JSON_H

/*
 * Just enough JSON for talking to the Claude API: a growable string buffer
 * for building request bodies, and a small parser for reading responses.
 */

#include <stddef.h>

/* ---------- building ---------- */

typedef struct {
    char *s;      /* always NUL-terminated once anything was added */
    size_t len, cap;
    int oom;      /* an allocation failed; s holds what fit */
} strbuf;

void sb_add(strbuf *b, const char *s);
void sb_addn(strbuf *b, const char *s, size_t n);
void sb_addc(strbuf *b, char c);
/* Appends s as a quoted, escaped JSON string. */
void sb_add_json_string(strbuf *b, const char *s);
/* Hands over the buffer (malloc'd, never NULL unless out of memory). */
char *sb_take(strbuf *b);
void sb_free(strbuf *b);

/* ---------- parsing ---------- */

typedef enum { JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT } json_type;

typedef struct json_value {
    json_type type;
    int boolean;
    double number;
    char *str;                 /* JSON_STRING, decoded UTF-8 */
    size_t n;                  /* items in an array or object */
    struct json_value *items;  /* array elements / object values */
    char **keys;               /* object keys (same order as items) */
} json_value;

/* Parses a complete JSON document. NULL if it's not valid JSON. */
json_value *json_parse(const char *text);
void json_free(json_value *v);

/* Object member by key, or NULL (also when v isn't an object). */
const json_value *json_get(const json_value *v, const char *key);
/* The member's string, or NULL if missing or not a string. */
const char *json_get_str(const json_value *v, const char *key);

#endif
