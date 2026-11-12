#ifndef SQLC_SIDEBAR_H
#define SQLC_SIDEBAR_H

/*
 * The GUI sidebar's search field: which table names it lets through. Pure
 * string matching: no UI, no database, no allocation.
 */

/*
 * 1 when query, with leading and trailing spaces left out, is empty (or
 * query is NULL), or appears anywhere in name ignoring ASCII case ("ord"
 * matches "Orders"); else 0. Bytes outside ASCII are compared as they are.
 * A NULL name never matches.
 */
int sidebar_name_matches(const char *name, const char *query);

#endif
