#include "profiles.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "platform.h"

#define MAX_NAME 128

static char *dup_or_empty(const char *s) { return strdup(s ? s : ""); }

static void profile_clear(profile *p) {
    free(p->name); free(p->host); free(p->user); free(p->database);
    if (p->password) { memset(p->password, 0, strlen(p->password)); free(p->password); }
    memset(p, 0, sizeof *p);
}

static int profile_copy(profile *dst, const profile *src) {
    memset(dst, 0, sizeof *dst);
    dst->name = dup_or_empty(src->name);
    dst->host = dup_or_empty(src->host);
    dst->user = dup_or_empty(src->user);
    dst->database = dup_or_empty(src->database);
    dst->password = dup_or_empty(src->remember_password ? src->password : NULL);
    dst->port = src->port;
    dst->remember_password = src->remember_password;
    dst->hide_views = src->hide_views;
    dst->no_auto_rehash = src->no_auto_rehash;
    dst->tables_once = src->tables_once;
    if (!dst->name || !dst->host || !dst->user || !dst->database || !dst->password) {
        profile_clear(dst);
        return -1;
    }
    return 0;
}

char *config_path(const char *file) {
    const char *dir = getenv("SQLC_CONFIG_DIR");
    char buf[4096], base[4096];
    if (!dir || !*dir) {
        if (plat_config_dir(base, sizeof base) != 0) return NULL;
        dir = base;
    }
    int n = snprintf(buf, sizeof buf, "%s%c%s", dir, PLAT_SEP, file);
    if (n < 0 || (size_t)n >= sizeof buf) { errno = ENAMETOOLONG; return NULL; }
    return strdup(buf);
}

char *profiles_path(void) { return config_path("connections"); }

int profile_name_valid(const char *name) {
    if (!name || !*name) return 0;
    size_t len = strlen(name);
    if (len > MAX_NAME) return 0;
    if (isspace((unsigned char)name[0]) || isspace((unsigned char)name[len - 1])) return 0;
    return strpbrk(name, "\r\n[]") == NULL;
}

profile *profiles_find(profile_list *pl, const char *name) {
    for (size_t i = 0; name && i < pl->n; i++)
        if (strcmp(pl->items[i].name, name) == 0) return &pl->items[i];
    return NULL;
}

int profiles_put(profile_list *pl, const profile *p) {
    if (!profile_name_valid(p->name)) return -1;
    profile copy;
    if (profile_copy(&copy, p) != 0) return -1;

    profile *old = profiles_find(pl, p->name);
    if (old) {
        profile_clear(old);
        *old = copy;
        return (int)(old - pl->items);
    }

    profile *items = realloc(pl->items, (pl->n + 1) * sizeof *items);
    if (!items) { profile_clear(&copy); return -1; }
    pl->items = items;
    size_t at = 0;
    while (at < pl->n && strcasecmp(pl->items[at].name, copy.name) < 0) at++;
    memmove(&pl->items[at + 1], &pl->items[at], (pl->n - at) * sizeof *items);
    pl->items[at] = copy;
    pl->n++;
    return (int)at;
}

int profiles_remove(profile_list *pl, const char *name) {
    profile *p = profiles_find(pl, name);
    if (!p) return -1;
    if (pl->last && strcmp(pl->last, name) == 0) profiles_set_last(pl, NULL);
    size_t i = (size_t)(p - pl->items);
    profile_clear(p);
    memmove(&pl->items[i], &pl->items[i + 1], (pl->n - i - 1) * sizeof *p);
    pl->n--;
    return 0;
}

int profiles_set_last(profile_list *pl, const char *name) {
    char *d = NULL;
    if (name && !(d = strdup(name))) return -1;
    free(pl->last);
    pl->last = d;
    return 0;
}

void profiles_free(profile_list *pl) {
    for (size_t i = 0; i < pl->n; i++) profile_clear(&pl->items[i]);
    free(pl->items);
    free(pl->last);
    memset(pl, 0, sizeof *pl);
}

/* ---------- reading ---------- */

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

static int set_str(char **field, const char *v) {
    char *d = strdup(v);
    if (!d) return -1;
    free(*field);
    *field = d;
    return 0;
}

/* Adds the profile being parsed (if any) to the list. */
static int flush(profile_list *pl, profile *cur, int *have) {
    int rc = 0;
    if (*have && profile_name_valid(cur->name)) rc = profiles_put(pl, cur) < 0 ? -1 : 0;
    profile_clear(cur);
    *have = 0;
    return rc;
}

int profiles_load(const char *path, profile_list *out) {
    memset(out, 0, sizeof *out);
    FILE *f = plat_fopen(path, "rb");
    if (!f) return errno == ENOENT ? 0 : -1;

    profile cur = { 0 };
    int have = 0, rc = 0;
    char *line = NULL;
    size_t cap = 0;
    while (rc == 0 && plat_getline(&line, &cap, f) != -1) {
        line[strcspn(line, "\r\n")] = '\0';
        char *s = line; /* leading space only: a password may end in spaces */
        while (isspace((unsigned char)*s)) s++;
        if (!*s || *s == '#' || *s == ';') continue;

        if (*s == '[') {
            char *end = strrchr(s, ']');
            if (!end) continue;
            *end = '\0';
            rc = flush(out, &cur, &have);
            if (rc == 0 && (rc = set_str(&cur.name, trim(s + 1))) == 0) {
                cur.port = 3306;
                have = 1;
            }
            continue;
        }

        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = trim(s);
        if (strcmp(key, "password") == 0) { /* verbatim, spaces and all */
            if (have) rc = set_str(&cur.password, eq + 1);
            continue;
        }
        const char *val = trim(eq + 1);

        if (!have) {
            if (strcmp(key, "last") == 0) rc = profiles_set_last(out, trim(eq + 1));
        } else if (strcmp(key, "host") == 0) {
            rc = set_str(&cur.host, val);
        } else if (strcmp(key, "user") == 0) {
            rc = set_str(&cur.user, val);
        } else if (strcmp(key, "database") == 0) {
            rc = set_str(&cur.database, val);
        } else if (strcmp(key, "port") == 0) {
            unsigned long p = strtoul(val, NULL, 10);
            cur.port = p > 0 && p <= 65535 ? (unsigned int)p : 3306;
        } else if (strcmp(key, "remember_password") == 0) {
            cur.remember_password = atoi(val) != 0;
        } else if (strcmp(key, "hide_views") == 0) {
            cur.hide_views = atoi(val) != 0;
        } else if (strcmp(key, "no_auto_rehash") == 0) {
            cur.no_auto_rehash = atoi(val) != 0;
        } else if (strcmp(key, "tables_once") == 0) {
            cur.tables_once = atoi(val) != 0;
        }
    }
    if (rc == 0) rc = flush(out, &cur, &have);
    else profile_clear(&cur);
    free(line);
    fclose(f);

    if (rc != 0) { profiles_free(out); errno = ENOMEM; return -1; }
    if (out->last && !profiles_find(out, out->last)) profiles_set_last(out, NULL);
    return 0;
}

/* ---------- writing ---------- */

/* Values are single-line; drop any line breaks rather than corrupt the file. */
static void put_value(FILE *f, const char *key, const char *v) {
    fprintf(f, "%s=", key);
    for (; v && *v; v++)
        if (*v != '\n' && *v != '\r') fputc(*v, f);
    fputc('\n', f);
}

static int mkdir_parents(const char *path) {
    char buf[4096];
    if (strlen(path) >= sizeof buf) { errno = ENAMETOOLONG; return -1; }
    strcpy(buf, path);
    char *slash = NULL;
    for (char *p = buf; *p; p++) if (plat_is_sep(*p)) slash = p;
    if (!slash || slash == buf) return 0;
    *slash = '\0';
    for (char *p = buf + 1; ; p++) {
        if (plat_is_sep(*p) || *p == '\0') {
            char c = *p;
            *p = '\0';
            if (plat_mkdir(buf) != 0) return -1;
            if (!c) break;
            *p = c;
        }
    }
    return 0;
}

/* Write a 0600 file atomically: fill(f, data) writes the contents. */
static int write_private(const char *path, void (*fill)(FILE *, const void *), const void *data) {
    if (mkdir_parents(path) != 0) return -1;

    char tmp[4096];
    if (snprintf(tmp, sizeof tmp, "%s.tmp-XXXXXX", path) >= (int)sizeof tmp) {
        errno = ENAMETOOLONG;
        return -1;
    }
    FILE *f = plat_mkstemp(tmp, 1);
    if (!f) return -1;

    fill(f, data);

    int err = ferror(f) || plat_fsync(f) != 0;
    int saved = errno;
    if (fclose(f) != 0) { err = 1; saved = errno; }
    if (err || plat_replace(tmp, path) != 0) {
        if (!err) saved = errno;
        plat_unlink(tmp);
        errno = saved;
        return -1;
    }
    return 0;
}

static void fill_profiles(FILE *f, const void *data) {
    const profile_list *pl = data;
    fprintf(f, "# sqlc saved connections (remembered passwords included; keep this file private).\n");
    if (pl->last) put_value(f, "last", pl->last);
    for (size_t i = 0; i < pl->n; i++) {
        const profile *p = &pl->items[i];
        fprintf(f, "\n[%s]\n", p->name);
        put_value(f, "host", p->host);
        fprintf(f, "port=%u\n", p->port ? p->port : 3306);
        put_value(f, "user", p->user);
        put_value(f, "database", p->database);
        fprintf(f, "remember_password=%d\n", p->remember_password ? 1 : 0);
        if (p->remember_password && p->password && *p->password)
            put_value(f, "password", p->password);
        fprintf(f, "hide_views=%d\n", p->hide_views ? 1 : 0);
        fprintf(f, "no_auto_rehash=%d\n", p->no_auto_rehash ? 1 : 0);
        fprintf(f, "tables_once=%d\n", p->tables_once ? 1 : 0);
    }
}

int profiles_save(const char *path, const profile_list *pl) {
    return write_private(path, fill_profiles, pl);
}

/* ---------- single secrets ---------- */

static void fill_secret(FILE *f, const void *data) {
    for (const char *v = data; v && *v; v++)
        if (*v != '\n' && *v != '\r') fputc(*v, f);
    fputc('\n', f);
}

int secret_write(const char *path, const char *value) {
    return write_private(path, fill_secret, value);
}

char *secret_read(const char *path) {
    FILE *f = plat_fopen(path, "rb");
    if (!f) return NULL;
    char *line = NULL;
    size_t cap = 0;
    long n = plat_getline(&line, &cap, f);
    fclose(f);
    if (n <= 0) { free(line); return NULL; }
    line[strcspn(line, "\r\n")] = '\0';
    if (!*line) { free(line); return NULL; }
    return line;
}

int secret_remove(const char *path) {
    return plat_unlink(path) == 0 || errno == ENOENT ? 0 : -1;
}
