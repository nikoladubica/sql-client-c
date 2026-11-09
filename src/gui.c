/*
 * sqlc-gui - a simple MySQL/MariaDB GUI built with libui-ng.
 *
 * Layout:
 *   [Saved connections v][Name][x] Remember password [Save][Delete]
 *   [Host][Port][User][Password][Database][Connect]
 *   +-----------+------------------------------------+
 *   | Tables    | SQL editor                         |
 *   | [x] Hide  | [Run]                              |
 *   |  views    | results (one tab per result set)   |
 *   +-----------+------------------------------------+
 *   status line
 *
 * All database work runs on a worker thread; results come back to the UI
 * thread via uiQueueMain(), so the window never freezes on a slow query.
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>

#include <ui.h>
#include <mysql.h>
#include <curl/curl.h>

#include "ai.h"
#include "ai_db.h"
#include "db.h"
#include "keychain.h"
#include "macos_menu.h"
#include "splitview.h"
#include "export.h"
#include "export_db.h"
#include "export_dialog.h"
#include "table_menu.h"
#include "profiles.h"

#define GUI_MAX_ROWS 10000 /* rows kept per result set in the grid */
#define AI_SCHEMA_BYTES (200u * 1024) /* schema text sent to Claude, at most about */

/* ---------- table models backed by a db_set ---------- */

typedef struct {
    uiTableModelHandler mh; /* must be first: libui hands us &mh */
    uiTableModel *model;
    const db_set *set;
} set_model;

static int m_num_columns(uiTableModelHandler *mh, uiTableModel *m) {
    (void)m;
    return (int)((set_model *)mh)->set->ncols;
}

static uiTableValueType m_column_type(uiTableModelHandler *mh, uiTableModel *m, int col) {
    (void)mh; (void)m; (void)col;
    return uiTableValueTypeString;
}

static int m_num_rows(uiTableModelHandler *mh, uiTableModel *m) {
    (void)m;
    return (int)((set_model *)mh)->set->nrows;
}

static uiTableValue *m_cell_value(uiTableModelHandler *mh, uiTableModel *m, int row, int col) {
    (void)m;
    const char *v = db_cell(((set_model *)mh)->set, (size_t)row, (unsigned)col);
    return uiNewTableValueString(v ? v : "NULL");
}

static void m_set_cell_value(uiTableModelHandler *mh, uiTableModel *m, int row, int col,
                             const uiTableValue *v) {
    (void)mh; (void)m; (void)row; (void)col; (void)v; /* read-only grid */
}

static set_model *new_set_model(const db_set *set) {
    set_model *sm = calloc(1, sizeof *sm);
    if (!sm) return NULL;
    sm->mh.NumColumns = m_num_columns;
    sm->mh.ColumnType = m_column_type;
    sm->mh.NumRows = m_num_rows;
    sm->mh.CellValue = m_cell_value;
    sm->mh.SetCellValue = m_set_cell_value;
    sm->set = set;
    sm->model = uiNewTableModel(&sm->mh);
    return sm;
}

/* Only call after every uiTable using the model has been destroyed. */
static void free_set_model(set_model *sm) {
    if (!sm) return;
    uiFreeTableModel(sm->model);
    free(sm);
}

static uiTable *new_grid(set_model *sm) {
    uiTableParams p = { .Model = sm->model, .RowBackgroundColorModelColumn = -1 };
    uiTable *t = uiNewTable(&p);
    for (unsigned int c = 0; c < sm->set->ncols; c++)
        uiTableAppendTextColumn(t, sm->set->names[c], (int)c,
                                uiTableModelColumnNeverEditable, NULL);
    return t;
}

/* ---------- app state ---------- */

static struct {
    uiWindow *win;
    uiEntry *host, *port, *user, *pass, *db;

    /* saved connections; profile_box item 0 is "New connection",
     * item i is profiles.items[i - 1] */
    uiCombobox *profile_box;
    uiEntry *profile_name;
    uiCheckbox *remember_pw;
    profile_list profiles;
    char *profiles_file;
    uiButton *connect_btn, *run_btn, *load_tables_btn, *export_btn;
    uiCheckbox *hide_views;     /* views left out of the sidebar and SHOW TABLES */
    uiCheckbox *no_rehash;      /* -A: only load the table list on request */
    uiCheckbox *tables_once;    /* tables only, loaded on connect, then like -A */
    uiMultilineEntry *editor;
    uiLabel *status;

    /* Ask Claude: describe a query, get SQL in the editor */
    uiEntry *ai_task;
    uiButton *ai_btn;
    int ai_in_editor;        /* the editor holds Claude's SQL, not run yet */

    /* results area: holder box with one child (a tab, grid or label) */
    uiBox *results_box;
    uiControl *results;
    set_model **models;
    size_t nmodels;
    db_outcome shown; /* data behind the result grids */

    /* sidebar: list of tables in the current database */
    uiControl *split;        /* sidebar | editor + results */
    uiBox *side_box;
    uiTable *side_table;
    set_model *side_model;
    db_outcome side_data;    /* the visible list (1 column) */
    db_outcome tables_raw;   /* TABLE_NAME, TABLE_TYPE from the server */
    size_t *side_index;      /* visible row -> row in tables_raw */
    int tables_loaded;       /* tables_raw holds this connection's list */
    char *menu_table;        /* table the context menu was opened on */
    int menu_is_view;
    char *browsing;          /* table shown by a double-click browse, or NULL */

    MYSQL *conn;
    int busy;
} app;

static void set_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_status(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    uiLabelSetText(app.status, buf);
}

static void show_error(const char *title, const char *msg) {
#ifdef SQLC_AUTOTEST
    printf("[dialog] %s: %s\n", title, msg);
#else
    uiMsgBoxError(app.win, title, msg);
#endif
}

static void set_busy(int busy) {
    app.busy = busy;
    if (busy) {
        uiControlDisable(uiControl(app.connect_btn));
        uiControlDisable(uiControl(app.run_btn));
        uiControlDisable(uiControl(app.load_tables_btn));
        uiControlDisable(uiControl(app.export_btn));
        uiControlDisable(uiControl(app.ai_btn));
    } else {
        uiControlEnable(uiControl(app.connect_btn));
        uiControlEnable(uiControl(app.run_btn));
        uiControlEnable(uiControl(app.load_tables_btn));
        uiControlEnable(uiControl(app.export_btn));
        uiControlEnable(uiControl(app.ai_btn));
    }
}

/* Load the table list on connect? Not with -A, but "Tables only, load once" does. */
static int load_on_connect(void) {
    return !uiCheckboxChecked(app.no_rehash) || uiCheckboxChecked(app.tables_once);
}

/* Reload it after every query? Neither -A nor "Tables only, load once" does. */
static int load_after_queries(void) {
    return !uiCheckboxChecked(app.no_rehash) && !uiCheckboxChecked(app.tables_once);
}

/* ---------- results area ---------- */

static void clear_results(void) {
    if (app.results) {
        uiBoxDelete(app.results_box, 0);
        uiControlDestroy(app.results); /* destroys the grids first ... */
        app.results = NULL;
    }
    for (size_t i = 0; i < app.nmodels; i++) free_set_model(app.models[i]); /* ... then models */
    free(app.models);
    app.models = NULL;
    app.nmodels = 0;
    db_outcome_free(&app.shown);
}

static void show_results_control(uiControl *c) {
    app.results = c;
    uiBoxAppend(app.results_box, c, 1);
}

static uiControl *message_page(const char *text) {
    uiBox *b = uiNewVerticalBox();
    uiBoxSetPadded(b, 1);
    uiBoxAppend(b, uiControl(uiNewLabel(text)), 0);
    return uiControl(b);
}

/* Takes ownership of out. */
static void show_outcome(db_outcome *out) {
    clear_results();
    app.shown = *out;
    memset(out, 0, sizeof *out);

    size_t npages = app.shown.nsets + (app.shown.error ? 1 : 0);
    app.models = calloc(app.shown.nsets ? app.shown.nsets : 1, sizeof *app.models);

    uiControl **pages = calloc(npages ? npages : 1, sizeof *pages);
    char (*titles)[32] = calloc(npages ? npages : 1, sizeof *titles);
    if (!pages || !titles || !app.models) { free(pages); free(titles); return; }

    size_t p = 0;
    for (size_t i = 0; i < app.shown.nsets; i++, p++) {
        const db_set *s = &app.shown.sets[i];
        snprintf(titles[p], sizeof titles[p], "Result %zu", i + 1);
        if (s->has_rows) {
            set_model *sm = new_set_model(s);
            app.models[app.nmodels++] = sm;
            pages[p] = uiControl(new_grid(sm));
        } else {
            char msg[128];
            snprintf(msg, sizeof msg, "Query OK, %llu row%s affected",
                     s->affected, s->affected == 1 ? "" : "s");
            pages[p] = message_page(msg);
        }
    }
    if (app.shown.error) {
        snprintf(titles[p], sizeof titles[p], "Error");
        pages[p++] = message_page(app.shown.error);
    }

    if (npages == 1) {
        show_results_control(pages[0]);
    } else if (npages > 1) {
        uiTab *tab = uiNewTab();
        for (size_t i = 0; i < npages; i++) uiTabAppend(tab, titles[i], pages[i]);
        show_results_control(uiControl(tab));
    }
    free(pages);
    free(titles);
}

/* ---------- sidebar ---------- */

static void on_table_double_click(uiTable *t, int row, void *data);
static unsigned on_table_menu_open(int row, void *data);
static void on_table_menu_action(table_menu_item item, int row, void *data);

static int is_view(const char *type) { return type && strstr(type, "VIEW") != NULL; }

/* Rebuild the visible table list from tables_raw and the "Hide views" box. */
static void rebuild_sidebar(void) {
    if (app.side_table) {
        uiBoxDelete(app.side_box, uiBoxNumChildren(app.side_box) - 1);
        uiControlDestroy(uiControl(app.side_table));
        app.side_table = NULL;
    }
    free_set_model(app.side_model);
    app.side_model = NULL;
    db_outcome_free(&app.side_data);
    free(app.side_index);
    app.side_index = NULL;

    int hide = uiCheckboxChecked(app.hide_views);
    const db_set *raw = app.tables_raw.nsets ? &app.tables_raw.sets[0] : NULL;
    size_t n = raw ? raw->nrows : 0;

    db_set *s = calloc(1, sizeof *s);
    app.side_index = calloc(n ? n : 1, sizeof *app.side_index);
    if (!s || !app.side_index) { free(s); return; }
    s->has_rows = 1;
    s->ncols = 1;
    s->names = calloc(1, sizeof *s->names);
    s->numeric = calloc(1, sizeof *s->numeric);
    s->cells = calloc(n ? n : 1, sizeof *s->cells);
    s->names[0] = strdup("Tables");

    for (size_t r = 0; r < n; r++) {
        const char *name = db_cell(raw, r, 0);
        const char *type = db_cell(raw, r, 1);
        if (!name || (hide && is_view(type))) continue;
        char buf[512];
        snprintf(buf, sizeof buf, is_view(type) ? "%s  (view)" : "%s", name);
        s->cells[s->nrows] = strdup(buf);
        app.side_index[s->nrows] = r;
        s->nrows++;
    }
    app.side_data.sets = s;
    app.side_data.nsets = 1;

    app.side_model = new_set_model(s);
    app.side_table = new_grid(app.side_model);
    sqlc_table_fill_width(app.side_table); /* column follows the sidebar width */
    uiTableOnRowDoubleClicked(app.side_table, on_table_double_click, NULL);
    sqlc_table_set_context_menu(app.side_table, on_table_menu_open, on_table_menu_action, NULL);
    uiBoxAppend(app.side_box, uiControl(app.side_table), 1);
}

/* ---------- worker thread ---------- */

typedef enum { JOB_CONNECT, JOB_QUERY, JOB_TABLES, JOB_EXPORT, JOB_ALTER, JOB_AI } job_kind;

/* JOB_ALTER: what the context menu asked for */
typedef enum { ALTER_RENAME, ALTER_TRUNCATE, ALTER_DROP } alter_kind;

typedef struct {
    job_kind kind;
    MYSQL *conn;              /* existing connection, or the new one */
    char *host, *user, *pass, *dbname;
    unsigned int port;
    char *sql;
    db_exec_opts eo;
    char *profile;            /* saved connection used to connect, if any */
    int load_tables;          /* refresh the table list afterwards */
    int hide_views;           /* ... leaving views out of it */
    /* JOB_EXPORT, JOB_ALTER */
    char *table, *path;
    alter_kind alter;
    char *new_name;           /* ALTER_RENAME */
    int is_view;
    export_format fmt;
    export_opts xopts;
    unsigned long long rows;
    /* JOB_AI */
    char *task, *api_key;
    ai_result ai;

    /* results, filled by the worker */
    int ok;
    char *error;              /* connect error */
    db_outcome out;           /* query results */
    db_outcome tables;        /* refreshed table list (if load_tables) */
    char *current_db;
    double seconds;
} job;

static void job_free(job *j) {
    free(j->host); free(j->user); free(j->dbname); free(j->sql); free(j->error);
    if (j->pass) { memset(j->pass, 0, strlen(j->pass)); free(j->pass); }
    free(j->current_db);
    free(j->profile);
    free(j->table);
    free(j->path);
    free(j->new_name);
    free(j->task);
    if (j->api_key) { memset(j->api_key, 0, strlen(j->api_key)); free(j->api_key); }
    ai_result_free(&j->ai);
    db_outcome_free(&j->out);
    db_outcome_free(&j->tables);
    free(j);
}

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Table list (if asked for) + current database; runs on the worker thread. */
static void load_tables(job *j) {
    db_exec_opts eo = { 0 };
    if (j->load_tables)
        db_execute(j->conn,
                   j->hide_views
                   ? "SELECT TABLE_NAME, TABLE_TYPE FROM information_schema.TABLES "
                     "WHERE TABLE_SCHEMA = DATABASE() "
                     "AND TABLE_TYPE NOT IN ('VIEW', 'SYSTEM VIEW') ORDER BY TABLE_NAME"
                   : "SELECT TABLE_NAME, TABLE_TYPE FROM information_schema.TABLES "
                     "WHERE TABLE_SCHEMA = DATABASE() ORDER BY TABLE_NAME",
                   &eo, &j->tables);
    db_outcome cur;
    if (db_execute(j->conn, "SELECT DATABASE()", &eo, &cur) == 0 && cur.nsets &&
        cur.sets[0].nrows && db_cell(&cur.sets[0], 0, 0))
        j->current_db = strdup(db_cell(&cur.sets[0], 0, 0));
    db_outcome_free(&cur);
}

static void job_done(void *data);
static void remember_last_profile(const char *name);
#ifdef SQLC_AUTOTEST
static void autotest_next(void);
static int on_should_quit(void *data);
#endif

/* JOB_AI on the worker thread: schema, then the Claude API call. */
static void ask_claude(job *j) {
    db_exec_opts eo = { 0 };
    db_outcome cur;
    char *dbname = NULL;
    if (db_execute(j->conn, "SELECT DATABASE()", &eo, &cur) == 0 && cur.nsets &&
        cur.sets[0].nrows && db_cell(&cur.sets[0], 0, 0))
        dbname = strdup(db_cell(&cur.sets[0], 0, 0));
    db_outcome_free(&cur);

    char *schema = dbname ? ai_schema(j->conn, AI_SCHEMA_BYTES) : NULL;
    char *body = ai_build_request(AI_MODEL, mysql_get_server_info(j->conn), dbname,
                                  schema, j->task);
    if (!body) j->ai.error = strdup("Out of memory");
    else j->ok = ai_generate(j->api_key, body, &j->ai) == 0;
    free(body);
    free(schema);
    free(dbname);
}

/* Export progress: called on the worker thread, shown on the UI thread. */
static void show_export_progress(void *data) {
    unsigned long long *rows = data;
    if (app.busy) set_status("Exporting... %llu rows", *rows);
    free(rows);
}

static void export_progress(unsigned long long rows, void *data) {
    (void)data;
    unsigned long long *r = malloc(sizeof *r);
    if (!r) return;
    *r = rows;
    uiQueueMain(show_export_progress, r);
}

static void *worker(void *data) {
    job *j = data;
    double t0 = now_seconds();

    if (j->kind == JOB_CONNECT) {
        j->conn = mysql_init(NULL);
        if (!j->conn) {
            j->error = strdup("mysql_init failed");
        } else {
            mysql_options(j->conn, MYSQL_SET_CHARSET_NAME, "utf8mb4");
            if (mysql_real_connect(j->conn, j->host, j->user, j->pass, j->dbname,
                                   j->port, NULL,
                                   CLIENT_MULTI_STATEMENTS | CLIENT_MULTI_RESULTS)) {
                j->ok = 1;
            } else {
                j->error = db_error_string(j->conn);
                mysql_close(j->conn);
                j->conn = NULL;
            }
        }
    } else if (j->kind == JOB_QUERY || j->kind == JOB_ALTER) {
        j->ok = db_execute(j->conn, j->sql, &j->eo, &j->out) == 0;
    } else if (j->kind == JOB_AI) {
        ask_claude(j);
    } else if (j->kind == JOB_EXPORT) {
        j->ok = export_table(j->conn, j->table, j->is_view, j->fmt, &j->xopts, j->path,
                             export_progress, NULL, &j->rows, &j->error) == 0;
    } else {
        j->ok = 1; /* JOB_TABLES: just the refresh below */
    }
    j->seconds = now_seconds() - t0;

    if (j->conn) load_tables(j);
    uiQueueMain(job_done, j);
    return NULL;
}

static void start_job(job *j) {
    pthread_t th;
    set_busy(1);
    if (pthread_create(&th, NULL, worker, j) != 0) {
        set_busy(0);
        set_status("Could not start worker thread");
        job_free(j);
        return;
    }
    pthread_detach(th);
}

static void show_message_page(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void show_message_page(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    clear_results();
    show_results_control(message_page(buf));
}

/* "SELECT * FROM `name` LIMIT 1000;" (malloc'd) */
static char *browse_sql(const char *name) {
    char *q = db_quote_ident(name);
    if (!q) return NULL;
    size_t n = strlen(q) + 40;
    char *sql = malloc(n);
    if (sql) snprintf(sql, n, "SELECT * FROM %s LIMIT 1000;", q);
    free(q);
    return sql;
}

/* A rename/truncate/drop from the context menu finished. */
static void alter_done(job *j) {
    static const char *const verbs[] = { "Rename", "Truncate", "Drop" };
    if (!j->ok) {
        const char *err = j->out.error ? j->out.error : "unknown error";
        char title[64];
        snprintf(title, sizeof title, "%s failed", verbs[j->alter]);
        set_status("%s: %s", title, err);
        show_error(title, err);
        return;
    }
    const char *what = j->is_view ? "view" : "table";
    int shown = app.browsing && strcmp(app.browsing, j->table) == 0;
    switch (j->alter) {
    case ALTER_RENAME:
        set_status("Renamed %s %s to %s", what, j->table, j->new_name);
        if (shown) { /* same rows, new name: just follow it */
            free(app.browsing);
            app.browsing = strdup(j->new_name);
            char *sql = browse_sql(j->new_name);
            if (sql) uiMultilineEntrySetText(app.editor, sql);
            free(sql);
        }
        break;
    case ALTER_TRUNCATE:
        set_status("Truncated table %s", j->table);
        if (shown) show_message_page("Table %s was truncated: it has no rows now.", j->table);
        break;
    case ALTER_DROP:
        set_status("Dropped %s %s", what, j->table);
        if (shown) {
            show_message_page("The %s %s was dropped.", what, j->table);
            free(app.browsing);
            app.browsing = NULL;
        }
        break;
    }
}

/* Back on the UI thread. */
static void job_done(void *data) {
    job *j = data;
    set_busy(0);

    if (j->kind == JOB_CONNECT) {
        if (!j->ok) {
            set_status("%s", j->error ? j->error : "Connection failed");
            uiMsgBoxError(app.win, "Connection failed", j->error ? j->error : "");
            job_free(j);
            return;
        }
        if (app.conn) mysql_close(app.conn);
        app.conn = j->conn;
        remember_last_profile(j->profile);
        set_status("Connected to %s (server %s)%s%s%s",
                   mysql_get_host_info(app.conn), mysql_get_server_info(app.conn),
                   j->current_db ? " - database: " : "",
                   j->current_db ? j->current_db : "",
                   !j->load_tables ? " - table list not loaded (-A)"
                   : uiCheckboxChecked(app.tables_once) ? " - tables only, not refreshed after queries"
                   : "");
        /* The old connection's list is stale whether or not we reload it. */
        db_outcome_free(&app.tables_raw);
        app.tables_loaded = 0;
        free(app.browsing);
        app.browsing = NULL;
    } else if (j->kind == JOB_ALTER) {
        alter_done(j);
    } else if (j->kind == JOB_AI) {
        if (!j->ok) {
            const char *err = j->ai.error ? j->ai.error : "unknown error";
            set_status("Ask Claude failed: %s", err);
            show_error("Ask Claude failed", err);
        } else {
            char *text = ai_format(j->task, &j->ai);
            if (text) {
                uiMultilineEntrySetText(app.editor, text);
                app.ai_in_editor = 1;
                free(app.browsing);
                app.browsing = NULL;
            }
            free(text);
            if (j->ai.sql && *j->ai.sql)
                set_status("%s wrote a query in %.1f s - review it, then press Run.",
                           j->ai.model ? j->ai.model : "Claude", j->seconds);
            else
                set_status("Claude didn't write a query - see the comments in the editor.");
        }
    } else if (j->kind == JOB_EXPORT) {
        if (j->ok) {
            set_status("Exported %llu row%s of %s to %s (%.2f s)", j->rows, j->rows == 1 ? "" : "s",
                       j->table, j->path, j->seconds);
        } else {
            set_status("Export failed: %s", j->error ? j->error : "unknown error");
            show_error("Export failed", j->error ? j->error : "");
        }
    } else if (j->kind == JOB_TABLES) {
        const db_set *t = j->tables.nsets ? &j->tables.sets[0] : NULL;
        if (j->tables.error)
            set_status("Could not load tables: %s", j->tables.error);
        else
            set_status("%zu table%s%s%s", t ? t->nrows : 0, t && t->nrows == 1 ? "" : "s",
                       j->current_db ? " in " : " (no database selected)",
                       j->current_db ? j->current_db : "");
    } else {
        size_t rows = 0;
        int truncated = 0;
        unsigned long long affected = 0;
        size_t row_sets = 0;
        for (size_t i = 0; i < j->out.nsets; i++) {
            const db_set *s = &j->out.sets[i];
            if (s->has_rows) { rows += s->nrows; row_sets++; truncated |= s->truncated; }
            else affected += s->affected;
        }
        if (j->out.error)
            set_status("%s", j->out.error);
        else if (row_sets)
            set_status("%zu row%s in %.3f s%s%s%s", rows, rows == 1 ? "" : "s", j->seconds,
                       truncated ? " (grid shows the first 10000 rows per result)" : "",
                       j->current_db ? " - database: " : "",
                       j->current_db ? j->current_db : "");
        else
            set_status("Query OK, %llu row%s affected in %.3f s", affected,
                       affected == 1 ? "" : "s", j->seconds);
        show_outcome(&j->out);
    }

    /* Refresh the sidebar (the database may have changed via USE). */
    if (j->load_tables) {
        db_outcome_free(&app.tables_raw);
        app.tables_raw = j->tables;
        memset(&j->tables, 0, sizeof j->tables);
        app.tables_loaded = 1;
    }
    uiButtonSetText(app.load_tables_btn, app.tables_loaded ? "Refresh tables" : "Load tables");
    /* Only when the list changed, so the selection survives e.g. an export. */
    if (j->kind == JOB_CONNECT || j->load_tables) rebuild_sidebar();

    job_free(j);
#ifdef SQLC_AUTOTEST
    autotest_next();
#endif
}

/* ---------- UI callbacks ---------- */

static char *entry_dup(uiEntry *e) {
    char *t = uiEntryText(e);
    char *d = strdup(t ? t : "");
    uiFreeText(t);
    return d;
}

static void on_connect(uiButton *b, void *data) {
    (void)b; (void)data;
    if (app.busy) return;
    job *j = calloc(1, sizeof *j);
    if (!j) return;
    j->kind = JOB_CONNECT;
    j->host = entry_dup(app.host);
    j->user = entry_dup(app.user);
    j->pass = entry_dup(app.pass);
    j->dbname = entry_dup(app.db);
    char *port = entry_dup(app.port);
    j->port = (unsigned int)strtoul(port, NULL, 10);
    free(port);
    if (!j->host[0]) { free(j->host); j->host = strdup("localhost"); }
    if (!j->dbname[0]) { free(j->dbname); j->dbname = NULL; }
    int sel = uiComboboxSelected(app.profile_box);
    if (sel > 0 && (size_t)sel <= app.profiles.n)
        j->profile = strdup(app.profiles.items[sel - 1].name);
    j->load_tables = load_on_connect();
    j->hide_views = uiCheckboxChecked(app.hide_views);
    set_status("Connecting to %s...", j->host);
    start_job(j);
}

/* Returns 1 if the query was started. */
static int run_sql(const char *sql) {
    if (app.busy) return 0;
    if (!app.conn) { set_status("Not connected - fill in the fields above and press Connect."); return 0; }

    const char *p = sql;
    while (isspace((unsigned char)*p)) p++;
    if (!*p) { set_status("Nothing to run."); return 0; }

    job *j = calloc(1, sizeof *j);
    if (!j) return 0;
    j->kind = JOB_QUERY;
    j->conn = app.conn;
    j->sql = strdup(sql);
    j->eo.hide_views = uiCheckboxChecked(app.hide_views);
    j->eo.max_rows = GUI_MAX_ROWS;
    /* With -A (or "Tables only, load once") the list is only refreshed when
     * asked (Load/Refresh tables). */
    j->load_tables = load_after_queries();
    j->hide_views = j->eo.hide_views;
    set_status("Running...");
    start_job(j);
    return 1;
}

/* Claude's SQL is about to run: ask first if it destroys data or structure.
 * Returns 1 to go ahead. */
static int confirm_ai_sql(const char *sql) {
    if (!app.ai_in_editor) return 1;
    char *what = ai_destructive(sql);
    if (!what) return 1;
    char msg[1024];
    snprintf(msg, sizeof msg,
             "The SQL Claude wrote contains: %s.\n\n"
             "This can't be undone. Check that it does what you asked before running it.",
             what);
    free(what);
#ifdef SQLC_AUTOTEST
    printf("[dialog] Run Claude's SQL?: %s\n", msg);
    return 0;
#else
    return sqlc_confirm_danger("Run statements that change data?", msg, "Run Anyway", 1);
#endif
}

static void on_run_menu(void) {
    char *sql = uiMultilineEntryText(app.editor);
    if (app.busy || !app.conn || confirm_ai_sql(sql ? sql : "")) {
        if (run_sql(sql ? sql : "")) { /* the user's own SQL: no longer browsing a table */
            free(app.browsing);
            app.browsing = NULL;
            app.ai_in_editor = 0;
        }
    }
    uiFreeText(sql);
}

static void on_run(uiButton *b, void *data) {
    (void)b; (void)data;
    on_run_menu();
}

/* ---------- Ask Claude ---------- */

static const char AI_KEY_MESSAGE[] =
    "Paste your Anthropic API key (from console.anthropic.com). It is stored in your "
    "login Keychain.\n\n"
    "When you ask Claude, sqlc sends your request, the server version and the current "
    "database's table and column names, types and keys to the Claude API. Row data is "
    "never sent.";

/* Prompts for the key and stores it. Returns the key (malloc'd) or NULL. */
static char *prompt_api_key(void) {
    char *key = sqlc_prompt_secret("Claude API key", AI_KEY_MESSAGE, "Save");
    if (!key) return NULL;
    char *p = key, *e;
    while (isspace((unsigned char)*p)) p++;
    e = p + strlen(p);
    while (e > p && isspace((unsigned char)e[-1])) *--e = '\0';
    if (!*p) { free(key); return NULL; }
    memmove(key, p, strlen(p) + 1);
    char err[256];
    if (keychain_set_item(KEYCHAIN_AI_SERVICE, KEYCHAIN_AI_ACCOUNT, key, err, sizeof err) != 0)
        show_error("Could not save the API key", err); /* still usable this time */
    else
        set_status("Claude API key saved in the Keychain.");
    return key;
}

/* The Keychain key, else $ANTHROPIC_API_KEY, else ask for one. */
static char *api_key(void) {
    char *key = keychain_get_item(KEYCHAIN_AI_SERVICE, KEYCHAIN_AI_ACCOUNT);
    if (key && *key) return key;
    free(key);
    const char *env = getenv("ANTHROPIC_API_KEY");
    if (env && *env) return strdup(env);
    return prompt_api_key();
}

static void on_set_api_key(uiMenuItem *m, uiWindow *w, void *data) {
    (void)m; (void)w; (void)data;
    char *key = prompt_api_key();
    if (key) { memset(key, 0, strlen(key)); free(key); }
}

static void on_remove_api_key(uiMenuItem *m, uiWindow *w, void *data) {
    (void)m; (void)w; (void)data;
    keychain_delete_item(KEYCHAIN_AI_SERVICE, KEYCHAIN_AI_ACCOUNT);
    set_status("Claude API key removed from the Keychain.");
}

static void on_ask_claude(uiButton *b, void *data) {
    (void)b; (void)data;
    if (app.busy) return;
    if (!app.conn) { set_status("Connect first, so Claude can see the database's tables."); return; }
    char *task = entry_dup(app.ai_task);
    const char *p = task;
    while (isspace((unsigned char)*p)) p++;
    if (!*p) {
        set_status("Describe the query you need in the Ask Claude field first.");
        free(task);
        return;
    }
    char *key = api_key();
    if (!key) { set_status("Ask Claude needs an API key (AI \xE2\x80\xBA Set Claude API Key\xE2\x80\xA6)."); free(task); return; }

    job *j = calloc(1, sizeof *j);
    if (!j) { free(task); free(key); return; }
    j->kind = JOB_AI;
    j->conn = app.conn;
    j->task = task;
    j->api_key = key;
    set_status("Asking Claude...");
    start_job(j);
}

static void load_tables_now(void) {
    if (app.busy) return;
    if (!app.conn) { set_status("Not connected - fill in the fields above and press Connect."); return; }
    job *j = calloc(1, sizeof *j);
    if (!j) return;
    j->kind = JOB_TABLES;
    j->conn = app.conn;
    j->load_tables = 1;
    j->hide_views = uiCheckboxChecked(app.hide_views);
    set_status("Loading tables...");
    start_job(j);
}

static void on_load_tables(uiButton *b, void *data) {
    (void)b; (void)data;
    load_tables_now();
}

/* Export a table on the worker thread (reusable from other entry points). */
static void start_export(const char *table, int is_view, export_format fmt,
                         const export_opts *opts, const char *path) {
    if (app.busy || !app.conn) return;
    job *j = calloc(1, sizeof *j);
    if (!j) return;
    j->kind = JOB_EXPORT;
    j->conn = app.conn;
    j->table = strdup(table);
    j->path = strdup(path);
    j->is_view = is_view;
    j->fmt = fmt;
    j->xopts = *opts;
    if (!j->table || !j->path) { job_free(j); return; }
    set_status("Exporting %s...", table);
    start_job(j);
}

static const char *table_at_row(int row, int *is_view_out);

/* The table selected in the sidebar, or NULL (pointer into tables_raw). */
static const char *selected_table(int *is_view_out) {
    if (!app.side_table || !app.side_data.nsets) return NULL;
    uiTableSelection *sel = uiTableGetSelection(app.side_table);
    int row = sel && sel->NumRows > 0 ? sel->Rows[0] : -1;
    uiFreeTableSelection(sel);
    return table_at_row(row, is_view_out);
}

/* Options dialog + save panel, then the export job. */
static void export_table_flow(const char *table, int view) {
    if (app.busy || !app.conn) return;
    char *t = strdup(table); /* the dialog runs a modal loop; keep our own copy */
    if (!t) return;
    export_format fmt;
    export_opts opts;
    char *path = NULL;
    if (sqlc_export_dialog(t, view, &fmt, &opts, &path)) start_export(t, view, fmt, &opts, path);
    free(path);
    free(t);
}

static void export_selected_table(void) {
    if (app.busy) return;
    if (!app.conn) { set_status("Not connected - fill in the fields above and press Connect."); return; }
    int view = 0;
    const char *name = selected_table(&view);
    if (!name) {
        uiMsgBoxError(app.win, "Export",
                      app.tables_loaded ? "Select a table in the sidebar first."
                                        : "Load the table list and select a table first.");
        return;
    }
    export_table_flow(name, view);
}

static void on_export(uiButton *b, void *data) {
    (void)b; (void)data;
    export_selected_table();
}

static void on_export_menu(uiMenuItem *m, uiWindow *w, void *data) {
    (void)m; (void)w; (void)data;
    export_selected_table();
}

static void on_hide_views(uiCheckbox *c, void *data) {
    (void)c; (void)data;
    /* Views are filtered on the server, so turning this off needs a reload
     * to bring them back. Filter what we have meanwhile (or if busy). */
    rebuild_sidebar();
    if (app.tables_loaded) load_tables_now();
}

/* "Tables only, load once" implies Hide views and replaces -A: lock both. */
static void sync_tables_once(int reload) {
    if (uiCheckboxChecked(app.tables_once)) {
        uiCheckboxSetChecked(app.no_rehash, 0);
        if (!uiCheckboxChecked(app.hide_views)) {
            uiCheckboxSetChecked(app.hide_views, 1);
            if (reload) on_hide_views(app.hide_views, NULL);
        }
        uiControlDisable(uiControl(app.hide_views));
        uiControlDisable(uiControl(app.no_rehash));
    } else {
        uiControlEnable(uiControl(app.hide_views));
        uiControlEnable(uiControl(app.no_rehash));
    }
}

static void on_tables_once(uiCheckbox *c, void *data) {
    (void)c; (void)data;
    sync_tables_once(1);
}

/* Double-click a table: SELECT * FROM it. */
static void on_table_double_click(uiTable *t, int row, void *data) {
    (void)t; (void)data;
    if (row < 0 || !app.side_data.nsets || (size_t)row >= app.side_data.sets[0].nrows) return;
    const char *name = db_cell(&app.tables_raw.sets[0], app.side_index[row], 0);
    if (!name) return;
    char *sql = browse_sql(name);
    if (!sql) return;
    uiMultilineEntrySetText(app.editor, sql);
    app.ai_in_editor = 0;
    if (run_sql(sql)) {
        free(app.browsing);
        app.browsing = strdup(name);
    }
    free(sql);
}

/* ---------- sidebar context menu ---------- */

/* Name and kind of the table on a visible sidebar row (pointer into tables_raw). */
static const char *table_at_row(int row, int *is_view_out) {
    if (row < 0 || !app.side_data.nsets || (size_t)row >= app.side_data.sets[0].nrows) return NULL;
    size_t r = app.side_index[row];
    if (is_view_out) *is_view_out = is_view(db_cell(&app.tables_raw.sets[0], r, 1));
    return db_cell(&app.tables_raw.sets[0], r, 0);
}

/* Rename, truncate or drop a table on the worker thread. */
static void start_alter(alter_kind kind, const char *table, int view, const char *new_name) {
    if (app.busy || !app.conn) return;
    char *qt = db_quote_ident(table);
    char *qn = new_name ? db_quote_ident(new_name) : NULL;
    job *j = calloc(1, sizeof *j);
    size_t n = (qt ? strlen(qt) : 0) + (qn ? strlen(qn) : 0) + 32;
    char *sql = malloc(n);
    if (!qt || (new_name && !qn) || !j || !sql) { free(qt); free(qn); free(j); free(sql); return; }
    switch (kind) {
    case ALTER_RENAME:   snprintf(sql, n, "RENAME TABLE %s TO %s", qt, qn); break;
    case ALTER_TRUNCATE: snprintf(sql, n, "TRUNCATE TABLE %s", qt); break;
    case ALTER_DROP:     snprintf(sql, n, view ? "DROP VIEW %s" : "DROP TABLE %s", qt); break;
    }
    free(qt);
    free(qn);
    j->kind = JOB_ALTER;
    j->conn = app.conn;
    j->sql = sql;
    j->alter = kind;
    j->table = strdup(table);
    j->new_name = new_name ? strdup(new_name) : NULL;
    j->is_view = view;
    j->load_tables = app.tables_loaded; /* show the change in the sidebar */
    j->hide_views = uiCheckboxChecked(app.hide_views);
    set_status("Running %s...", sql);
    start_job(j);
}

static unsigned on_table_menu_open(int row, void *data) {
    (void)data;
    int view = 0;
    const char *name = table_at_row(row, &view);
    free(app.menu_table);
    app.menu_table = name ? strdup(name) : NULL;
    app.menu_is_view = view;
    if (!app.menu_table) return 0;
    unsigned items = TABLE_MENU_BIT(TABLE_MENU_COPY_NAME);
    if (app.conn && !app.busy) {
        items |= TABLE_MENU_BIT(TABLE_MENU_RENAME) | TABLE_MENU_BIT(TABLE_MENU_DELETE) |
                 TABLE_MENU_BIT(TABLE_MENU_EXPORT);
        if (!view) items |= TABLE_MENU_BIT(TABLE_MENU_TRUNCATE); /* not for views */
    }
    return items;
}

static void export_table_flow(const char *table, int view);

static void on_table_menu_action(table_menu_item item, int row, void *data) {
    (void)row; (void)data;
    if (!app.menu_table) return;
    /* the name as it was when the menu opened, in case the list changed since */
    char *name = strdup(app.menu_table);
    int view = app.menu_is_view;
    if (!name) return;
    const char *what = view ? "view" : "table";
    char title[600], msg[600];

    switch (item) {
    case TABLE_MENU_COPY_NAME:
        sqlc_copy_to_clipboard(name);
        set_status("Copied \"%s\" to the clipboard", name);
        break;
    case TABLE_MENU_RENAME: {
        snprintf(title, sizeof title, "Rename %s `%s`", what, name);
        char *new_name = sqlc_prompt(title, "New name:", name, "Rename");
        if (new_name && new_name[0] && strcmp(new_name, name) != 0)
            start_alter(ALTER_RENAME, name, view, new_name);
        free(new_name);
        break;
    }
    case TABLE_MENU_TRUNCATE:
        if (view) break;
        snprintf(title, sizeof title, "Truncate table `%s`?", name);
        if (sqlc_confirm_danger(title, "All rows will be deleted. This cannot be undone.", "Truncate", 0))
            start_alter(ALTER_TRUNCATE, name, view, NULL);
        break;
    case TABLE_MENU_DELETE:
        snprintf(title, sizeof title, "Drop %s `%s`?", what, name);
        snprintf(msg, sizeof msg, view ? "The view will be permanently deleted."
                                       : "The table and all its data will be permanently deleted.");
        if (sqlc_confirm_danger(title, msg, "Delete", 1))
            start_alter(ALTER_DROP, name, view, NULL);
        break;
    case TABLE_MENU_EXPORT:
        export_table_flow(name, view);
        break;
    default:
        break;
    }
    free(name);
}

/* ---------- saved connections ---------- */

static int save_profiles_file(void) {
    if (app.profiles_file && profiles_save(app.profiles_file, &app.profiles) == 0) return 0;
    char msg[1200];
    snprintf(msg, sizeof msg, "Could not write %s: %s",
             app.profiles_file ? app.profiles_file : "(no config path)", strerror(errno));
    uiMsgBoxError(app.win, "Could not save connections", msg);
    return -1;
}

/* Refill the dropdown and select the profile called name (or "New connection"). */
static void fill_profile_box(const char *name) {
    uiComboboxClear(app.profile_box);
    uiComboboxAppend(app.profile_box, "New connection");
    int sel = 0;
    for (size_t i = 0; i < app.profiles.n; i++) {
        uiComboboxAppend(app.profile_box, app.profiles.items[i].name);
        if (name && strcmp(app.profiles.items[i].name, name) == 0) sel = (int)i + 1;
    }
    uiComboboxSetSelected(app.profile_box, sel);
}

/* Fill the connection fields from the selected dropdown item. */
static void apply_selected_profile(void) {
    int sel = uiComboboxSelected(app.profile_box);
    if (sel <= 0 || (size_t)sel > app.profiles.n) {
        uiEntrySetText(app.profile_name, "");
        return;
    }
    const profile *p = &app.profiles.items[sel - 1];
    char port[16];
    snprintf(port, sizeof port, "%u", p->port ? p->port : 3306);
    uiEntrySetText(app.profile_name, p->name);
    uiEntrySetText(app.host, p->host);
    uiEntrySetText(app.port, port);
    uiEntrySetText(app.user, p->user);
    uiEntrySetText(app.db, p->database);
    uiCheckboxSetChecked(app.remember_pw, p->remember_password);
    uiCheckboxSetChecked(app.hide_views, p->hide_views);
    uiCheckboxSetChecked(app.no_rehash, p->no_auto_rehash);
    uiCheckboxSetChecked(app.tables_once, p->tables_once);
    sync_tables_once(0);
    rebuild_sidebar();

    char *pw = p->remember_password ? keychain_get(p->name) : NULL;
    uiEntrySetText(app.pass, pw ? pw : "");
    if (pw) { memset(pw, 0, strlen(pw)); free(pw); }
}

static void on_profile_selected(uiCombobox *c, void *data) {
    (void)c; (void)data;
    apply_selected_profile();
}

static char *trimmed_entry(uiEntry *e) {
    char *s = entry_dup(e);
    if (!s) return NULL;
    char *b = s;
    while (isspace((unsigned char)*b)) b++;
    char *end = b + strlen(b);
    while (end > b && isspace((unsigned char)end[-1])) *--end = '\0';
    memmove(s, b, strlen(b) + 1);
    return s;
}

static void on_save_profile(uiButton *b, void *data) {
    (void)b; (void)data;
    char *name = trimmed_entry(app.profile_name);
    if (!name) return;
    if (!profile_name_valid(name)) {
        uiMsgBoxError(app.win, "Can't save connection",
                      "Enter a name for this connection (no line breaks, '[' or ']').");
        free(name);
        return;
    }

    char *port = entry_dup(app.port);
    unsigned long portn = port ? strtoul(port, NULL, 10) : 0;
    free(port);
    profile p = {
        .name = name,
        .host = entry_dup(app.host),
        .port = portn > 0 && portn <= 65535 ? (unsigned int)portn : 3306,
        .user = entry_dup(app.user),
        .database = entry_dup(app.db),
        .remember_password = uiCheckboxChecked(app.remember_pw),
        .hide_views = uiCheckboxChecked(app.hide_views),
        .no_auto_rehash = uiCheckboxChecked(app.no_rehash),
        .tables_once = uiCheckboxChecked(app.tables_once),
    };
    int existed = profiles_find(&app.profiles, name) != NULL;

    if (p.remember_password) {
        char *pw = entry_dup(app.pass);
        char err[512] = "";
        if (pw && keychain_set(name, pw, err, sizeof err) != 0) {
            char msg[700];
            snprintf(msg, sizeof msg,
                     "The password could not be stored in the Keychain (%s). "
                     "The connection is saved without it.", err);
            uiMsgBoxError(app.win, "Keychain", msg);
            p.remember_password = 0;
        }
        if (pw) { memset(pw, 0, strlen(pw)); free(pw); }
    } else {
        keychain_delete(name);
    }

    if (profiles_put(&app.profiles, &p) < 0) {
        set_status("Out of memory");
    } else {
        profiles_set_last(&app.profiles, name);
        if (save_profiles_file() == 0) {
            fill_profile_box(name);
            uiCheckboxSetChecked(app.remember_pw, p.remember_password);
            set_status("%s connection \"%s\".", existed ? "Updated" : "Saved", name);
        }
    }
    free(p.name); free(p.host); free(p.user); free(p.database);
}

static void on_delete_profile(uiButton *b, void *data) {
    (void)b; (void)data;
    int sel = uiComboboxSelected(app.profile_box);
    if (sel <= 0 || (size_t)sel > app.profiles.n) {
        set_status("Pick a saved connection to delete.");
        return;
    }
    char *name = strdup(app.profiles.items[sel - 1].name);
    if (!name) return;
    char msg[300];
    snprintf(msg, sizeof msg, "Delete the saved connection \"%s\"? "
             "Its saved password is removed from the Keychain too.", name);
#ifndef SQLC_AUTOTEST
    if (!sqlc_confirm("Delete saved connection?", msg, "Delete")) { free(name); return; }
#endif
    keychain_delete(name);
    profiles_remove(&app.profiles, name);
    if (save_profiles_file() == 0) set_status("Deleted connection \"%s\".", name);
    fill_profile_box(NULL);
    uiEntrySetText(app.profile_name, "");
    free(name);
}

/* After a successful connect: preselect this profile on the next launch. */
static void remember_last_profile(const char *name) {
    if (!name || !profiles_find(&app.profiles, name)) return;
    if (app.profiles.last && strcmp(app.profiles.last, name) == 0) return;
    if (profiles_set_last(&app.profiles, name) == 0) save_profiles_file();
}

static void load_profiles(void) {
    app.profiles_file = profiles_path();
    if (app.profiles_file && profiles_load(app.profiles_file, &app.profiles) != 0)
        set_status("Could not read saved connections from %s: %s",
                   app.profiles_file, strerror(errno));
    fill_profile_box(app.profiles.last);
    apply_selected_profile();
}

#ifdef SQLC_AUTOTEST
/*
 * Scripted smoke test (make gui-test): drives the real callbacks against
 * the database in $SQLC_TEST_DB, prints what the UI would show, then quits.
 */
static int autotest_step;

static void autotest_print(const char *what) {
    char *st = uiLabelText(app.status);
    printf("[%s] status: %s\n", what, st);
    uiFreeText(st);
    printf("[%s] result sets: %zu, grids: %zu, error page: %s\n", what,
           app.shown.nsets, app.nmodels, app.shown.error ? "yes" : "no");
    const db_set *s = app.side_data.nsets ? &app.side_data.sets[0] : NULL;
    printf("[%s] sidebar:", what);
    for (size_t r = 0; s && r < s->nrows; r++) printf(" [%s]", db_cell(s, r, 0));
    printf("\n");
    printf("[%s] table list loaded: %s, rows from server: %zu\n", what,
           app.tables_loaded ? "yes" : "no",
           app.tables_raw.nsets ? app.tables_raw.sets[0].nrows : 0);
    fflush(stdout);
}

/* Visible sidebar row of a table, or -1. */
static int autotest_row(const char *name) {
    for (size_t r = 0; app.side_data.nsets && r < app.side_data.sets[0].nrows; r++) {
        const char *t = table_at_row((int)r, NULL);
        if (t && strcmp(t, name) == 0) return (int)r;
    }
    return -1;
}

static void autotest_next(void) {
    switch (autotest_step++) {
    case 0:
        autotest_print("connect");
        uiMultilineEntrySetText(app.editor,
            "SELECT * FROM customers; UPDATE orders SET total = total; SELECT nope;");
        on_run_menu();
        break;
    case 1:
        autotest_print("multi");
        uiCheckboxSetChecked(app.hide_views, 1);
        on_hide_views(app.hide_views, NULL); /* reloads without views */
        break;
    case 2:
        autotest_print("hide views");
        on_table_double_click(app.side_table, 0, NULL);
        break;
    case 3:
        autotest_print("double-click");
        uiMultilineEntrySetText(app.editor, "SHOW TABLES;");
        on_run_menu();
        break;
    case 4:
        autotest_print("show tables, views hidden");
        uiMultilineEntrySetText(app.editor, "SELECT seq FROM seq_1_to_20000;");
        on_run_menu();
        break;
    case 5:
        autotest_print("20000 rows");
        printf("[20000 rows] grid rows kept: %zu, truncated: %d\n",
               app.shown.sets[0].nrows, app.shown.sets[0].truncated);
        /* save the current connection as a profile, then read the file back */
        uiEntrySetText(app.profile_name, "autotest");
        uiCheckboxSetChecked(app.remember_pw, 0);
        uiCheckboxSetChecked(app.no_rehash, 1);
        on_save_profile(NULL, NULL);
        {
            profile_list back;
            int rc = profiles_load(app.profiles_file, &back);
            profile *p = rc == 0 ? profiles_find(&back, "autotest") : NULL;
            printf("[profiles] saved: %s, db: %s, hide_views: %d, no_auto_rehash: %d, "
                   "last: %s, dropdown items: %d\n",
                   p ? "yes" : "no", p ? p->database : "-", p ? p->hide_views : -1,
                   p ? p->no_auto_rehash : -1,
                   back.last ? back.last : "-", uiComboboxNumItems(app.profile_box));
            profiles_free(&back);
        }
        on_delete_profile(NULL, NULL);
        printf("[profiles] after delete: %zu saved, dropdown items: %d\n",
               app.profiles.n, uiComboboxNumItems(app.profile_box));
        /* sidebar divider: min width, and the table column follows it */
        sqlc_split_set_sidebar_width(app.split, 400);
        printf("[sidebar] width 400 -> %d, column %d\n",
               sqlc_split_sidebar_width(app.split), uiTableColumnWidth(app.side_table, 0));
        sqlc_split_set_sidebar_width(app.split, 50);
        printf("[sidebar] width 50 -> %d (min 120), column %d\n",
               sqlc_split_sidebar_width(app.split), uiTableColumnWidth(app.side_table, 0));
        sqlc_split_set_sidebar_width(app.split, 5000);
        printf("[sidebar] width 5000 -> %d (window leaves room for the editor)\n",
               sqlc_split_sidebar_width(app.split));
        /* export the selected sidebar table (the dialog is skipped) */
        {
            int r0 = 0;
            uiTableSelection sel = { .NumRows = 1, .Rows = &r0 };
            uiTableSetSelection(app.side_table, &sel);
            int view = -1;
            const char *t = selected_table(&view);
            printf("[export] selected: %s (view: %d)\n", t ? t : "-", view);
            export_opts o = export_default_opts();
            if (t) start_export(t, view, EXPORT_CSV, &o, "build/autotest-export.csv");
        }
        break;
    case 6:
        autotest_print("export");
        {
            FILE *f = fopen("build/autotest-export.csv", "r");
            int lines = 0;
            for (int c; f && (c = getc(f)) != EOF;) lines += c == '\n';
            if (f) fclose(f);
            printf("[export] file lines: %d\n", lines);
            remove("build/autotest-export.csv");
        }
        /* reconnect with -A (still checked): no table list until asked */
        on_connect(app.connect_btn, NULL);
        break;
    case 7:
        autotest_print("connect -A");
        load_tables_now();
        break;
    case 8:
        autotest_print("load tables");
        /* context menu: work on a scratch table, with -A off so the list follows */
        uiCheckboxSetChecked(app.no_rehash, 0);
        run_sql("DROP TABLE IF EXISTS ctx_tmp; CREATE TABLE ctx_tmp (id INT); "
                "INSERT INTO ctx_tmp VALUES (1), (2);");
        break;
    case 9: {
        int row = autotest_row("ctx_tmp");
        char menu[512];
        sqlc_table_menu_describe(app.side_table, row, menu, sizeof menu);
        printf("[menu] table: %s\n", menu);
        printf("[menu] selected after right-click: %s\n", selected_table(NULL) ? selected_table(NULL) : "-");
        sqlc_table_menu_describe(app.side_table, 999, menu, sizeof menu);
        printf("[menu] empty space: [%s]\n", menu);
        /* Copy table name (keeping the user's clipboard) */
        char *saved = sqlc_clipboard_text();
        sqlc_table_menu_describe(app.side_table, row, menu, sizeof menu);
        on_table_menu_action(TABLE_MENU_COPY_NAME, row, NULL);
        char *clip = sqlc_clipboard_text();
        printf("[menu] clipboard: %s\n", clip ? clip : "-");
        free(clip);
        if (saved) sqlc_copy_to_clipboard(saved);
        free(saved);
        on_table_double_click(app.side_table, row, NULL); /* browse it */
        break;
    }
    case 10:
        autotest_print("browse ctx_tmp");
        start_alter(ALTER_RENAME, "ctx_tmp", 0, "ctx `odd` name");
        break;
    case 11: {
        autotest_print("rename");
        char *ed = uiMultilineEntryText(app.editor);
        printf("[rename] editor: %s, browsing: %s\n", ed, app.browsing ? app.browsing : "-");
        uiFreeText(ed);
        start_alter(ALTER_TRUNCATE, "ctx `odd` name", 0, NULL);
        break;
    }
    case 12:
        autotest_print("truncate");
        run_sql("SELECT COUNT(*) FROM `ctx ``odd`` name`");
        break;
    case 13:
        printf("[truncate] rows left: %s\n", db_cell(&app.shown.sets[0], 0, 0));
        on_table_double_click(app.side_table, autotest_row("ctx `odd` name"), NULL);
        break;
    case 14:
        start_alter(ALTER_DROP, "ctx `odd` name", 0, NULL);
        break;
    case 15:
        autotest_print("drop");
        printf("[drop] browsing: %s\n", app.browsing ? app.browsing : "-");
        start_alter(ALTER_DROP, "ctx `odd` name", 0, NULL); /* again: server error */
        break;
    case 16:
        autotest_print("drop again");
        /* views: Truncate is disabled */
        uiCheckboxSetChecked(app.hide_views, 0);
        on_hide_views(app.hide_views, NULL);
        break;
    case 17: {
        char menu[512];
        sqlc_table_menu_describe(app.side_table, autotest_row("customer_totals"), menu, sizeof menu);
        printf("[menu] view: %s\n", menu);
        /* "Tables only, load once": no views on connect, no refresh after queries */
        uiCheckboxSetChecked(app.tables_once, 1);
        on_tables_once(app.tables_once, NULL);
        printf("[tables once] hide views: %d, -A: %d\n",
               uiCheckboxChecked(app.hide_views), uiCheckboxChecked(app.no_rehash));
        break; /* turning on Hide views reloads the list */
    }
    case 18:
        on_connect(app.connect_btn, NULL);
        break;
    case 19:
        autotest_print("connect tables once");
        run_sql("DROP TABLE IF EXISTS once_tmp; CREATE TABLE once_tmp (id INT);");
        break;
    case 20:
        autotest_print("query, tables once");
        printf("[tables once] once_tmp in sidebar: %s\n", autotest_row("once_tmp") >= 0 ? "yes" : "no");
        load_tables_now();
        break;
    case 21:
        printf("[tables once] after refresh, once_tmp in sidebar: %s\n",
               autotest_row("once_tmp") >= 0 ? "yes" : "no");
        uiCheckboxSetChecked(app.tables_once, 0);
        on_tables_once(app.tables_once, NULL);
        run_sql("DROP TABLE once_tmp;");
        break;
    case 22: {
        /* Claude's SQL (no network here: as if Ask Claude had filled the
         * editor): a destructive statement asks first; the test cancels. */
        ai_result r = { .explanation = strdup("Deletes every customer."),
                        .sql = strdup("DELETE FROM customers;") };
        char *text = ai_format("remove all customers", &r);
        uiMultilineEntrySetText(app.editor, text);
        app.ai_in_editor = 1;
        free(text);
        ai_result_free(&r);
        on_run_menu();
        printf("[ai] destructive SQL ran: %s\n", app.busy ? "yes" : "no");
        /* a read-only query from Claude runs without asking */
        uiMultilineEntrySetText(app.editor, "-- Request: count customers\nSELECT COUNT(*) FROM customers;\n");
        on_run_menu();
        printf("[ai] read-only SQL ran: %s, still marked as Claude's: %d\n",
               app.busy ? "yes" : "no", app.ai_in_editor);
        break;
    }
    case 23:
        autotest_print("ai read-only");
        if (on_should_quit(NULL)) uiQuit();
        break;
    }
}

static void autotest_start(void *data) {
    (void)data;
    const char *db = getenv("SQLC_TEST_DB");
    uiEntrySetText(app.db, db ? db : "");
    on_connect(app.connect_btn, NULL);
}
#endif

static int on_closing(uiWindow *w, void *data) {
    (void)w; (void)data;
    uiQuit();
    return 1;
}

static int on_should_quit(void *data) {
    (void)data;
    uiControlDestroy(uiControl(app.win));
    return 1;
}

/* ---------- layout ---------- */

static uiBox *labeled(const char *label, uiControl *c) {
    uiBox *b = uiNewVerticalBox();
    uiBoxAppend(b, uiControl(uiNewLabel(label)), 0);
    uiBoxAppend(b, c, 0);
    return b;
}

static void build_window(void) {
    uiMenu *file = uiNewMenu("File");
    uiMenuItemOnClicked(uiMenuAppendItem(file, "Export Table\xE2\x80\xA6"), on_export_menu, NULL);
    uiMenuAppendSeparator(file);
    uiMenuAppendQuitItem(file);
    uiMenu *ai = uiNewMenu("AI");
    uiMenuItemOnClicked(uiMenuAppendItem(ai, "Set Claude API Key\xE2\x80\xA6"), on_set_api_key, NULL);
    uiMenuItemOnClicked(uiMenuAppendItem(ai, "Remove Claude API Key"), on_remove_api_key, NULL);
    uiOnShouldQuit(on_should_quit, NULL);

    app.win = uiNewWindow("sqlc", 1100, 720, 1);
    uiWindowSetMargined(app.win, 1);
    uiWindowOnClosing(app.win, on_closing, NULL);

    uiBox *root = uiNewVerticalBox();
    uiBoxSetPadded(root, 1);
    uiWindowSetChild(app.win, uiControl(root));

    /* saved connections */
    uiBox *saved = uiNewHorizontalBox();
    uiBoxSetPadded(saved, 1);
    app.profile_box = uiNewCombobox();
    uiComboboxOnSelected(app.profile_box, on_profile_selected, NULL);
    app.profile_name = uiNewEntry();
    app.remember_pw = uiNewCheckbox("Remember password");
    uiCheckboxSetChecked(app.remember_pw, 1);
    uiButton *save_btn = uiNewButton("Save");
    uiButtonOnClicked(save_btn, on_save_profile, NULL);
    uiButton *delete_btn = uiNewButton("Delete");
    uiButtonOnClicked(delete_btn, on_delete_profile, NULL);
    uiBoxAppend(saved, uiControl(labeled("Saved connections", uiControl(app.profile_box))), 1);
    uiBoxAppend(saved, uiControl(labeled("Name", uiControl(app.profile_name))), 1);
    uiBoxAppend(saved, uiControl(labeled(" ", uiControl(app.remember_pw))), 0);
    uiBoxAppend(saved, uiControl(labeled(" ", uiControl(save_btn))), 0);
    uiBoxAppend(saved, uiControl(labeled(" ", uiControl(delete_btn))), 0);
    uiBoxAppend(root, uiControl(saved), 0);

    /* connection bar */
    uiBox *bar = uiNewHorizontalBox();
    uiBoxSetPadded(bar, 1);
    app.host = uiNewEntry();  uiEntrySetText(app.host, "localhost");
    app.port = uiNewEntry();  uiEntrySetText(app.port, "3306");
    app.user = uiNewEntry();
    app.pass = uiNewPasswordEntry();
    app.db   = uiNewEntry();
    const char *login = getenv("USER");
    if (login) uiEntrySetText(app.user, login);
    app.connect_btn = uiNewButton("Connect");
    uiButtonOnClicked(app.connect_btn, on_connect, NULL);
    /* connection options, settable before connecting (saved with profiles) */
    app.hide_views = uiNewCheckbox("Hide views");
    uiCheckboxOnToggled(app.hide_views, on_hide_views, NULL);
    app.no_rehash = uiNewCheckbox("No auto-rehash (-A)");
    app.tables_once = uiNewCheckbox("Tables only, load once");
    uiCheckboxOnToggled(app.tables_once, on_tables_once, NULL);

    uiBoxAppend(bar, uiControl(labeled("Host", uiControl(app.host))), 1);
    uiBoxAppend(bar, uiControl(labeled("Port", uiControl(app.port))), 0);
    uiBoxAppend(bar, uiControl(labeled("User", uiControl(app.user))), 1);
    uiBoxAppend(bar, uiControl(labeled("Password", uiControl(app.pass))), 1);
    uiBoxAppend(bar, uiControl(labeled("Database", uiControl(app.db))), 1);
    uiBoxAppend(bar, uiControl(labeled(" ", uiControl(app.hide_views))), 0);
    uiBoxAppend(bar, uiControl(labeled(" ", uiControl(app.no_rehash))), 0);
    uiBoxAppend(bar, uiControl(labeled(" ", uiControl(app.tables_once))), 0);
    uiBoxAppend(bar, uiControl(labeled(" ", uiControl(app.connect_btn))), 0);
    uiBoxAppend(root, uiControl(bar), 0);
    uiBoxAppend(root, uiControl(uiNewHorizontalSeparator()), 0);

    /* main area: sidebar | editor + results, with a draggable divider */
    app.side_box = uiNewVerticalBox();
    uiBoxSetPadded(app.side_box, 1);
    app.load_tables_btn = uiNewButton("Load tables");
    uiButtonOnClicked(app.load_tables_btn, on_load_tables, NULL);
    uiBoxAppend(app.side_box, uiControl(app.load_tables_btn), 0);
    app.export_btn = uiNewButton("Export\xE2\x80\xA6");
    uiButtonOnClicked(app.export_btn, on_export, NULL);
    uiBoxAppend(app.side_box, uiControl(app.export_btn), 0);

    uiBox *right = uiNewVerticalBox();
    uiBoxSetPadded(right, 1);

    app.split = sqlc_new_sidebar_split(uiControl(app.side_box), uiControl(right),
                                       220, 120, "SidebarWidth");
    uiBoxAppend(root, app.split, 1);

    /* Ask Claude: describe it, get SQL (with an explanation) in the editor */
    uiBox *ai_row = uiNewHorizontalBox();
    uiBoxSetPadded(ai_row, 1);
    app.ai_task = uiNewEntry();
    app.ai_btn = uiNewButton("Ask Claude");
    uiButtonOnClicked(app.ai_btn, on_ask_claude, NULL);
    uiBoxAppend(ai_row, uiControl(app.ai_task), 1);
    uiBoxAppend(ai_row, uiControl(app.ai_btn), 0);
    uiBoxAppend(right, uiControl(labeled("Ask Claude - describe the query you need "
                                         "(e.g. top 10 customers by total order value last month)",
                                         uiControl(ai_row))), 0);

    app.editor = uiNewNonWrappingMultilineEntry();
    uiMultilineEntrySetText(app.editor, "SHOW TABLES;");
    uiBoxAppend(right, uiControl(app.editor), 1);

    uiBox *run_row = uiNewHorizontalBox();
    uiBoxSetPadded(run_row, 1);
    app.run_btn = uiNewButton("Run (\xE2\x8C\x98\xE2\x86\xA9)"); /* "Run (⌘↩)" */
    uiButtonOnClicked(app.run_btn, on_run, NULL);
    uiBoxAppend(run_row, uiControl(app.run_btn), 0);
    uiBoxAppend(run_row, uiControl(uiNewLabel("Double-click a table to browse it.")), 0);
    uiBoxAppend(right, uiControl(run_row), 0);

    app.results_box = uiNewVerticalBox();
    uiBoxAppend(right, uiControl(app.results_box), 1);

    app.status = uiNewLabel("Not connected.");
    uiBoxAppend(root, uiControl(app.status), 0);

    rebuild_sidebar();
    load_profiles();
}

int main(void) {
    uiInitOptions o = { 0 };
    const char *err = uiInit(&o);
    if (err) {
        fprintf(stderr, "error initializing libui: %s\n", err);
        uiFreeInitError(err);
        return 1;
    }
    mysql_library_init(0, NULL, NULL); /* before any worker thread */
    curl_global_init(CURL_GLOBAL_DEFAULT);

    build_window();
    sqlc_install_menus(on_run_menu);
    uiControlShow(uiControl(app.win));
#ifdef SQLC_AUTOTEST
    uiQueueMain(autotest_start, NULL);
#endif
    uiMain();

    /* The window (and its grids) is gone; free what's left. */
    if (!app.busy) {
        for (size_t i = 0; i < app.nmodels; i++) free_set_model(app.models[i]);
        free(app.models);
        free_set_model(app.side_model);
        db_outcome_free(&app.shown);
        db_outcome_free(&app.side_data);
        db_outcome_free(&app.tables_raw);
        free(app.side_index);
        free(app.menu_table);
        free(app.browsing);
        profiles_free(&app.profiles);
        free(app.profiles_file);
        if (app.conn) mysql_close(app.conn);
        uiUninit();
    }
    mysql_library_end();
    curl_global_cleanup();
    return 0;
}
