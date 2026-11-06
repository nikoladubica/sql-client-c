/*
 * sqlc - a minimal MySQL/MariaDB command-line client.
 *
 * Built on MariaDB Connector/C (libmariadb), which speaks the wire
 * protocol for us. Also works against MySQL servers.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <getopt.h>
#include <readpassphrase.h>
#include <editline/readline.h>
#include <mysql.h>

#include "complete.h"

#define MAX_PASSWORD 256

typedef struct {
    const char *host;
    const char *user;
    const char *password;
    const char *database;
    const char *socket;
    unsigned int port;
    const char *execute; /* -e "SQL": run and exit */
    int auto_rehash;     /* load names for tab completion (off with -A) */
    int hide_views;      /* --no-views: drop views from SHOW TABLES */
} options_t;

static options_t opt = { .host = "localhost", .auto_rehash = 1 };

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options] [database]\n"
        "  -h, --host=HOST        server host (default: localhost)\n"
        "  -P, --port=PORT        server port (default: 3306)\n"
        "  -u, --user=USER        user name\n"
        "  -p, --password[=PASS]  password; prompts if no value (-pPASS, no space)\n"
        "  -D, --database=DB      default database\n"
        "  -S, --socket=PATH      unix socket path\n"
        "  -e, --execute=SQL      execute statement(s) and exit\n"
        "  -A, --no-auto-rehash   don't load table/column names for tab completion\n"
        "                         (faster startup on big databases)\n"
        "      --no-views         hide views from SHOW TABLES output\n"
        "  -?, --help             show this help\n"
        "\n"
        "Interactive commands: quit | exit | \\q, rehash | \\#, views on|off\n",
        prog);
}

static void print_error(MYSQL *conn) {
    fflush(stdout);
    fprintf(stderr, "ERROR %u (%s): %s\n",
            mysql_errno(conn), mysql_sqlstate(conn), mysql_error(conn));
}

/* Print a horizontal separator like +------+-----+ */
static void print_separator(const size_t *widths, const int *show, unsigned int n) {
    putchar('+');
    for (unsigned int i = 0; i < n; i++) {
        if (!show[i]) continue;
        for (size_t j = 0; j < widths[i] + 2; j++) putchar('-');
        putchar('+');
    }
    putchar('\n');
}

/*
 * How to filter a result set. type_col >= 0 means "skip rows whose value in
 * that column is a view"; hide_type_col also removes that column from output.
 */
typedef struct {
    int type_col;
    int hide_type_col;
} filter_t;

static const filter_t no_filter = { -1, 0 };

static int row_is_view(MYSQL_ROW row, const filter_t *f) {
    return f->type_col >= 0 && row[f->type_col] &&
           strstr(row[f->type_col], "VIEW") != NULL;
}

static void print_result(MYSQL_RES *res, const filter_t *f) {
    unsigned int ncols = mysql_num_fields(res);
    MYSQL_FIELD *fields = mysql_fetch_fields(res);
    size_t *widths = calloc(ncols, sizeof *widths);
    int *show = calloc(ncols, sizeof *show);
    if (!widths || !show) { perror("calloc"); free(widths); free(show); return; }

    for (unsigned int i = 0; i < ncols; i++) {
        show[i] = !(f->hide_type_col && (int)i == f->type_col);
        size_t w = strlen(fields[i].name);
        widths[i] = w < 4 ? 4 : w; /* room for "NULL" */
    }

    /* First pass: column widths from the rows we will actually print. */
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        if (row_is_view(row, f)) continue;
        unsigned long *lens = mysql_fetch_lengths(res);
        for (unsigned int i = 0; i < ncols; i++)
            if (row[i] && lens[i] > widths[i]) widths[i] = lens[i];
    }
    mysql_data_seek(res, 0);

    print_separator(widths, show, ncols);
    putchar('|');
    for (unsigned int i = 0; i < ncols; i++)
        if (show[i]) printf(" %-*s |", (int)widths[i], fields[i].name);
    putchar('\n');
    print_separator(widths, show, ncols);

    unsigned long long printed = 0;
    while ((row = mysql_fetch_row(res))) {
        if (row_is_view(row, f)) continue;
        putchar('|');
        for (unsigned int i = 0; i < ncols; i++) {
            if (!show[i]) continue;
            const char *val = row[i] ? row[i] : "NULL";
            if (IS_NUM(fields[i].type))
                printf(" %*s |", (int)widths[i], val);
            else
                printf(" %-*s |", (int)widths[i], val);
        }
        putchar('\n');
        printed++;
    }
    print_separator(widths, show, ncols);

    printf("%llu row%s in set\n\n", printed, printed == 1 ? "" : "s");
    free(widths);
    free(show);
}

/* Case-insensitive keyword match at *p followed by a word boundary. */
static int match_word(const char **p, const char *word) {
    size_t n = strlen(word);
    if (strncasecmp(*p, word, n) != 0) return 0;
    char next = (*p)[n];
    if (next != '\0' && next != ';' && !isspace((unsigned char)next)) return 0;
    *p += n;
    while (isspace((unsigned char)**p)) (*p)++;
    return 1;
}

/*
 * If sql is a single "SHOW [FULL] TABLES ..." statement, return a malloc'd
 * "SHOW FULL TABLES ..." so we get the Table_type column to filter on.
 * *was_full tells whether the user asked for FULL themselves.
 */
static char *rewrite_show_tables(const char *sql, int *was_full) {
    const char *p = sql;
    while (isspace((unsigned char)*p)) p++;
    if (!match_word(&p, "SHOW")) return NULL;
    *was_full = match_word(&p, "FULL");
    if (!match_word(&p, "TABLES")) return NULL;

    /* Only handle a single statement: no ';' before the trailing one. */
    const char *semi = strchr(p, ';');
    if (semi) {
        const char *q = semi + 1;
        while (isspace((unsigned char)*q)) q++;
        if (*q != '\0') return NULL;
    }

    const char *prefix = "SHOW FULL TABLES ";
    char *out = malloc(strlen(prefix) + strlen(p) + 1);
    if (!out) return NULL;
    strcpy(out, prefix);
    strcat(out, p);
    return out;
}

static int find_column(MYSQL_RES *res, const char *name) {
    unsigned int n = mysql_num_fields(res);
    MYSQL_FIELD *fields = mysql_fetch_fields(res);
    for (unsigned int i = 0; i < n; i++)
        if (strcasecmp(fields[i].name, name) == 0) return (int)i;
    return -1;
}

/* Run SQL (possibly several statements) and print every result set. */
static int run_query(MYSQL *conn, const char *sql) {
    int was_full = 0;
    char *rewritten = opt.hide_views ? rewrite_show_tables(sql, &was_full) : NULL;

    int filtering = rewritten != NULL;
    int qrc = mysql_query(conn, rewritten ? rewritten : sql);
    free(rewritten);
    if (qrc != 0) { print_error(conn); return -1; }

    int status;
    do {
        MYSQL_RES *res = mysql_store_result(conn);
        if (res) {
            filter_t f = no_filter;
            if (filtering) {
                f.type_col = find_column(res, "Table_type");
                f.hide_type_col = !was_full;
            }
            print_result(res, &f);
            mysql_free_result(res);
        } else if (mysql_field_count(conn) == 0) {
            my_ulonglong n = mysql_affected_rows(conn);
            printf("Query OK, %llu row%s affected\n\n", n, n == 1 ? "" : "s");
        } else {
            print_error(conn);
            return -1;
        }

        status = mysql_next_result(conn); /* 0 = more, -1 = done, >0 = error */
        if (status > 0) { print_error(conn); return -1; }
    } while (status == 0);

    return 0;
}

static char *rtrim(char *s) {
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
    return s;
}

static const char *ltrim(const char *s) {
    while (isspace((unsigned char)*s)) s++;
    return s;
}

/* Compare a command, ignoring case and an optional trailing ';'. */
static int is_cmd(const char *line, const char *cmd) {
    size_t n = strlen(cmd);
    return strncasecmp(line, cmd, n) == 0 &&
           (line[n] == '\0' || (line[n] == ';' && line[n + 1] == '\0'));
}

/* Client-side commands. Returns 1 if handled, -1 to quit, 0 otherwise. */
static int client_command(MYSQL *conn, const char *line) {
    line = ltrim(line);
    if (is_cmd(line, "quit") || is_cmd(line, "exit") || is_cmd(line, "\\q"))
        return -1;
    if (is_cmd(line, "rehash") || is_cmd(line, "\\#")) {
        if (!opt.auto_rehash) {
            printf("Tab completion is off (-A).\n");
        } else {
            completion_rehash(conn);
            printf("Completion names reloaded.\n");
        }
        return 1;
    }
    if (is_cmd(line, "views off")) {
        opt.hide_views = 1;
        printf("Views hidden from SHOW TABLES.\n");
        return 1;
    }
    if (is_cmd(line, "views on")) {
        opt.hide_views = 0;
        printf("Views shown in SHOW TABLES.\n");
        return 1;
    }
    return 0;
}

/* Did this buffer start with USE? Then the current database changed. */
static int starts_with_use(const char *sql) {
    const char *p = ltrim(sql);
    return strncasecmp(p, "use", 3) == 0 &&
           (isspace((unsigned char)p[3]) || p[3] == '`');
}

/*
 * Interactive loop. Lines are accumulated until one ends with ';',
 * then the whole buffer is sent to the server.
 */
static void repl(MYSQL *conn) {
    char *buf = NULL;
    size_t buflen = 0;

    completion_init(opt.auto_rehash);
    if (opt.auto_rehash) completion_rehash(conn);

    for (;;) {
        char *line = readline(buflen == 0 ? "sqlc> " : "   -> ");
        if (!line) { putchar('\n'); break; } /* Ctrl-D */

        rtrim(line);
        if (buflen == 0 && line[0] == '\0') { free(line); continue; }
        if (buflen == 0) {
            int cc = client_command(conn, line);
            if (cc != 0) {
                if (cc > 0) add_history(line);
                free(line);
                if (cc < 0) break;
                continue;
            }
        }

        size_t add = strlen(line);
        char *nbuf = realloc(buf, buflen + add + 2);
        if (!nbuf) { perror("realloc"); free(line); break; }
        buf = nbuf;
        if (buflen > 0) buf[buflen++] = '\n';
        memcpy(buf + buflen, line, add + 1);
        buflen += add;
        free(line);

        if (buflen > 0 && buf[buflen - 1] == ';') {
            add_history(buf);
            if (run_query(conn, buf) == 0 && starts_with_use(buf))
                completion_rehash(conn);
            buflen = 0;
            buf[0] = '\0';
        }
    }
    free(buf);
    completion_free();
}

enum { OPT_NO_VIEWS = 1000 };

int main(int argc, char **argv) {
    char pwbuf[MAX_PASSWORD];
    int prompt_password = 0;
    int c;

    static const struct option longopts[] = {
        { "host",           required_argument, NULL, 'h' },
        { "port",           required_argument, NULL, 'P' },
        { "user",           required_argument, NULL, 'u' },
        { "password",       optional_argument, NULL, 'p' },
        { "database",       required_argument, NULL, 'D' },
        { "socket",         required_argument, NULL, 'S' },
        { "execute",        required_argument, NULL, 'e' },
        { "no-auto-rehash", no_argument,       NULL, 'A' },
        { "no-views",       no_argument,       NULL, OPT_NO_VIEWS },
        { "help",           no_argument,       NULL, '?' },
        { NULL, 0, NULL, 0 },
    };

    while ((c = getopt_long(argc, argv, "Ah:P:u:p::D:S:e:?", longopts, NULL)) != -1) {
        switch (c) {
        case 'h': opt.host = optarg; break;
        case 'P': opt.port = (unsigned int)strtoul(optarg, NULL, 10); break;
        case 'u': opt.user = optarg; break;
        case 'p':
            if (optarg) opt.password = optarg;
            else prompt_password = 1;
            break;
        case 'D': opt.database = optarg; break;
        case 'S': opt.socket = optarg; break;
        case 'e': opt.execute = optarg; break;
        case 'A': opt.auto_rehash = 0; break;
        case OPT_NO_VIEWS: opt.hide_views = 1; break;
        case '?':
            usage(argv[0]);
            /* getopt also returns '?' for unknown options */
            return (optopt == 0 || optopt == '?') ? 0 : 1;
        default: usage(argv[0]); return 1;
        }
    }
    /* Allow a trailing positional database name, like `mysql dbname` */
    if (optind < argc && !opt.database) opt.database = argv[optind];

    if (prompt_password) {
        if (!readpassphrase("Enter password: ", pwbuf, sizeof pwbuf, RPP_REQUIRE_TTY)) {
            fprintf(stderr, "Could not read password\n");
            return 1;
        }
        opt.password = pwbuf;
    }

    MYSQL *conn = mysql_init(NULL);
    if (!conn) { fprintf(stderr, "mysql_init failed\n"); return 1; }

    mysql_options(conn, MYSQL_SET_CHARSET_NAME, "utf8mb4");

    if (!mysql_real_connect(conn, opt.host, opt.user, opt.password,
                            opt.database, opt.port, opt.socket,
                            CLIENT_MULTI_STATEMENTS | CLIENT_MULTI_RESULTS)) {
        print_error(conn);
        mysql_close(conn);
        return 1;
    }
    memset(pwbuf, 0, sizeof pwbuf);

    int rc = 0;
    if (opt.execute) {
        rc = run_query(conn, opt.execute) == 0 ? 0 : 1;
    } else {
        printf("Connected to %s (server %s). Type 'quit' or Ctrl-D to exit.\n",
               mysql_get_host_info(conn), mysql_get_server_info(conn));
        printf("End statements with ';'.%s\n\n",
               opt.auto_rehash ? " Tab completes names (views excluded)." : "");
        repl(conn);
    }

    mysql_close(conn);
    return rc;
}
