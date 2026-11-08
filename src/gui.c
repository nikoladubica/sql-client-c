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

#include "db.h"
#include "keychain.h"
#include "macos_menu.h"
#include "splitview.h"
#include "profiles.h"

#define GUI_MAX_ROWS 10000 /* rows kept per result set in the grid */

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
    uiButton *connect_btn, *run_btn;
    uiCheckbox *hide_views;
    uiMultilineEntry *editor;
    uiLabel *status;

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

static void set_busy(int busy) {
    app.busy = busy;
    if (busy) {
        uiControlDisable(uiControl(app.connect_btn));
        uiControlDisable(uiControl(app.run_btn));
    } else {
        uiControlEnable(uiControl(app.connect_btn));
        uiControlEnable(uiControl(app.run_btn));
    }
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
    uiBoxAppend(app.side_box, uiControl(app.side_table), 1);
}

/* ---------- worker thread ---------- */

typedef enum { JOB_CONNECT, JOB_QUERY } job_kind;

typedef struct {
    job_kind kind;
    MYSQL *conn;              /* existing connection, or the new one */
    char *host, *user, *pass, *dbname;
    unsigned int port;
    char *sql;
    db_exec_opts eo;
    char *profile;            /* saved connection used to connect, if any */

    /* results, filled by the worker */
    int ok;
    char *error;              /* connect error */
    db_outcome out;           /* query results */
    db_outcome tables;        /* refreshed table list */
    char *current_db;
    double seconds;
} job;

static void job_free(job *j) {
    free(j->host); free(j->user); free(j->dbname); free(j->sql); free(j->error);
    if (j->pass) { memset(j->pass, 0, strlen(j->pass)); free(j->pass); }
    free(j->current_db);
    free(j->profile);
    db_outcome_free(&j->out);
    db_outcome_free(&j->tables);
    free(j);
}

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Table list + current database; runs on the worker thread. */
static void load_tables(job *j) {
    db_exec_opts eo = { 0 };
    db_execute(j->conn,
               "SELECT TABLE_NAME, TABLE_TYPE FROM information_schema.TABLES "
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
    } else {
        j->ok = db_execute(j->conn, j->sql, &j->eo, &j->out) == 0;
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
        set_status("Connected to %s (server %s)%s%s",
                   mysql_get_host_info(app.conn), mysql_get_server_info(app.conn),
                   j->current_db ? " - database: " : "",
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
    db_outcome_free(&app.tables_raw);
    app.tables_raw = j->tables;
    memset(&j->tables, 0, sizeof j->tables);
    rebuild_sidebar();

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
    set_status("Connecting to %s...", j->host);
    start_job(j);
}

static void run_sql(const char *sql) {
    if (app.busy) return;
    if (!app.conn) { set_status("Not connected - fill in the fields above and press Connect."); return; }

    const char *p = sql;
    while (isspace((unsigned char)*p)) p++;
    if (!*p) { set_status("Nothing to run."); return; }

    job *j = calloc(1, sizeof *j);
    if (!j) return;
    j->kind = JOB_QUERY;
    j->conn = app.conn;
    j->sql = strdup(sql);
    j->eo.hide_views = uiCheckboxChecked(app.hide_views);
    j->eo.max_rows = GUI_MAX_ROWS;
    set_status("Running...");
    start_job(j);
}

static void on_run_menu(void) {
    char *sql = uiMultilineEntryText(app.editor);
    run_sql(sql ? sql : "");
    uiFreeText(sql);
}

static void on_run(uiButton *b, void *data) {
    (void)b; (void)data;
    on_run_menu();
}

static void on_hide_views(uiCheckbox *c, void *data) {
    (void)c; (void)data;
    rebuild_sidebar();
}

/* Double-click a table: SELECT * FROM it. */
static void on_table_double_click(uiTable *t, int row, void *data) {
    (void)t; (void)data;
    if (row < 0 || !app.side_data.nsets || (size_t)row >= app.side_data.sets[0].nrows) return;
    const char *name = db_cell(&app.tables_raw.sets[0], app.side_index[row], 0);
    if (!name) return;

    /* Quote as `name`, doubling any backticks inside. */
    static const char head[] = "SELECT * FROM `", tail[] = "` LIMIT 1000;";
    size_t cap = sizeof head + strlen(name) * 2 + sizeof tail;
    char *sql = malloc(cap);
    if (!sql) return;
    char *o = sql;
    memcpy(o, head, sizeof head - 1); o += sizeof head - 1;
    for (const char *c = name; *c; c++) { if (*c == '`') *o++ = '`'; *o++ = *c; }
    memcpy(o, tail, sizeof tail); /* includes the terminating NUL */

    uiMultilineEntrySetText(app.editor, sql);
    run_sql(sql);
    free(sql);
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
    fflush(stdout);
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
        on_hide_views(app.hide_views, NULL);
        autotest_print("hide views");
        on_table_double_click(app.side_table, 0, NULL);
        break;
    case 2:
        autotest_print("double-click");
        uiMultilineEntrySetText(app.editor, "SHOW TABLES;");
        on_run_menu();
        break;
    case 3:
        autotest_print("show tables, views hidden");
        uiMultilineEntrySetText(app.editor, "SELECT seq FROM seq_1_to_20000;");
        on_run_menu();
        break;
    case 4:
        autotest_print("20000 rows");
        printf("[20000 rows] grid rows kept: %zu, truncated: %d\n",
               app.shown.sets[0].nrows, app.shown.sets[0].truncated);
        /* save the current connection as a profile, then read the file back */
        uiEntrySetText(app.profile_name, "autotest");
        uiCheckboxSetChecked(app.remember_pw, 0);
        on_save_profile(NULL, NULL);
        {
            profile_list back;
            int rc = profiles_load(app.profiles_file, &back);
            profile *p = rc == 0 ? profiles_find(&back, "autotest") : NULL;
            printf("[profiles] saved: %s, db: %s, last: %s, dropdown items: %d\n",
                   p ? "yes" : "no", p ? p->database : "-",
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
    uiMenuAppendQuitItem(file);
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

    uiBoxAppend(bar, uiControl(labeled("Host", uiControl(app.host))), 1);
    uiBoxAppend(bar, uiControl(labeled("Port", uiControl(app.port))), 0);
    uiBoxAppend(bar, uiControl(labeled("User", uiControl(app.user))), 1);
    uiBoxAppend(bar, uiControl(labeled("Password", uiControl(app.pass))), 1);
    uiBoxAppend(bar, uiControl(labeled("Database", uiControl(app.db))), 1);
    uiBoxAppend(bar, uiControl(labeled(" ", uiControl(app.connect_btn))), 0);
    uiBoxAppend(root, uiControl(bar), 0);
    uiBoxAppend(root, uiControl(uiNewHorizontalSeparator()), 0);

    /* main area: sidebar | editor + results, with a draggable divider */
    app.side_box = uiNewVerticalBox();
    uiBoxSetPadded(app.side_box, 1);
    app.hide_views = uiNewCheckbox("Hide views");
    uiCheckboxOnToggled(app.hide_views, on_hide_views, NULL);
    uiBoxAppend(app.side_box, uiControl(app.hide_views), 0);

    uiBox *right = uiNewVerticalBox();
    uiBoxSetPadded(right, 1);

    app.split = sqlc_new_sidebar_split(uiControl(app.side_box), uiControl(right),
                                       220, 120, "SidebarWidth");
    uiBoxAppend(root, app.split, 1);

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
        profiles_free(&app.profiles);
        free(app.profiles_file);
        if (app.conn) mysql_close(app.conn);
        uiUninit();
    }
    mysql_library_end();
    return 0;
}
