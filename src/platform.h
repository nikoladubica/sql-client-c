#ifndef SQLC_PLATFORM_H
#define SQLC_PLATFORM_H

#include <stdio.h>

/*
 * The few file-system calls that differ between macOS and Windows, so
 * profiles.c and export.c stay one implementation. Paths are UTF-8 on
 * both; on Windows they are converted to UTF-16 for the wide-character
 * API (the plain one would read them in the ANSI code page). No UI, no
 * database. Every function sets errno on failure.
 */

/* '/' on macOS, '\\' on Windows: what config_path puts between folder and file. */
#ifdef _WIN32
#define PLAT_SEP '\\'
#else
#define PLAT_SEP '/'
#endif

/* printf-style checking: MinGW's GCC checks plain "printf" against the
 * Microsoft library, which lacks %zu; sqlc uses C99 stdio there. */
#ifdef __MINGW32__
#define PLAT_PRINTF(fmt, first) __attribute__((format(gnu_printf, fmt, first)))
#else
#define PLAT_PRINTF(fmt, first) __attribute__((format(printf, fmt, first)))
#endif

/* A random number (arc4random / rand_s), e.g. to pick a tip. */
unsigned long plat_random(void);

/* The folder the running program is in (UTF-8, no trailing separator).
 * 0, or -1 (Windows only; elsewhere always -1, ENOSYS). */
int plat_exe_dir(char *buf, size_t size);

/* 1 when c separates path components ('/', and '\\' on Windows). */
int plat_is_sep(char c);

/*
 * The folder sqlc's settings live in, without a trailing separator, into
 * buf: "$HOME/Library/Application Support/sqlc" on macOS,
 * "%APPDATA%\sqlc" on Windows. 0, or -1 (errno ENOENT) when the variable
 * is unset or empty, or ENAMETOOLONG when it does not fit.
 */
int plat_config_dir(char *buf, size_t size);

/* The user's home folder for "~" in messages: $HOME, or %USERPROFILE% on Windows. NULL when unset. */
const char *plat_home(void);

/* fopen with a UTF-8 path. mode as for fopen; use "rb" / "wb" for exact bytes. */
FILE *plat_fopen(const char *path, const char *mode);

/* Create one folder (owner-only on macOS); 0 when it exists already. */
int plat_mkdir(const char *path);

/* Remove a file; 0, or -1 with errno (ENOENT when it was not there). */
int plat_unlink(const char *path);

/*
 * Create and open a new file for writing, binary, named template with its
 * trailing "XXXXXX" replaced (mkstemp). private_file: owner-only (0600)
 * on macOS, else 0644; Windows files get the folder's permissions.
 */
FILE *plat_mkstemp(char *template_path, int private_file);

/*
 * getline(): read one line (with its '\n') into *line, growing it (*cap);
 * the length, or -1 at the end of the file or on error.
 */
long plat_getline(char **line, size_t *cap, FILE *f);

/* Flush f to the disk (fflush + fsync / _commit). 0 or -1. */
int plat_fsync(FILE *f);

/*
 * Move from over to (replacing it if it exists) in one step:
 * rename() on macOS, MoveFileExW(MOVEFILE_REPLACE_EXISTING) on Windows,
 * where rename() refuses to replace a file.
 */
int plat_replace(const char *from, const char *to);

#endif
