/* The sidebar search field's name match, see sidebar.h. */
#include "sidebar.h"

#include <stddef.h>
#include <string.h>

static unsigned char ascii_lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c - 'A' + 'a') : c;
}

int sidebar_name_matches(const char *name, const char *query) {
    if (!query) return name != NULL;
    while (*query == ' ') query++;
    size_t qlen = strlen(query);
    while (qlen > 0 && query[qlen - 1] == ' ') qlen--;
    if (!name) return 0;
    if (qlen == 0) return 1;

    size_t nlen = strlen(name);
    for (size_t i = 0; i + qlen <= nlen; i++) {
        size_t k = 0;
        while (k < qlen && ascii_lower((unsigned char)name[i + k]) ==
                               ascii_lower((unsigned char)query[k]))
            k++;
        if (k == qlen) return 1;
    }
    return 0;
}
