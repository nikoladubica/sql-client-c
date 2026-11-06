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
#include "db.h"

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

/* Print a horizontal separator like +------+-----+ */
static void print_separator(const size_t *widths, unsigned int n) {
    putchar('+');
    for (unsigned int i = 0; i < n; i++) {
        for (size_t j = 0; j < widths[i] + 2; j++) putchar('-');
        putchar('+');
    }
    putchar('\n');
}

static void print_set(const db_set *s) {
    if (!s->has_rows) {
        printf("Query OK, %llu row%s affected\n\n", s->affected, s->affected == 1 ? "" : "s");
        return;
    }

    size_t *widths = calloc(s->ncols ? s->ncols : 1, sizeof *widths);
    if (!widths) { perror("calloc"); return; }
    for (unsigned int i = 0; i < s->ncols; i++) {
        size_t w = strlen(s->names[i]);
        widths[i] = w < 4 ? 4 : w; /* room for "NULL" */
        for (size_t r = 0; r < s->nrows; r++) {
            const char *v = db_cell(s, r, i);
            if (v && strlen(v) > widths[i]) widths[i] = strlen(v);
        }
    }

    print_separator(widths, s->ncols);
    putchar('|');
    for (unsigned int i = 0; i < s->ncols; i++)
        printf(" %-*s |", (int)widths[i], s->names[i]);
    putchar('\n');
    print_separator(widths, s->ncols);

    for (size_t r = 0; r < s->nrows; r++) {
        putchar('|');
        for (unsigned int i = 0; i < s->ncols; i++) {
            const char *v = db_cell(s, r, i);
            if (!v) v = "NULL";
            printf(s->numeric[i] ? " %*s |" : " %-*s |", (int)widths[i], v);
        }
        putchar('\n');
    }
    print_separator(widths, s->ncols);

    printf("%zu row%s in set\n\n", s->nrows, s->nrows == 1 ? "" : "s");
    free(widths);
}

/* Run SQL (possibly several statements) and print every result set. */
static int run_query(MYSQL *conn, const char *sql) {
    db_exec_opts eo = { .hide_views = opt.hide_views, .max_rows = 0 };
    db_outcome out;
    int rc = db_execute(conn, sql, &eo, &out);
    for (size_t i = 0; i < out.nsets; i++) print_set(&out.sets[i]);
    if (out.error) {
        fflush(stdout);
        fprintf(stderr, "%s\n", out.error);
    }
    db_outcome_free(&out);
    return rc;
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
        char *e = db_error_string(conn);
        fprintf(stderr, "%s\n", e ? e : "connection failed");
        free(e);
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
