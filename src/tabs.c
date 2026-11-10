/* Tab titles and the running-jobs list for the GUI's tabs, see tabs.h. */
#include "tabs.h"

#include <stdio.h>
#include <string.h>

#define ELLIPSIS "\xE2\x80\xA6" /* "…", 3 bytes */
#define PART_MAX 80             /* bytes of a tab title or job name in a line */

/* Bytes of s that fit in max without cutting a UTF-8 character. */
static size_t utf8_cut(const char *s, size_t max) {
    size_t len = strlen(s);
    if (len <= max) return len;
    len = max;
    while (len > 0 && ((unsigned char)s[len] & 0xC0) == 0x80) len--;
    return len;
}

char *tab_title(char *buf, size_t size, const char *profile, const char *user,
                const char *host, const char *db) {
    if (size == 0) return buf;
    char full[512];
    int has_user = user && *user, has_db = db && *db;
    if (profile && *profile)
        snprintf(full, sizeof full, "%s", profile);
    else if (host && *host)
        snprintf(full, sizeof full, "%s%s%s%s%s", has_user ? user : "", has_user ? "@" : "",
                 host, has_db ? " / " : "", has_db ? db : "");
    else
        snprintf(full, sizeof full, "New connection");

    size_t limit = size - 1 < TAB_TITLE_MAX ? size - 1 : TAB_TITLE_MAX;
    size_t len = strlen(full);
    if (len <= limit) {
        memcpy(buf, full, len + 1);
    } else if (limit >= sizeof ELLIPSIS - 1) {
        size_t cut = utf8_cut(full, limit - (sizeof ELLIPSIS - 1));
        memcpy(buf, full, cut);
        memcpy(buf + cut, ELLIPSIS, sizeof ELLIPSIS); /* with its NUL */
    } else {
        size_t cut = utf8_cut(full, limit);
        memcpy(buf, full, cut);
        buf[cut] = '\0';
    }
    return buf;
}

size_t running_jobs_text(char *buf, size_t size, const tab_job_info *items, size_t n) {
    if (size == 0) return 0;
    buf[0] = '\0';
    size_t len = 0, listed = 0;
    for (size_t i = 0; i < n; i++) {
        const char *tb = items[i].tab ? items[i].tab : "";
        const char *jb = items[i].job ? items[i].job : "";
        char line[2 * PART_MAX + 8];
        int ll = snprintf(line, sizeof line, "%.*s - %.*s", (int)utf8_cut(tb, PART_MAX), tb,
                          (int)utf8_cut(jb, PART_MAX), jb);
        if (ll < 0) break;
        /* room for this line, and for "...and N more" after it if more follow */
        size_t need = (len ? 1 : 0) + (size_t)ll;
        size_t reserve = 0;
        if (i + 1 < n) {
            char more[64];
            int ml = snprintf(more, sizeof more, "...and %zu more", n - i - 1);
            reserve = 1 + (ml > 0 ? (size_t)ml : 0);
        }
        if (len + need + reserve >= size) break;
        if (len) buf[len++] = '\n';
        memcpy(buf + len, line, (size_t)ll + 1);
        len += (size_t)ll;
        listed++;
    }
    if (listed < n)
        snprintf(buf + len, size - len, "%s...and %zu more", len ? "\n" : "", n - listed);
    return listed;
}
