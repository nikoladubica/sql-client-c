/* File-system calls that differ between macOS and Windows (see platform.h). */

#ifdef _WIN32
#define _CRT_RAND_S /* rand_s() */
#endif

#include "platform.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

#include <windows.h>
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>

/* UTF-8 -> newly allocated UTF-16, or NULL (errno EINVAL / ENOMEM). */
static wchar_t *widen(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    if (n <= 0) { errno = EINVAL; return NULL; }
    wchar_t *w = malloc((size_t)n * sizeof *w);
    if (!w) { errno = ENOMEM; return NULL; }
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n);
    return w;
}

/* Windows error code -> the closest errno. */
static int win_errno(DWORD e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: return ENOENT;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: return EACCES;
    case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS: return EEXIST;
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: return ENOSPC;
    case ERROR_NOT_SAME_DEVICE: return EXDEV;
    case ERROR_FILENAME_EXCED_RANGE: return ENAMETOOLONG;
    default: return EIO;
    }
}

unsigned long plat_random(void) {
    unsigned int v = 0;
    if (rand_s(&v) != 0) v = (unsigned int)GetTickCount();
    return v;
}

int plat_is_sep(char c) { return c == '/' || c == '\\'; }

int plat_exe_dir(char *buf, size_t size) {
    WCHAR w[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(NULL, w, (DWORD)(sizeof w / sizeof w[0]));
    if (n == 0 || n >= sizeof w / sizeof w[0]) { errno = ENAMETOOLONG; return -1; }
    WCHAR *slash = wcsrchr(w, L'\\');
    if (slash) *slash = 0;
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)size, NULL, NULL);
    if (len <= 0) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

int plat_config_dir(char *buf, size_t size) {
    const char *appdata = getenv("APPDATA");
    if (!appdata || !*appdata) { errno = ENOENT; return -1; }
    if ((size_t)snprintf(buf, size, "%s\\sqlc", appdata) >= size) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

const char *plat_home(void) {
    const char *h = getenv("USERPROFILE");
    return h && *h ? h : NULL;
}

FILE *plat_fopen(const char *path, const char *mode) {
    wchar_t *wp = widen(path), *wm = widen(mode);
    FILE *f = wp && wm ? _wfopen(wp, wm) : NULL;
    free(wp);
    free(wm);
    return f;
}

int plat_mkdir(const char *path) {
    /* "C:" alone is a drive, not a folder to create */
    size_t n = strlen(path);
    if (n == 2 && path[1] == ':') return 0;
    wchar_t *w = widen(path);
    if (!w) return -1;
    int rc = _wmkdir(w);
    free(w);
    return rc != 0 && errno == EEXIST ? 0 : rc;
}

int plat_unlink(const char *path) {
    wchar_t *w = widen(path);
    if (!w) return -1;
    int rc = _wunlink(w);
    free(w);
    return rc;
}

FILE *plat_mkstemp(char *template_path, int private_file) {
    (void)private_file;
    size_t n = strlen(template_path);
    if (n < 6 || strcmp(template_path + n - 6, "XXXXXX") != 0) { errno = EINVAL; return NULL; }
    static const char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    unsigned seed = (unsigned)GetTickCount() ^ (unsigned)GetCurrentProcessId() ^ (unsigned)(uintptr_t)template_path;
    for (int attempt = 0; attempt < 100; attempt++) {
        for (size_t i = n - 6; i < n; i++) {
            seed = seed * 1103515245u + 12345u;
            template_path[i] = chars[(seed >> 16) % (sizeof chars - 1)];
        }
        wchar_t *w = widen(template_path);
        if (!w) return NULL;
        int fd = _wopen(w, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
        free(w);
        if (fd >= 0) {
            FILE *f = _fdopen(fd, "wb");
            if (!f) { int e = errno; _close(fd); plat_unlink(template_path); errno = e; }
            return f;
        }
        if (errno != EEXIST) return NULL;
    }
    errno = EEXIST;
    return NULL;
}

long plat_getline(char **line, size_t *cap, FILE *f) {
    size_t n = 0;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (n + 2 > *cap || !*line) {
            size_t ncap = *cap < 128 ? 128 : *cap * 2;
            char *p = realloc(*line, ncap);
            if (!p) { errno = ENOMEM; return -1; }
            *line = p;
            *cap = ncap;
        }
        (*line)[n++] = (char)c;
        if (c == '\n') break;
    }
    if (n == 0) return -1;
    (*line)[n] = '\0';
    return (long)n;
}

int plat_fsync(FILE *f) {
    if (fflush(f) != 0) return -1;
    return _commit(_fileno(f));
}

int plat_replace(const char *from, const char *to) {
    wchar_t *wf = widen(from), *wt = widen(to);
    int rc = -1;
    if (wf && wt) {
        if (MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) rc = 0;
        else errno = win_errno(GetLastError());
    }
    free(wf);
    free(wt);
    return rc;
}

#else /* macOS / POSIX */

#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

unsigned long plat_random(void) { return (unsigned long)arc4random(); }

int plat_is_sep(char c) { return c == '/'; }

int plat_exe_dir(char *buf, size_t size) {
    (void)buf; (void)size;
    errno = ENOSYS;
    return -1;
}

int plat_config_dir(char *buf, size_t size) {
    const char *home = getenv("HOME");
    if (!home || !*home) { errno = ENOENT; return -1; }
    if ((size_t)snprintf(buf, size, "%s/Library/Application Support/sqlc", home) >= size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

const char *plat_home(void) {
    const char *h = getenv("HOME");
    return h && *h ? h : NULL;
}

FILE *plat_fopen(const char *path, const char *mode) { return fopen(path, mode); }

int plat_mkdir(const char *path) {
    return mkdir(path, 0700) != 0 && errno != EEXIST ? -1 : 0;
}

int plat_unlink(const char *path) { return unlink(path); }

FILE *plat_mkstemp(char *template_path, int private_file) {
    int fd = mkstemp(template_path);
    if (fd < 0) return NULL;
    if (!private_file) fchmod(fd, 0644);
    FILE *f = fdopen(fd, "w");
    if (!f) { int e = errno; close(fd); unlink(template_path); errno = e; }
    return f;
}

long plat_getline(char **line, size_t *cap, FILE *f) { return (long)getline(line, cap, f); }

int plat_fsync(FILE *f) {
    if (fflush(f) != 0) return -1;
    return fsync(fileno(f));
}

int plat_replace(const char *from, const char *to) { return rename(from, to); }

#endif
