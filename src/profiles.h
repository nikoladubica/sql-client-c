#ifndef SQLC_PROFILES_H
#define SQLC_PROFILES_H

#include <stddef.h>

/*
 * Saved connection profiles for sqlc-gui.
 *
 * Stored as a small INI-style text file (mode 0600):
 *
 *   last=prod
 *
 *   [prod]
 *   host=db.example.com
 *   port=3306
 *   user=app
 *   database=shop
 *   remember_password=1
 *   password=secret
 *   hide_views=0
 *   no_auto_rehash=0
 *   tables_once=0
 *
 * The password is stored (in plain text, which is why the file is 0600)
 * only for profiles with remember_password set.
 */

typedef struct {
    char *name;
    char *host;
    unsigned int port;
    char *user;
    char *database;
    int remember_password; /* keep the password in the file */
    char *password;        /* "" if not remembered */
    int hide_views;
    int no_auto_rehash;    /* like the CLI's -A: don't load the table list */
    int tables_once;       /* tables only (no views), loaded on connect, then -A */
} profile;

typedef struct {
    profile *items; /* sorted by name, case-insensitively */
    size_t n;
    char *last;     /* name of the last-used profile, or NULL */
} profile_list;

/* $SQLC_CONFIG_DIR/connections, or
 * ~/Library/Application Support/sqlc/connections. malloc'd; NULL on error. */
char *profiles_path(void);

/* $SQLC_CONFIG_DIR/<file>, or ~/Library/Application Support/sqlc/<file>.
 * malloc'd; NULL on error. */
char *config_path(const char *file);

/* A small secret (e.g. the Claude API key) in its own 0600 file, written
 * atomically. secret_read: malloc'd text without its trailing line break,
 * NULL if the file is missing or empty. secret_remove: a missing file is fine. */
int secret_write(const char *path, const char *value);
char *secret_read(const char *path);
int secret_remove(const char *path);

/* A missing file gives an empty list. Returns 0 on success, -1 (errno set). */
int profiles_load(const char *path, profile_list *out);

/* Writes atomically, creating the directory if needed. 0 or -1 (errno set). */
int profiles_save(const char *path, const profile_list *pl);

/* Non-empty, at most 128 bytes, no line breaks, '[' or ']',
 * no leading/trailing whitespace. */
int profile_name_valid(const char *name);

profile *profiles_find(profile_list *pl, const char *name);

/* Copies p into the list, replacing a profile with the same name.
 * Returns its index, or -1 on allocation failure / invalid name. */
int profiles_put(profile_list *pl, const profile *p);

/* Returns 0 if removed, -1 if not found. Clears last if it pointed at it. */
int profiles_remove(profile_list *pl, const char *name);

/* NULL clears it. Returns 0 or -1 on allocation failure. */
int profiles_set_last(profile_list *pl, const char *name);

void profiles_free(profile_list *pl);

#endif
