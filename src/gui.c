/*
 * sqlc-gui - a simple MySQL/MariaDB GUI built with libui-ng.
 *
 * Layout, two screens in one window (one shown at a time):
 *
 * Welcome, while not connected:
 *   +-------------------+------------------------------+
 *   | Saved connections |   Name / Host / Port / User  |
 *   | New connection    |   Password / Database        |
 *   | ...               |   options, [Save][Delete]    |
 *   |                   |                  [Connect]   |
 *   +-------------------+------------------------------+
 *   status line
 *
 * Connected:
 *   Connected to user@host:port - database: db  [x] Hide views [Disconnect]
 *   +-----------+------------------------------------+
 *   | Tables    | [ Query | Structure ]                |
 *   |           | Query: SQL editor, [Run]           |
 *   |           |   ======== draggable divider ===== |
 *   |           |   results (one tab per result set) |
 *   |           | Structure: columns and indexes of  |
 *   |           |   the table selected in the sidebar|
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
#include <limits.h>
#include <stdint.h>
#include <time.h>

#include <ui.h>
#include <mysql.h>
#include <curl/curl.h>

#include "ai.h"
#include "ai_db.h"
#include "db.h"
#include "macos_menu.h"
#include "segmented.h"
#include "splitview.h"
#include "sql_tips.h"
#include "export.h"
#include "export_db.h"
#include "export_dialog.h"
#include "grid_edit.h"
#include "loading.h"
#include "row_edit.h"
#include "table_menu.h"
#include "profiles.h"

#define GUI_MAX_ROWS 10000 /* rows kept per result set in the grid */
#define AI_KEY_FILE "claude-api-key" /* in the config folder, mode 0600 */
#define AI_SCHEMA_BYTES (200u * 1024) /* schema text sent to Claude, at most about */

/* ---------- table models backed by a db_set ---------- */

typedef struct {
    uiTableModelHandler mh; /* must be first: libui hands us &mh */
    uiTableModel *model;
    db_set *set;
    /* an unsaved new row: cells not typed into yet show "(default)" */
    int pending_row;               /* -1 = none */
    const unsigned char *pending_set;
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
    set_model *sm = (set_model *)mh;
    if (row == sm->pending_row && sm->pending_set && !sm->pending_set[col])
        return uiNewTableValueString("(default)");
    const char *v = db_cell(sm->set, (size_t)row, (unsigned)col);
    return uiNewTableValueString(v ? v : "NULL");
}

static void m_set_cell_value(uiTableModelHandler *mh, uiTableModel *m, int row, int col,
                             const uiTableValue *v) {
    (void)mh; (void)m; (void)row; (void)col; (void)v; /* edits go through grid_edit.m */
}

static set_model *new_set_model(db_set *set) {
    set_model *sm = calloc(1, sizeof *sm);
    if (!sm) return NULL;
    sm->mh.NumColumns = m_num_columns;
    sm->mh.ColumnType = m_column_type;
    sm->mh.NumRows = m_num_rows;
    sm->mh.CellValue = m_cell_value;
    sm->mh.SetCellValue = m_set_cell_value;
    sm->set = set;
    sm->pending_row = -1;
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

/* What the main area shows, picked with the Normal | AI | Structure switch
 * (its segments, in this order). Normal and AI share the query/results
 * split and swap only its top pane; Structure is a page of its own. */
enum { MODE_NORMAL, MODE_AI, MODE_STRUCTURE };

/* The Structure page: columns and indexes of one table. */
typedef struct {
    uiLabel *title;
    uiButton *refresh_btn;
    uiBox *cols_box, *idx_box;    /* hold the grids (or a message) */
    uiControl *cols, *idx;        /* their current child */
    set_model *cols_model, *idx_model;
    db_outcome data;              /* columns, indexes, table info */
    char *table;                  /* table shown (or tried), NULL = none */
} structure_page;

/* The grid of a browsed table, when its rows can be edited. */
typedef struct {
    uiTable *grid;           /* NULL = the results aren't a browsed table */
    set_model *model;
    db_set *set;             /* app.shown.sets[0] */
    table_info info;
    const char *readonly;    /* why it can't be edited, or NULL */
    int new_row;             /* the unsaved new row, or -1 */
    unsigned char *new_set;  /* ... its columns typed into */
    uiButton *add_btn, *del_btn, *reload_btn, *clear_btn;
} grid_edit_state;

static struct {
    uiWindow *win;
    uiEntry *host, *port, *user, *pass, *db;

    /* the two screens: welcome (not connected) and the full layout */
    uiControl *welcome;
    uiControl *workspace;

    /* saved connections, the welcome screen's list: row 0 is "New
     * connection", row i is profiles.items[i - 1] */
    uiBox *saved_box;        /* holds saved_table */
    uiTable *saved_table;
    set_model *saved_model;
    db_outcome saved_data;   /* the list's rows (1 column) */
    int profile_sel;         /* selected row, as above */
    int filling_saved;       /* the list is being refilled: ignore its selection */
    uiControl *form_col;     /* the form and its buttons, disabled while connecting */
    uiControl *loading;      /* spinner + SQL tip under the form, see loading.h */
    int connecting;          /* a connect job is running: the above are in use */
    size_t last_tip;         /* index of the tip shown last time, or SIZE_MAX */
    uiControl *side_loading; /* spinner + tip in the sidebar, while refreshing */
    int refreshing;          /* a table-list job is running */
    uiEntry *profile_name;
    uiCheckbox *remember_pw;
    profile_list profiles;
    char *profiles_file;
    uiButton *connect_btn, *run_btn, *load_tables_btn, *export_btn;
    uiCheckbox *hide_views;     /* views left out of the sidebar and SHOW TABLES */
    uiCheckbox *no_rehash;      /* -A: only load the table list on request */
    uiCheckbox *tables_once;    /* tables only, loaded on connect, then like -A */

    /* the connected bar above the full layout */
    uiLabel *conn_label;
    uiCheckbox *hide_views_bar; /* mirrors hide_views, which stays the real one */
    uiButton *disconnect_btn;
    char *conn_user, *conn_host; /* what the connection was opened with */
    unsigned int conn_port;

    uiMultilineEntry *editor;
    uiLabel *status;

    /* main area: the Normal | AI | Structure switch above one page */
    uiControl *mode_switch;  /* segmented, see segmented.h */
    int mode;                /* MODE_*, what's shown */
    uiControl *query_split;  /* Normal / AI page: query pane above, results below */
    uiControl *structure_view; /* Structure page: columns above, indexes below */
    structure_page st;

    /* the query split's top pane: one of these at a time, in the same space */
    uiControl *normal_view;  /* SQL editor + run row (MODE_NORMAL) */
    uiControl *ai_view;      /* request + Ask Claude (MODE_AI) */

    /* Ask Claude: describe a query, get SQL in the editor */
    uiMultilineEntry *ai_task;
    uiButton *ai_btn;
    int ai_in_editor;        /* the editor holds Claude's SQL, not run yet */

    /* results area: holder box with one child (a tab, grid or label) */
    uiBox *results_box;
    uiControl *results;
    set_model **models;
    size_t nmodels;
    db_outcome shown; /* data behind the result grids */

    /* sidebar: list of tables in the current database */
    uiControl *split;        /* sidebar | Query / Structure */
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
    char *pending_browse;    /* double-clicked while a structure was loading */
    grid_edit_state edit;    /* editing that table's rows */
    char *current_db;        /* SELECT DATABASE() after the last job, or NULL */

    MYSQL *conn;
    int busy;
    int job_kind;            /* of the running job, while busy */
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

/* Toolbar under a browsed table's grid. */
static void update_edit_buttons(void) {
    grid_edit_state *e = &app.edit;
    if (!e->grid) return;
    int can = !app.busy && !e->readonly;
    uiButton *editing[] = { e->add_btn, e->del_btn, e->clear_btn };
    for (int i = 0; i < 3; i++) {
        if (can) uiControlEnable(uiControl(editing[i]));
        else uiControlDisable(uiControl(editing[i]));
    }
    if (app.busy) uiControlDisable(uiControl(e->reload_btn));
    else uiControlEnable(uiControl(e->reload_btn));
}

static void set_connecting(int on);
static void set_refreshing(int on);

static void set_busy(int busy) {
    app.busy = busy;
    if (!busy) {
        set_connecting(0);
        set_refreshing(0);
    }
    update_edit_buttons();
    if (busy) {
        uiControlDisable(uiControl(app.connect_btn));
        uiControlDisable(uiControl(app.disconnect_btn));
        uiControlDisable(uiControl(app.run_btn));
        uiControlDisable(uiControl(app.load_tables_btn));
        uiControlDisable(uiControl(app.export_btn));
        uiControlDisable(uiControl(app.ai_btn));
        uiControlDisable(uiControl(app.st.refresh_btn));
    } else {
        uiControlEnable(uiControl(app.connect_btn));
        uiControlEnable(uiControl(app.disconnect_btn));
        uiControlEnable(uiControl(app.run_btn));
        uiControlEnable(uiControl(app.load_tables_btn));
        uiControlEnable(uiControl(app.export_btn));
        uiControlEnable(uiControl(app.ai_btn));
        uiControlEnable(uiControl(app.st.refresh_btn));
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

static void clear_edit_state(void) {
    grid_edit_state *e = &app.edit;
    if (e->grid) sqlc_grid_detach(e->grid); /* before the grid goes away */
    table_info_free(&e->info);
    free(e->new_set);
    memset(e, 0, sizeof *e);
    e->new_row = -1;
}

static void clear_results(void) {
    clear_edit_state();
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

static uiControl *editable_page(set_model *sm, const db_outcome *meta);

/* Takes ownership of out. meta: the columns of the browsed table (see
 * fetch_browse_meta), to make its grid editable; NULL for other queries. */
static void show_outcome(db_outcome *out, const db_outcome *meta) {
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
            set_model *sm = new_set_model(&app.shown.sets[i]);
            app.models[app.nmodels++] = sm;
            int editable = meta && app.shown.nsets == 1 && !app.shown.error;
            pages[p] = editable ? editable_page(sm, meta) : uiControl(new_grid(sm));
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
static void on_side_selection(uiTable *t, void *data);
static const char *selected_table(int *is_view_out);
static size_t selected_count(void);
static int selected_tables(char ***names_out, size_t *n_out);
static void free_names(char **names, size_t n);
static unsigned on_table_menu_open(int row, void *data);
static void on_table_menu_action(table_menu_item item, int row, void *data);

static int is_view(const char *type) { return type && strstr(type, "VIEW") != NULL; }

/* Rebuild the visible table list from tables_raw and the "Hide views" box,
 * selecting the nkeep tables named in keep (those still in the list). */
static void rebuild_sidebar_keeping(char *const *keep, size_t nkeep) {
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
    /* Shift-click selects a range, Command-click adds or removes a row */
    uiTableSetSelectionMode(app.side_table, uiTableSelectionModeZeroOrMany);
    sqlc_table_fill_width(app.side_table); /* column follows the sidebar width */
    uiTableOnRowDoubleClicked(app.side_table, on_table_double_click, NULL);
    uiTableOnSelectionChanged(app.side_table, on_side_selection, NULL);
    sqlc_table_set_context_menu(app.side_table, on_table_menu_open, on_table_menu_action, NULL);
    uiBoxAppend(app.side_box, uiControl(app.side_table), 1);
    if (nkeep == 0 || s->nrows == 0 || s->nrows > (size_t)INT_MAX) return;

    /* the visible rows whose table is in keep */
    const char **names = calloc(s->nrows, sizeof *names);
    unsigned char *kept = calloc(s->nrows, 1);
    int *rows = calloc(s->nrows, sizeof *rows);
    if (names && kept && rows) {
        for (size_t v = 0; v < s->nrows; v++) names[v] = db_cell(raw, app.side_index[v], 0);
        if (export_select_names(s->nrows, names, nkeep, (const char *const *)keep, kept, NULL) >= 0) {
            int nrows = 0;
            for (size_t v = 0; v < s->nrows; v++)
                if (kept[v]) rows[nrows++] = (int)v;
            if (nrows) {
                uiTableSelection sel = { .NumRows = nrows, .Rows = rows };
                uiTableSetSelection(app.side_table, &sel);
            }
        }
    }
    free(rows);
    free(kept);
    free(names);
}

/* The same, keeping the current selection (tables_raw must be unchanged). */
static void rebuild_sidebar(void) {
    char **keep = NULL;
    size_t nkeep = 0;
    if (selected_tables(&keep, &nkeep) != 0) nkeep = 0; /* out of memory: drop it */
    rebuild_sidebar_keeping(keep, nkeep);
    free_names(keep, nkeep);
}

/* ---------- welcome screen / connected layout ---------- */

/* The connected bar's Hide views follows the real checkbox (welcome form). */
static void sync_bar_hide_views(void) {
    uiCheckboxSetChecked(app.hide_views_bar, uiCheckboxChecked(app.hide_views));
    if (uiControlEnabled(uiControl(app.hide_views))) uiControlEnable(uiControl(app.hide_views_bar));
    else uiControlDisable(uiControl(app.hide_views_bar));
}

/* "Connected to user@host:port - database: db", kept up to date after each job. */
static void update_conn_label(void) {
    if (!app.conn) return;
    char port[16] = "";
    if (app.conn_port) snprintf(port, sizeof port, ":%u", app.conn_port);
    char buf[1024];
    snprintf(buf, sizeof buf, "Connected to %s%s%s%s - %s%s",
             app.conn_user ? app.conn_user : "", app.conn_user && *app.conn_user ? "@" : "",
             app.conn_host ? app.conn_host : "", port,
             app.current_db ? "database: " : "no database selected",
             app.current_db ? app.current_db : "");
    uiLabelSetText(app.conn_label, buf);
}

/* Start a loading indicator with a random SQL tip, never the last one shown. */
static void start_loading_with_tip(uiControl *loading) {
    app.last_tip = sql_tip_pick(app.last_tip, (unsigned long)arc4random());
    const char *tip = sql_tip(app.last_tip);
    char buf[SQL_TIP_MAX_LEN + 16];
    snprintf(buf, sizeof buf, "Tip: %s", tip ? tip : "");
    sqlc_loading_start(loading, tip ? buf : NULL);
}

/* While a connection is being opened: the welcome form and saved list are
 * disabled, and a spinner with a random SQL tip (a different one each time)
 * shows under the form. Off again from set_busy(0), whatever the outcome. */
static void set_connecting(int on) {
    if (on == app.connecting) return;
    app.connecting = on;
    if (app.saved_table) sqlc_table_set_enabled(app.saved_table, !on);
    if (!on) {
        uiControlEnable(app.form_col);
        sqlc_loading_stop(app.loading);
        return;
    }
    uiControlDisable(app.form_col);
    start_loading_with_tip(app.loading);
}

/* While the table list loads (Load / Refresh tables, Hide views): the list
 * is dimmed and a spinner with a SQL tip shows above it in the sidebar.
 * Off again from set_busy(0); the list is rebuilt enabled afterwards. */
static void set_refreshing(int on) {
    if (on == app.refreshing) return;
    app.refreshing = on;
    if (app.side_table) sqlc_table_set_enabled(app.side_table, !on);
    if (!on) {
        sqlc_loading_stop(app.side_loading);
        uiControlHide(app.side_loading);
        return;
    }
    uiControlShow(app.side_loading);
    start_loading_with_tip(app.side_loading);
}

/* Welcome screen while not connected, the full layout once connected. */
static void show_screen(void) {
    if (app.conn) {
        update_conn_label();
        sync_bar_hide_views();
        uiControlHide(app.welcome);
        uiControlShow(app.workspace);
        sqlc_window_layout_now(app.win, 1); /* no divider drawn where it was */
    } else {
        uiControlHide(app.workspace);
        uiControlShow(app.welcome);
    }
}

/* ---------- worker thread ---------- */

typedef enum { JOB_CONNECT, JOB_QUERY, JOB_TABLES, JOB_EXPORT, JOB_EXPORT_DB, JOB_ALTER,
               JOB_AI, JOB_ROWS, JOB_STRUCTURE } job_kind;

/* JOB_ROWS: a change to the rows of the browsed table's grid */
typedef enum { ROW_UPDATE, ROW_INSERT, ROW_DELETE } row_op;

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
    /* JOB_EXPORT, JOB_EXPORT_DB (table = the database's name), JOB_ALTER,
     * JOB_STRUCTURE */
    char *table, *path;
    alter_kind alter;
    char *new_name;           /* ALTER_RENAME */
    int is_view;
    export_format fmt;
    export_opts xopts;
    unsigned long long rows;
    export_db_stats db_stats; /* JOB_EXPORT_DB */
    char **names;             /* ... only these tables/views (malloc'd), if nnames */
    size_t nnames;
    /* JOB_AI */
    char *task, *api_key;
    ai_result ai;
    /* JOB_QUERY browsing a table: also fetch its columns into meta */
    char *browse;
    db_outcome meta;
    /* JOB_ROWS */
    row_op op;
    db_set *edit_set;         /* the grid's rows when it started */
    int row, col, next_col;   /* edited cell; cell to edit next (-1) */
    char *value;              /* ROW_UPDATE: new value (NULL = SQL NULL) */
    char *typed;              /* ... as typed, to reopen the editor on failure */
    size_t *del_rows;         /* ROW_DELETE: saved rows, ascending */
    size_t ndel;
    int drop_new;             /* ... and the unsaved new row was selected too */

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
    free_names(j->names, j->nnames);
    free(j->task);
    if (j->api_key) { memset(j->api_key, 0, strlen(j->api_key)); free(j->api_key); }
    ai_result_free(&j->ai);
    free(j->browse);
    db_outcome_free(&j->meta);
    free(j->value);
    free(j->typed);
    free(j->del_rows);
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
static void ask_claude_now(void);

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

/* Columns and type of the browsed table j->browse; runs on the worker thread. */
static void fetch_browse_meta(job *j) {
    size_t n = strlen(j->browse);
    char *name = malloc(2 * n + 1);
    if (!name) return;
    mysql_real_escape_string(j->conn, name, j->browse, (unsigned long)n);
    size_t len = 2 * strlen(name) + 400;
    char *sql = malloc(len);
    if (sql) {
        snprintf(sql, len,
                 "SELECT COLUMN_NAME, DATA_TYPE, IS_NULLABLE, COLUMN_KEY, EXTRA "
                 "FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() "
                 "AND TABLE_NAME = '%s' ORDER BY ORDINAL_POSITION;"
                 "SELECT TABLE_TYPE FROM information_schema.TABLES "
                 "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = '%s'", name, name);
        db_exec_opts eo = { 0 };
        db_execute(j->conn, sql, &eo, &j->meta);
    }
    free(sql);
    free(name);
}

/* JOB_STRUCTURE on the worker thread: three result sets in j->out -
 * columns, indexes (one row each, columns joined), and the table's kind,
 * engine, row estimate, collation and comment. */
static void fetch_structure(job *j) {
    size_t n = strlen(j->table);
    char *name = malloc(2 * n + 1);
    if (!name) { j->out.error = strdup("Out of memory"); return; }
    mysql_real_escape_string(j->conn, name, j->table, (unsigned long)n);
    size_t len = 3 * strlen(name) + 1400;
    char *sql = malloc(len);
    if (sql) {
        snprintf(sql, len,
                 "SELECT COLUMN_NAME AS `Column`, COLUMN_TYPE AS `Type`, "
                 "IS_NULLABLE AS `Null`, COLUMN_KEY AS `Key`, "
                 /* SQL NULL here means "no default" (MySQL: also DEFAULT NULL) */
                 "IFNULL(COLUMN_DEFAULT, IF(IS_NULLABLE = 'YES', 'NULL', '')) AS `Default`, "
                 "EXTRA AS `Extra`, IFNULL(COLLATION_NAME, '') AS `Collation`, "
                 "COLUMN_COMMENT AS `Comment` "
                 "FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() "
                 "AND TABLE_NAME = '%s' ORDER BY ORDINAL_POSITION;"
                 "SELECT INDEX_NAME AS `Index`, IF(MAX(NON_UNIQUE) = 0, 'YES', 'NO') AS `Unique`, "
                 "GROUP_CONCAT(CONCAT(COLUMN_NAME, IFNULL(CONCAT('(', SUB_PART, ')'), '')) "
                 "ORDER BY SEQ_IN_INDEX SEPARATOR ', ') AS `Columns`, "
                 "MAX(INDEX_TYPE) AS `Type`, MAX(INDEX_COMMENT) AS `Comment` "
                 "FROM information_schema.STATISTICS WHERE TABLE_SCHEMA = DATABASE() "
                 "AND TABLE_NAME = '%s' GROUP BY INDEX_NAME "
                 "ORDER BY INDEX_NAME <> 'PRIMARY', INDEX_NAME;"
                 "SELECT TABLE_TYPE, ENGINE, TABLE_ROWS, TABLE_COLLATION, TABLE_COMMENT "
                 "FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() "
                 "AND TABLE_NAME = '%s'", name, name, name);
        db_exec_opts eo = { 0 };
        j->ok = db_execute(j->conn, sql, &eo, &j->out) == 0;
    } else {
        j->out.error = strdup("Out of memory");
    }
    free(sql);
    free(name);
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

/* Database export progress: same hop, with the table being written. */
typedef struct {
    char *table;
    const char *what;         /* "database" or "tables" (static strings) */
    size_t index, count;
    unsigned long long rows;
} db_export_progress;

static void show_db_export_progress(void *data) {
    db_export_progress *p = data;
    if (app.busy)
        set_status("Exporting %s... %s (%zu of %zu), %llu rows",
                   p->what, p->table, p->index, p->count, p->rows);
    free(p->table);
    free(p);
}

static void db_export_progress_cb(const char *table, size_t index, size_t count,
                                  unsigned long long rows, void *data) {
    db_export_progress *p = malloc(sizeof *p);
    if (!p) return;
    p->table = strdup(table ? table : "");
    if (!p->table) { free(p); return; }
    p->what = data ? (const char *)data : "database"; /* a string literal */
    p->index = index;
    p->count = count;
    p->rows = rows;
    uiQueueMain(show_db_export_progress, p);
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
        if (j->ok && j->browse) fetch_browse_meta(j);
    } else if (j->kind == JOB_ROWS) {
        j->ok = db_execute(j->conn, j->sql, &j->eo, &j->out) == 0;
        if (!j->ok && j->op == ROW_DELETE) { /* undo the deletes that worked */
            db_outcome rb;
            db_execute(j->conn, "ROLLBACK", &j->eo, &rb);
            db_outcome_free(&rb);
        }
    } else if (j->kind == JOB_AI) {
        ask_claude(j);
    } else if (j->kind == JOB_STRUCTURE) {
        fetch_structure(j);
    } else if (j->kind == JOB_EXPORT) {
        j->ok = export_table(j->conn, j->table, j->is_view, j->fmt, &j->xopts, j->path,
                             export_progress, NULL, &j->rows, &j->error) == 0;
    } else if (j->kind == JOB_EXPORT_DB) {
        if (j->nnames)
            j->ok = export_tables(j->conn, (const char *const *)j->names, j->nnames, &j->xopts,
                                  j->path, db_export_progress_cb, (void *)"tables",
                                  &j->db_stats, &j->error) == 0;
        else
            j->ok = export_database(j->conn, &j->xopts, j->path, db_export_progress_cb, NULL,
                                    &j->db_stats, &j->error) == 0;
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
    app.job_kind = j->kind;
    set_busy(1);
    if (j->kind == JOB_CONNECT) set_connecting(1);
    else if (j->kind == JOB_TABLES) set_refreshing(1);
    if (pthread_create(&th, NULL, worker, j) != 0) {
        set_busy(0);
        set_status("Could not start worker thread");
        job_free(j);
        return;
    }
    pthread_detach(th);
}

static void reload_browse(void);
static int browse_table(const char *name);
static void rows_done(job *j);

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

/* ---------- Structure page ---------- */

static void structure_clear(void) {
    structure_page *st = &app.st;
    /* the boxes' children (grids or labels) first, then the models */
    if (st->cols) { uiBoxDelete(st->cols_box, 0); uiControlDestroy(st->cols); st->cols = NULL; }
    if (st->idx) { uiBoxDelete(st->idx_box, 0); uiControlDestroy(st->idx); st->idx = NULL; }
    free_set_model(st->cols_model);
    free_set_model(st->idx_model);
    st->cols_model = st->idx_model = NULL;
    db_outcome_free(&st->data);
}

/* Empty page with a message instead of the structure; forgets the table. */
static void structure_message(const char *text) {
    structure_clear();
    free(app.st.table);
    app.st.table = NULL;
    uiLabelSetText(app.st.title, text);
}

/* One grid (or "none" label) into a box. */
static uiControl *structure_grid(uiBox *box, db_set *set, set_model **model, const char *none) {
    uiControl *c;
    if (set && set->has_rows && set->nrows) {
        *model = new_set_model(set);
        uiTable *t = new_grid(*model);
        c = uiControl(t);
    } else {
        c = uiControl(uiNewLabel(none));
    }
    uiBoxAppend(box, c, set && set->nrows);
    return c;
}

/* JOB_STRUCTURE finished: show what came back (takes j->out). */
static void structure_done(job *j) {
    structure_page *st = &app.st;
    structure_clear();
    free(st->table);
    st->table = j->table; /* even on failure, so it isn't retried in a loop */
    j->table = NULL;
    st->data = j->out;
    memset(&j->out, 0, sizeof j->out);

    db_set *cols = st->data.nsets > 0 ? &st->data.sets[0] : NULL;
    db_set *idx = st->data.nsets > 1 ? &st->data.sets[1] : NULL;
    db_set *info = st->data.nsets > 2 && st->data.sets[2].nrows ? &st->data.sets[2] : NULL;

    char title[1024];
    if (st->data.error) {
        snprintf(title, sizeof title, "%s: %s", st->table, st->data.error);
        set_status("Could not load the structure of %s: %s", st->table, st->data.error);
    } else if (!cols || !cols->nrows) {
        snprintf(title, sizeof title, "%s: no such table in this database", st->table);
        set_status("%s", title);
    } else {
        const char *type = info ? db_cell(info, 0, 0) : NULL;
        const char *engine = info ? db_cell(info, 0, 1) : NULL;
        const char *rows = info ? db_cell(info, 0, 2) : NULL;
        const char *coll = info ? db_cell(info, 0, 3) : NULL;
        const char *comment = info ? db_cell(info, 0, 4) : NULL;
        int n = snprintf(title, sizeof title, "%s - %s", st->table,
                         info && is_view(type) ? "view" : "table");
        if (engine && n < (int)sizeof title)
            n += snprintf(title + n, sizeof title - n, ", %s", engine);
        if (rows && !(info && is_view(type)) && n < (int)sizeof title)
            n += snprintf(title + n, sizeof title - n, ", about %s rows", rows);
        if (coll && n < (int)sizeof title)
            n += snprintf(title + n, sizeof title - n, ", %s", coll);
        if (comment && *comment && !is_view(type) && n < (int)sizeof title)
            snprintf(title + n, sizeof title - n, " - %s", comment);
        size_t ni = idx ? idx->nrows : 0;
        set_status("Structure of %s: %zu column%s, %zu index%s", st->table, cols->nrows,
                   cols->nrows == 1 ? "" : "s", ni, ni == 1 ? "" : "es");
    }
    uiLabelSetText(st->title, title);
    st->cols = structure_grid(st->cols_box, cols, &st->cols_model, "No columns.");
    st->idx = structure_grid(st->idx_box, idx, &st->idx_model,
                             st->data.error ? "" : "No indexes.");
}

static void start_structure(const char *table) {
    job *j = calloc(1, sizeof *j);
    if (!j) return;
    j->kind = JOB_STRUCTURE;
    j->conn = app.conn;
    j->table = strdup(table);
    set_status("Loading the structure of %s...", table);
    start_job(j);
}

/* On the Structure page, show the table selected in the sidebar (loading
 * it if it isn't the one shown). Called whenever that might have changed. */
static void structure_sync(void) {
    if (app.mode != MODE_STRUCTURE) return;
    if (!app.conn) { structure_message("Not connected."); return; }
    size_t nsel = selected_count();
    if (nsel > 1) {
        char msg[128];
        snprintf(msg, sizeof msg, "%zu tables selected - select one to see its structure.", nsel);
        structure_message(msg);
        return;
    }
    const char *name = selected_table(NULL);
    if (!name) {
        if (!app.st.table)
            uiLabelSetText(app.st.title, app.tables_loaded
                           ? "Select a table in the sidebar to see its structure."
                           : "Load the table list and select a table to see its structure.");
        return;
    }
    if (app.busy || (app.st.table && strcmp(app.st.table, name) == 0)) return;
    start_structure(name);
}

/* Shows one mode of the main area and keeps the switch in step (setting it
 * from code doesn't call on_mode_changed). The results stay as they are
 * across Normal / AI; Structure loads the selected table if needed. */
static void show_mode(int mode) {
    if (mode != MODE_AI && mode != MODE_STRUCTURE) mode = MODE_NORMAL;
    app.mode = mode;
    if (mode == MODE_STRUCTURE) {
        uiControlHide(app.query_split);
        uiControlShow(app.structure_view);
    } else {
        if (mode == MODE_AI) {
            uiControlHide(app.normal_view);
            uiControlShow(app.ai_view);
        } else {
            uiControlHide(app.ai_view);
            uiControlShow(app.normal_view);
        }
        uiControlHide(app.structure_view);
        uiControlShow(app.query_split);
    }
    if (sqlc_segmented_selected(app.mode_switch) != mode)
        sqlc_segmented_set_selected(app.mode_switch, mode);
    structure_sync();
}

static void on_mode_changed(uiControl *seg, int index, void *data) {
    (void)seg; (void)data;
    show_mode(index);
}

static int sync_queued;

static void structure_sync_later(void *data) {
    (void)data;
    sync_queued = 0;
    structure_sync();
}

/* Not right away: a right-click selects the row and then builds its menu,
 * which must not see the app busy loading the structure. */
static void on_side_selection(uiTable *t, void *data) {
    (void)t; (void)data;
    if (!sync_queued) {
        sync_queued = 1;
        uiQueueMain(structure_sync_later, NULL);
    }
}

static void on_structure_refresh(uiButton *b, void *data) {
    (void)b; (void)data;
    if (app.busy || !app.conn) return;
    const char *name = selected_table(NULL);
    if (!name) name = app.st.table;
    if (name) start_structure(name);
    else structure_sync();
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
    int structure = app.st.table && strcmp(app.st.table, j->table) == 0;
    if (structure) {
        if (j->alter == ALTER_DROP) {
            char msg[600];
            snprintf(msg, sizeof msg, "The %s %s was dropped.", what, j->table);
            structure_message(msg);
        } else {
            /* forget it: reloaded below if it's still selected (by its new name) */
            free(app.st.table);
            app.st.table = NULL;
        }
    }
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
        if (shown) reload_browse(); /* an empty grid, still editable */
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

    /* The sidebar's selection, to keep when its list is replaced below
     * (following a rename). Not across connections. */
    char **keep = NULL;
    size_t nkeep = 0;
    if (j->load_tables && j->kind != JOB_CONNECT) {
        if (selected_tables(&keep, &nkeep) != 0) nkeep = 0;
        if (j->kind == JOB_ALTER && j->ok && j->alter == ALTER_RENAME) {
            for (size_t i = 0; i < nkeep; i++) {
                if (strcmp(keep[i], j->table) != 0) continue;
                char *renamed = strdup(j->new_name);
                if (renamed) { free(keep[i]); keep[i] = renamed; }
            }
        }
    }

    if (j->kind == JOB_CONNECT) {
        if (!j->ok) {
            set_status("%s", j->error ? j->error : "Connection failed");
            uiMsgBoxError(app.win, "Connection failed", j->error ? j->error : "");
            job_free(j);
            return;
        }
        if (app.conn) mysql_close(app.conn);
        app.conn = j->conn;
        free(app.conn_user);
        free(app.conn_host);
        app.conn_user = j->user; /* the connection's, for the connected bar */
        app.conn_host = j->host;
        j->user = j->host = NULL;
        app.conn_port = j->port;
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
        structure_message("");
        show_screen(); /* off the welcome screen */
    } else if (j->kind == JOB_STRUCTURE) {
        structure_done(j);
    } else if (j->kind == JOB_ALTER) {
        alter_done(j);
    } else if (j->kind == JOB_ROWS) {
        rows_done(j);
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
                show_mode(MODE_NORMAL); /* the SQL is in the editor: show it */
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
    } else if (j->kind == JOB_EXPORT_DB) {
        const export_db_stats *st = &j->db_stats;
        if (j->ok && j->nnames) {
            set_status("Exported %zu selected table%s from %s: %zu table%s, %zu view%s, %llu row%s "
                       "to %s (%.2f s)%s",
                       j->nnames, j->nnames == 1 ? "" : "s",
                       j->table, st->tables, st->tables == 1 ? "" : "s",
                       st->views, st->views == 1 ? "" : "s",
                       st->rows, st->rows == 1 ? "" : "s", j->path, j->seconds,
                       st->skipped ? " - some objects skipped, see the file" : "");
        } else if (j->ok) {
            set_status("Exported database %s: %zu table%s, %zu view%s, %llu row%s to %s (%.2f s)%s",
                       j->table, st->tables, st->tables == 1 ? "" : "s",
                       st->views, st->views == 1 ? "" : "s",
                       st->rows, st->rows == 1 ? "" : "s", j->path, j->seconds,
                       st->skipped ? " - some objects skipped, see the file" : "");
        } else {
            const char *title = j->nnames ? "Export failed" : "Database export failed";
            set_status("%s: %s", title, j->error ? j->error : "unknown error");
            show_error(title, j->error ? j->error : "");
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
        int browsed = j->browse && app.browsing && strcmp(j->browse, app.browsing) == 0;
        show_outcome(&j->out, browsed ? &j->meta : NULL);
    }

    /* The worker read DATABASE() after the job (see load_tables). */
    if (j->conn) {
        free(app.current_db);
        app.current_db = j->current_db;
        j->current_db = NULL;
        update_conn_label(); /* the database may have changed via USE */
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
    if (j->kind == JOB_CONNECT || j->load_tables) rebuild_sidebar_keeping(keep, nkeep);
    free_names(keep, nkeep);

    job_free(j);
    /* a double-click that came while the structure was loading */
    if (app.pending_browse) {
        char *name = app.pending_browse;
        app.pending_browse = NULL;
        browse_table(name);
        free(name);
    }
    structure_sync();
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
    int sel = app.profile_sel;
    if (sel > 0 && (size_t)sel <= app.profiles.n)
        j->profile = strdup(app.profiles.items[sel - 1].name);
    j->load_tables = load_on_connect();
    j->hide_views = uiCheckboxChecked(app.hide_views);
    set_status("Connecting to %s...", j->host);
    start_job(j);
}

/* Returns 1 if the query was started. browse: the table it shows (for editing). */
static int run_query(const char *sql, const char *browse) {
    if (app.busy) return 0;
    if (!app.conn) { set_status("Not connected - connect from the welcome screen first."); return 0; }

    const char *p = sql;
    while (isspace((unsigned char)*p)) p++;
    if (!*p) { set_status("Nothing to run."); return 0; }

    job *j = calloc(1, sizeof *j);
    if (!j) return 0;
    j->kind = JOB_QUERY;
    j->conn = app.conn;
    j->sql = strdup(sql);
    j->browse = browse ? strdup(browse) : NULL;
    j->eo.hide_views = uiCheckboxChecked(app.hide_views);
    j->eo.max_rows = GUI_MAX_ROWS;
    /* With -A (or "Tables only, load once") the list is only refreshed when
     * asked (Load/Refresh tables). */
    j->load_tables = load_after_queries();
    j->hide_views = j->eo.hide_views;
    set_status("Running...");
    start_job(j);
    /* that's where the results go (AI shows them too: stay there) */
    if (app.mode == MODE_STRUCTURE) show_mode(MODE_NORMAL);
    return 1;
}

static int run_sql(const char *sql) { return run_query(sql, NULL); }

/* SELECT * FROM a table, into an editable grid. Returns 1 if started. */
static int browse_table(const char *name) {
    char *copy = strdup(name); /* name may be app.browsing */
    char *sql = copy ? browse_sql(copy) : NULL;
    int started = 0;
    if (sql) {
        uiMultilineEntrySetText(app.editor, sql);
        app.ai_in_editor = 0;
        show_mode(MODE_NORMAL); /* the editor's text was replaced: show what runs */
        started = run_query(sql, copy);
        if (started) {
            free(app.browsing);
            app.browsing = copy;
            copy = NULL;
        }
    }
    free(sql);
    free(copy);
    return started;
}

static void reload_browse(void) {
    if (app.browsing) browse_table(app.browsing);
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
    if (app.mode == MODE_AI) { /* the editor is hidden: ⌘↩ sends the request instead */
        ask_claude_now();
        return;
    }
    if (app.mode == MODE_STRUCTURE) { /* the editor is hidden: don't run it unseen */
        set_status("Switch to Normal to run SQL (\xE2\x8C\x98\xE2\x86\xA9 runs the editor there).");
        return;
    }
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
    "Paste your Anthropic API key (from console.anthropic.com). It is saved in "
    "sqlc's settings folder (~/Library/Application Support/sqlc), readable only by you.\n\n"
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
    char *path = config_path(AI_KEY_FILE);
    if (!path || secret_write(path, key) != 0) {
        char err[600];
        snprintf(err, sizeof err, "Could not write %s: %s", path ? path : "(no config path)",
                 strerror(errno));
        show_error("Could not save the API key", err); /* still usable this time */
    } else {
        set_status("Claude API key saved in %s.", path);
    }
    free(path);
    return key;
}

/* The saved key, else $ANTHROPIC_API_KEY, else ask for one. */
static char *api_key(void) {
    char *path = config_path(AI_KEY_FILE);
    char *key = path ? secret_read(path) : NULL;
    free(path);
    if (key) return key;
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
    char *path = config_path(AI_KEY_FILE);
    if (path && secret_remove(path) == 0) set_status("Claude API key removed.");
    else set_status("Could not remove the Claude API key: %s", strerror(errno));
    free(path);
}

static void ask_claude_now(void) {
    if (app.busy) return;
    if (!app.conn) { set_status("Connect first, so Claude can see the database's tables."); return; }
    char *t = uiMultilineEntryText(app.ai_task);
    char *task = strdup(t ? t : "");
    uiFreeText(t);
    if (!task) return;
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

static void on_ask_claude(uiButton *b, void *data) {
    (void)b; (void)data;
    ask_claude_now();
}

static void on_ask_claude_menu(uiMenuItem *m, uiWindow *w, void *data) {
    (void)m; (void)w; (void)data;
    show_mode(MODE_AI); /* on the welcome screen: shown once connected */
    if (!app.conn) set_status("Not connected - connect from the welcome screen first.");
}

static void load_tables_now(void) {
    if (app.busy) return;
    if (!app.conn) { set_status("Not connected - connect from the welcome screen first."); return; }
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

/* The table selected in the sidebar when exactly one row is selected, else
 * NULL (pointer into tables_raw). */
static const char *selected_table(int *is_view_out) {
    if (!app.side_table || !app.side_data.nsets) return NULL;
    uiTableSelection *sel = uiTableGetSelection(app.side_table);
    int row = sel && sel->NumRows == 1 ? sel->Rows[0] : -1;
    uiFreeTableSelection(sel);
    return table_at_row(row, is_view_out);
}

/* How many sidebar rows are selected. */
static size_t selected_count(void) {
    if (!app.side_table || !app.side_data.nsets) return 0;
    uiTableSelection *sel = uiTableGetSelection(app.side_table);
    size_t n = sel && sel->NumRows > 0 ? (size_t)sel->NumRows : 0;
    uiFreeTableSelection(sel);
    return n;
}

static void free_names(char **names, size_t n) {
    for (size_t i = 0; names && i < n; i++) free(names[i]);
    free(names);
}

/* Copies of the names of every selected sidebar table, in list order:
 * *names_out (malloc'd array of malloc'd strings, free with free_names) and
 * *n_out, NULL and 0 when nothing is selected. Returns 0, or -1 if out of
 * memory (nothing to free then). */
static int selected_tables(char ***names_out, size_t *n_out) {
    *names_out = NULL;
    *n_out = 0;
    if (!app.side_table || !app.side_data.nsets) return 0;
    uiTableSelection *sel = uiTableGetSelection(app.side_table);
    if (!sel) return 0;
    size_t nrows = sel->NumRows > 0 ? (size_t)sel->NumRows : 0;
    char **names = nrows ? calloc(nrows, sizeof *names) : NULL;
    size_t n = 0;
    int rc = 0;
    if (nrows && !names) rc = -1;
    for (size_t i = 0; names && i < nrows; i++) {
        const char *t = table_at_row(sel->Rows[i], NULL);
        if (!t) continue;
        if (!(names[n] = strdup(t))) { rc = -1; break; }
        n++;
    }
    uiFreeTableSelection(sel);
    if (rc != 0 || n == 0) {
        free_names(names, n);
        return rc;
    }
    *names_out = names;
    *n_out = n;
    return 0;
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

static void export_database_flow(void);
static void export_tables_flow(char **names, size_t n);

/* Export…: the whole database with nothing selected in the sidebar, the one
 * selected table (SQL / CSV / XML), or the selected tables into one SQL file. */
static void export_sidebar_selection(void) {
    if (app.busy) return;
    if (!app.conn) { set_status("Not connected - connect from the welcome screen first."); return; }
    char **names = NULL;
    size_t n = 0;
    if (selected_tables(&names, &n) != 0) { set_status("Out of memory"); return; }
    if (n == 0) {
        export_database_flow();
    } else if (n == 1) {
        int view = 0;
        const char *name = selected_table(&view);
        export_table_flow(name ? name : names[0], view);
    } else {
        export_tables_flow(names, n); /* takes names */
        names = NULL;
        n = 0;
    }
    free_names(names, n);
}

static void on_export(uiButton *b, void *data) {
    (void)b; (void)data;
    export_sidebar_selection();
}

static void on_export_menu(uiMenuItem *m, uiWindow *w, void *data) {
    (void)m; (void)w; (void)data;
    export_sidebar_selection();
}

/* Export the current database (all tables, then views) on the worker thread. */
static void start_export_db(const char *db, const export_opts *opts, const char *path) {
    if (app.busy || !app.conn) return;
    job *j = calloc(1, sizeof *j);
    if (!j) return;
    j->kind = JOB_EXPORT_DB;
    j->conn = app.conn;
    j->table = strdup(db);
    j->path = strdup(path);
    j->fmt = EXPORT_SQL;
    j->xopts = *opts;
    if (!j->table || !j->path) { job_free(j); return; }
    set_status("Exporting database %s...", db);
    start_job(j);
}

/* Export the n named tables/views of database db into one SQL file on the
 * worker thread. Takes names (malloc'd, see selected_tables) in every case. */
static void start_export_tables(const char *db, char **names, size_t n,
                                const export_opts *opts, const char *path) {
    job *j = app.busy || !app.conn || !n ? NULL : calloc(1, sizeof *j);
    if (!j) { free_names(names, n); return; }
    j->kind = JOB_EXPORT_DB;
    j->conn = app.conn;
    j->names = names;
    j->nnames = n;
    j->table = strdup(db);
    j->path = strdup(path);
    j->fmt = EXPORT_SQL;
    j->xopts = *opts;
    if (!j->table || !j->path) { job_free(j); return; }
    set_status("Exporting %zu tables from %s...", n, db);
    start_job(j);
}

/* Options dialog + save panel for the selected tables. Takes names. */
static void export_tables_flow(char **names, size_t n) {
    if (!app.current_db) {
        free_names(names, n);
        uiMsgBoxError(app.win, "Export",
                      "No database selected. Pick one in the Database field and connect, "
                      "or run USE <database>; first.");
        return;
    }
    char *db = strdup(app.current_db); /* the dialog runs a modal loop; keep our own copy */
    if (!db) { free_names(names, n); return; }
    export_opts opts;
    char *path = NULL;
    if (sqlc_export_database_dialog(db, n, &opts, &path))
        start_export_tables(db, names, n, &opts, path);
    else
        free_names(names, n);
    free(path);
    free(db);
}

static void export_database_flow(void) {
    if (app.busy) return;
    if (!app.conn) { set_status("Not connected - connect from the welcome screen first."); return; }
    if (!app.current_db) {
        uiMsgBoxError(app.win, "Export Database",
                      "No database selected. Pick one in the Database field and connect, "
                      "or run USE <database>; first.");
        return;
    }
    char *db = strdup(app.current_db); /* the dialog runs a modal loop; keep our own copy */
    if (!db) return;
    export_opts opts;
    char *path = NULL;
    if (sqlc_export_database_dialog(db, 0, &opts, &path)) start_export_db(db, &opts, path);
    free(path);
    free(db);
}

static void on_export_db_menu(uiMenuItem *m, uiWindow *w, void *data) {
    (void)m; (void)w; (void)data;
    export_database_flow();
}

static void on_hide_views(uiCheckbox *c, void *data) {
    (void)c; (void)data;
    /* Views are filtered on the server, so turning this off needs a reload
     * to bring them back. Filter what we have meanwhile (or if busy). */
    sync_bar_hide_views();
    rebuild_sidebar();
    if (app.tables_loaded) load_tables_now();
}

/* The connected bar's copy of Hide views: drives the real one. */
static void on_hide_views_bar(uiCheckbox *c, void *data) {
    (void)data;
    uiCheckboxSetChecked(app.hide_views, uiCheckboxChecked(c));
    on_hide_views(app.hide_views, NULL);
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
    sync_bar_hide_views();
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
    /* The first click of it may have started loading the structure: browse
     * once that's done. */
    if (app.busy && app.job_kind == JOB_STRUCTURE) {
        free(app.pending_browse);
        app.pending_browse = strdup(name);
        return;
    }
    browse_table(name);
}

/* ---------- sidebar context menu ---------- */

/* Name and kind of the table on a visible sidebar row (pointer into tables_raw). */
static const char *table_at_row(int row, int *is_view_out) {
    if (row < 0 || !app.side_data.nsets || !app.tables_raw.nsets ||
        (size_t)row >= app.side_data.sets[0].nrows) return NULL;
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
    if (app.conn) items |= TABLE_MENU_BIT(TABLE_MENU_STRUCTURE);
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
    case TABLE_MENU_STRUCTURE:
        show_mode(MODE_STRUCTURE); /* the menu selected its row */
        break;
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

/* ---------- editing a browsed table's rows ---------- */

#ifdef SQLC_AUTOTEST
static int autotest_confirm = 1; /* answer to "are you sure?" dialogs */
#endif

static int confirm_danger(const char *title, const char *msg, const char *ok, int destructive) {
#ifdef SQLC_AUTOTEST
    (void)ok; (void)destructive;
    printf("[dialog] %s: %s -> %s\n", title, msg, autotest_confirm ? ok : "Cancel");
    return autotest_confirm;
#else
    return sqlc_confirm_danger(title, msg, ok, destructive);
#endif
}

static unsigned long escape_value(char *to, const char *from, unsigned long len, void *data) {
    return mysql_real_escape_string(data, to, from, len);
}

static row_edit_escaper escaper(void) {
    row_edit_escaper e = { escape_value, app.conn };
    return e;
}

static int cell_editable(int row, int col) {
    grid_edit_state *e = &app.edit;
    return e->grid && !e->readonly && row >= 0 && (size_t)row < e->set->nrows &&
           col >= 0 && (unsigned)col < e->info.ncols && row_edit_editable(&e->info, (unsigned)col);
}

static grid_editor_kind editor_kind(col_kind k) {
    return k == COL_DATE ? GRID_EDITOR_DATE : k == COL_DATETIME ? GRID_EDITOR_DATETIME
                                                                : GRID_EDITOR_TEXT;
}

/* Open the editor on a cell, with `text` (NULL = the cell's value). */
static void begin_cell_edit(int row, int col, const char *text) {
    grid_edit_state *e = &app.edit;
    if (!e->grid) return;
    if (!cell_editable(row, col)) {
        if (e->readonly) set_status("%s", e->readonly);
        else if (col >= 0 && (unsigned)col < e->info.ncols)
            set_status("Column `%s` can't be edited here (binary or generated).", e->info.cols[col].name);
        return;
    }
    const col_info *ci = &e->info.cols[col];
    const char *v = db_cell(e->set, (size_t)row, (unsigned)col);
    int is_default = row == e->new_row && !e->new_set[col];
    const char *placeholder = is_default ? (ci->auto_increment ? "auto" : "DEFAULT")
                              : v ? NULL : "NULL";
    if (!text) text = is_default || !v ? "" : v;
    sqlc_grid_begin_edit(e->grid, row, col, text, placeholder, editor_kind(ci->kind));
}

/* Next editable column after (dir = 1) or before (dir = -1) col, or -1. */
static int next_editable(int row, int col, int dir) {
    for (int c = col + dir; c >= 0 && (unsigned)c < app.edit.info.ncols; c += dir)
        if (cell_editable(row, c)) return c;
    return -1;
}

static int edit_job_ok(void) {
    if (app.busy) { set_status("Busy - wait for the running query to finish, then try again."); return 0; }
    if (!app.conn || !app.browsing) { set_status("Not connected."); return 0; }
    return 1;
}

static job *new_rows_job(row_op op, char *sql) {
    job *j = calloc(1, sizeof *j);
    if (!j) { free(sql); return NULL; }
    j->kind = JOB_ROWS;
    j->op = op;
    j->conn = app.conn;
    j->sql = sql;
    j->edit_set = app.edit.set;
    j->next_col = -1;
    return j;
}

/* Save one cell of a saved row (value NULL = SQL NULL). */
static void start_row_update(int row, int col, const char *value, const char *typed, int next_col) {
    if (!edit_job_ok()) { begin_cell_edit(row, col, typed); return; }
    char err[512];
    row_edit_escaper esc = escaper();
    char *sql = row_edit_update_sql(app.browsing, &app.edit.info, app.edit.set, (size_t)row,
                                    (unsigned)col, value, &esc, err, sizeof err);
    if (!sql) { set_status("%s", err); show_error("Can't save this change", err); return; }
    job *j = new_rows_job(ROW_UPDATE, sql);
    if (!j) return;
    j->row = row;
    j->col = col;
    j->next_col = next_col;
    j->value = value ? strdup(value) : NULL;
    j->typed = strdup(typed);
    set_status("Saving %s...", app.edit.info.cols[col].name);
    start_job(j);
}

/* INSERT the unsaved new row; col is the cell the user was in. */
static void start_row_insert(int col) {
    grid_edit_state *e = &app.edit;
    if (e->new_row < 0) return;
    if (!edit_job_ok()) return;
    char err[512];
    row_edit_escaper esc = escaper();
    char *sql = row_edit_insert_sql(app.browsing, &e->info, e->set, (size_t)e->new_row,
                                    e->new_set, &esc, err, sizeof err);
    if (!sql) { set_status("%s", err); show_error("Can't add this row", err); return; }
    job *j = new_rows_job(ROW_INSERT, sql);
    if (!j) return;
    j->row = e->new_row;
    j->col = col;
    set_status("Adding the row...");
    start_job(j);
}

static void set_cell(int row, int col, char *value /* taken */) {
    char **cell = &app.edit.set->cells[(size_t)row * app.edit.set->ncols + (unsigned)col];
    free(*cell);
    *cell = value;
}

/* The editor closed with text in (row, col). */
static void on_cell_committed(int row, int col, const char *text, grid_end how, void *data) {
    (void)data;
    grid_edit_state *e = &app.edit;
    if (!cell_editable(row, col)) return;
    int is_new = row == e->new_row;
    int next = how == GRID_END_TAB ? next_editable(row, col, 1)
             : how == GRID_END_BACKTAB ? next_editable(row, col, -1) : -1;
    const char *old = db_cell(e->set, (size_t)row, (unsigned)col);

    int unchanged = is_new ? !e->new_set[col] && !*text
                           : old ? strcmp(old, text) == 0 : !*text;
    if (!unchanged) {
        char err[512];
        int is_null = 0;
        char *v = row_edit_normalize(&e->info.cols[col], text, &is_null, err, sizeof err);
        if (!v && !is_null) {
            set_status("%s", err);
            show_error("Invalid value", err);
            begin_cell_edit(row, col, text); /* keep what was typed, to fix it */
            return;
        }
        if (!is_new) { /* saved row: UPDATE now, then go on to `next` */
            start_row_update(row, col, v, text, next);
            free(v);
            return;
        }
        set_cell(row, col, v);
        e->new_set[col] = 1;
        uiTableModelRowChanged(e->model->model, row);
    }
    if (is_new && how == GRID_END_RETURN) start_row_insert(col);
    else if (next >= 0) begin_cell_edit(row, next, NULL);
}

static void on_cell_cancelled(int row, int col, void *data) {
    (void)row; (void)col; (void)data;
    if (app.edit.new_row >= 0)
        set_status("The new row isn't saved yet: press Return in one of its cells to add it, "
                   "or select it and press \xE2\x88\x92 to discard it.");
}

static void on_cell_double_clicked(int row, int col, void *data) {
    (void)data;
    begin_cell_edit(row, col, NULL);
}

/* + : a new row under the last one, edited in place. */
static void on_add_row(uiButton *b, void *data) {
    (void)b; (void)data;
    grid_edit_state *e = &app.edit;
    if (!e->grid || e->readonly || app.busy) return;
    db_set *s = e->set;
    int first = -1;
    if (e->new_row < 0) {
        char **cells = realloc(s->cells, (s->nrows + 1) * s->ncols * sizeof *cells);
        unsigned char *flags = calloc(s->ncols, 1);
        if (!cells || !flags) { if (cells) s->cells = cells; free(flags); return; }
        s->cells = cells;
        for (unsigned c = 0; c < s->ncols; c++) s->cells[s->nrows * s->ncols + c] = NULL;
        e->new_set = flags;
        e->new_row = (int)s->nrows++;
        e->model->pending_row = e->new_row;
        e->model->pending_set = e->new_set;
        uiTableModelRowInserted(e->model->model, e->new_row);
        set_status("New row: type the values (Tab moves to the next column) and press Return "
                   "to save it. Cells left as (default) get the column's default.");
    } else {
        set_status("Finish the new row first: press Return in one of its cells to save it.");
    }
    /* start in the first column the user would fill in (not an auto-increment id) */
    for (unsigned c = 0; c < s->ncols && first < 0; c++)
        if (cell_editable(e->new_row, (int)c) && !e->info.cols[c].auto_increment) first = (int)c;
    if (first < 0) first = next_editable(e->new_row, -1, 1);
    sqlc_grid_scroll_to_row(e->grid, e->new_row);
    if (first >= 0) begin_cell_edit(e->new_row, first, NULL);
}

static void remove_grid_row(int row) {
    grid_edit_state *e = &app.edit;
    db_set *s = e->set;
    sqlc_grid_close_editor(e->grid); /* it may sit on a row that moves */
    for (unsigned c = 0; c < s->ncols; c++) free(s->cells[(size_t)row * s->ncols + c]);
    memmove(s->cells + (size_t)row * s->ncols, s->cells + (size_t)(row + 1) * s->ncols,
            (s->nrows - (size_t)row - 1) * s->ncols * sizeof *s->cells);
    s->nrows--;
    if (row == e->new_row) {
        e->new_row = -1;
        free(e->new_set);
        e->new_set = NULL;
    } else if (row < e->new_row) {
        e->new_row--;
    }
    e->model->pending_row = e->new_row;
    e->model->pending_set = e->new_set;
    uiTableModelRowDeleted(e->model->model, row);
}

/* - : delete the selected rows, after asking. */
static void delete_selected_rows(void) {
    grid_edit_state *e = &app.edit;
    if (!e->grid || e->readonly) return;
    if (!edit_job_ok()) return;
    uiTableSelection *sel = uiTableGetSelection(e->grid);
    size_t n = sel ? (size_t)sel->NumRows : 0;
    size_t *rows = calloc(n ? n : 1, sizeof *rows);
    size_t nsaved = 0;
    int drop_new = 0;
    for (size_t i = 0; rows && i < n; i++) {
        int r = sel->Rows[i];
        if (r < 0 || (size_t)r >= e->set->nrows) continue;
        if (r == e->new_row) drop_new = 1;
        else rows[nsaved++] = (size_t)r;
    }
    uiFreeTableSelection(sel);
    if (!rows) return;
    if (nsaved == 0 && !drop_new) {
        free(rows);
        set_status("Select the rows to delete first (\xE2\x8C\x98-click or \xE2\x87\xA7-click selects several).");
        return;
    }
    for (size_t i = 1; i < nsaved; i++) /* ascending, for removing them later */
        for (size_t k = i; k > 0 && rows[k - 1] > rows[k]; k--) {
            size_t t = rows[k]; rows[k] = rows[k - 1]; rows[k - 1] = t;
        }

    char title[600], msg[300];
    size_t total = nsaved + (size_t)drop_new;
    if (total == 1) snprintf(title, sizeof title, "Delete 1 row from `%s`?", app.browsing);
    else snprintf(title, sizeof title, "Delete %zu rows from `%s`?", total, app.browsing);
    snprintf(msg, sizeof msg, "%s", nsaved ? "This can't be undone." : "The new row isn't saved yet; it will be discarded.");
    if (!confirm_danger(title, msg, "Delete", 1)) { free(rows); return; }

    if (nsaved == 0) { /* only the unsaved row: nothing to tell the server */
        remove_grid_row(e->new_row);
        free(rows);
        set_status("Discarded the new row.");
        return;
    }
    char err[512];
    row_edit_escaper esc = escaper();
    char *sql = row_edit_delete_sql(app.browsing, &e->info, e->set, rows, nsaved, &esc, err, sizeof err);
    if (!sql) { free(rows); set_status("%s", err); show_error("Can't delete these rows", err); return; }
    job *j = new_rows_job(ROW_DELETE, sql);
    if (!j) { free(rows); return; }
    j->del_rows = rows;
    j->ndel = nsaved;
    j->drop_new = drop_new;
    set_status("Deleting %zu row%s...", nsaved, nsaved == 1 ? "" : "s");
    start_job(j);
}

static void on_delete_rows(uiButton *b, void *data) {
    (void)b; (void)data;
    delete_selected_rows();
}

static void on_reload_rows(uiButton *b, void *data) {
    (void)b; (void)data;
    if (app.busy) return;
    if (app.edit.new_row >= 0) set_status("Reloading: the unsaved new row was discarded.");
    reload_browse();
}

/* Delete all rows: TRUNCATE TABLE, then the grid reloads (see alter_done). */
static void on_clear_rows(uiButton *b, void *data) {
    (void)b; (void)data;
    if (!app.edit.grid || app.edit.readonly || !edit_job_ok()) return;
    char *name = strdup(app.browsing);
    if (!name) return;
    char title[600];
    snprintf(title, sizeof title, "Delete all rows of `%s`?", name);
    if (confirm_danger(title,
                       "TRUNCATE TABLE removes every row, including those not shown here, "
                       "and resets AUTO_INCREMENT. This can't be undone.",
                       "Delete All Rows", 1))
        start_alter(ALTER_TRUNCATE, name, 0, NULL);
    free(name);
}

/* Copy row 0 of src (a SELECT * of the table) into grid row `row`. */
static void replace_grid_row(int row, const db_set *src) {
    for (unsigned c = 0; c < app.edit.set->ncols; c++) {
        const char *v = db_cell(src, 0, c);
        set_cell(row, (int)c, v ? strdup(v) : NULL);
    }
    uiTableModelRowChanged(app.edit.model->model, row);
}

/* The row read back after an UPDATE / INSERT, if there is exactly one. */
static const db_set *refreshed_row(const job *j) {
    const db_set *s = j->out.nsets >= 2 ? &j->out.sets[1] : NULL;
    return s && s->has_rows && s->nrows == 1 && s->ncols == app.edit.set->ncols ? s : NULL;
}

static void rows_done(job *j) {
    grid_edit_state *e = &app.edit;
    const char *err = j->out.error ? j->out.error : "unknown error";
    if (!e->grid || e->set != j->edit_set) { /* the grid was replaced meanwhile */
        set_status(j->ok ? "Saved." : "Failed: %s", err);
        return;
    }
    switch (j->op) {
    case ROW_UPDATE: {
        if (!j->ok) {
            set_status("Update failed: %s", err);
            show_error("Update failed", err);
            begin_cell_edit(j->row, j->col, j->typed); /* keep the edit, to fix it */
            break;
        }
        const db_set *r = refreshed_row(j);
        if (r) replace_grid_row(j->row, r);
        else { /* not found again (e.g. a trigger changed it): show what we sent */
            set_cell(j->row, j->col, j->value ? strdup(j->value) : NULL);
            uiTableModelRowChanged(e->model->model, j->row);
        }
        unsigned long long n = j->out.nsets ? j->out.sets[0].affected : 0;
        set_status(n ? "Saved %s in row %d of %s." : "%s in row %d of %s: no change.",
                   e->info.cols[j->col].name, j->row + 1, app.browsing);
        if (j->next_col >= 0) begin_cell_edit(j->row, j->next_col, NULL);
        break;
    }
    case ROW_INSERT: {
        if (!j->ok) {
            set_status("Insert failed: %s", err);
            show_error("Insert failed", err);
            if (e->new_row >= 0) begin_cell_edit(e->new_row, j->col, NULL);
            break;
        }
        const db_set *r = refreshed_row(j);
        if (!r) { /* can't find it again (no key): read the table back */
            set_status("Added 1 row to %s.", app.browsing);
            reload_browse();
            break;
        }
        int row = e->new_row;
        e->new_row = -1;
        e->model->pending_row = -1;
        e->model->pending_set = NULL;
        free(e->new_set);
        e->new_set = NULL;
        replace_grid_row(row, r);
        set_status("Added 1 row to %s.", app.browsing);
        break;
    }
    case ROW_DELETE: {
        if (!j->ok) {
            set_status("Delete failed (nothing was deleted): %s", err);
            show_error("Delete failed", err);
            break;
        }
        unsigned long long gone = 0;
        for (size_t i = 0; i < j->out.nsets; i++) gone += j->out.sets[i].affected;
        if (j->drop_new && e->new_row >= 0) remove_grid_row(e->new_row);
        for (size_t i = j->ndel; i-- > 0;) remove_grid_row((int)j->del_rows[i]);
        set_status("Deleted %llu row%s from %s.", gone, gone == 1 ? "" : "s", app.browsing);
        break;
    }
    }
}

/* ---- cell context menu ---- */

static unsigned on_cell_menu_open(int row, int col, void *data) {
    (void)data;
    grid_edit_state *e = &app.edit;
    if (!e->grid || row < 0 || (size_t)row >= e->set->nrows) return 0;
    unsigned items = 0;
    if (col >= 0) items |= GRID_MENU_BIT(GRID_MENU_COPY);
    if (!e->readonly && !app.busy) {
        items |= GRID_MENU_BIT(GRID_MENU_DELETE_ROWS);
        if (cell_editable(row, col)) {
            items |= GRID_MENU_BIT(GRID_MENU_EDIT);
            int is_default = row == e->new_row && !e->new_set[col];
            if (e->info.cols[col].nullable && (is_default || db_cell(e->set, (size_t)row, (unsigned)col)))
                items |= GRID_MENU_BIT(GRID_MENU_SET_NULL);
        }
    }
    return items;
}

static void on_cell_menu_action(grid_menu_item item, int row, int col, void *data) {
    (void)data;
    grid_edit_state *e = &app.edit;
    if (!e->grid || row < 0 || (size_t)row >= e->set->nrows) return;
    switch (item) {
    case GRID_MENU_EDIT:
        begin_cell_edit(row, col, NULL);
        break;
    case GRID_MENU_SET_NULL:
        if (!cell_editable(row, col)) break;
        if (row == e->new_row) {
            set_cell(row, col, NULL);
            e->new_set[col] = 1;
            uiTableModelRowChanged(e->model->model, row);
        } else {
            start_row_update(row, col, NULL, "", -1);
        }
        break;
    case GRID_MENU_COPY: {
        if (col < 0 || (unsigned)col >= e->set->ncols) break;
        const char *v = db_cell(e->set, (size_t)row, (unsigned)col);
        sqlc_copy_to_clipboard(v ? v : "NULL");
        set_status("Copied the value to the clipboard.");
        break;
    }
    case GRID_MENU_DELETE_ROWS:
        delete_selected_rows();
        break;
    default:
        break;
    }
}

/* ---- the page: grid + toolbar ---- */

static uiButton *toolbar_button(uiBox *bar, const char *text, void (*f)(uiButton *, void *)) {
    uiButton *b = uiNewButton(text);
    uiButtonOnClicked(b, f, NULL);
    uiBoxAppend(bar, uiControl(b), 0);
    return b;
}

/* The grid of a browsed table, with + / - / Reload / Delete All Rows below.
 * meta: its columns (sets[0]) and TABLE_TYPE (sets[1]). */
static uiControl *editable_page(set_model *sm, const db_outcome *meta) {
    grid_edit_state *e = &app.edit;
    clear_edit_state();
    e->model = sm;
    e->set = sm->set;

    const db_set *cols = meta->nsets > 0 ? &meta->sets[0] : NULL;
    const db_set *type = meta->nsets > 1 ? &meta->sets[1] : NULL;
    const char *ttype = type && type->nrows ? db_cell(type, 0, 0) : NULL;
    if (is_view(ttype))
        e->readonly = "Views are read-only here: browse the table itself to edit rows.";
    else if (table_info_load(&e->info, cols, sm->set) != 0)
        e->readonly = "This table's columns couldn't be read, so its rows can't be edited.";

    uiTable *t = new_grid(sm);
    e->grid = t;
    uiTableSetSelectionMode(t, uiTableSelectionModeZeroOrMany);
    grid_edit_callbacks cb = {
        .double_clicked = on_cell_double_clicked,
        .committed = on_cell_committed,
        .cancelled = on_cell_cancelled,
        .menu_open = on_cell_menu_open,
        .menu_action = on_cell_menu_action,
    };
    sqlc_grid_enable_editing(t, &cb);

    uiBox *page = uiNewVerticalBox();
    uiBoxSetPadded(page, 1);
    uiBoxAppend(page, uiControl(t), 1);

    uiBox *bar = uiNewHorizontalBox();
    uiBoxSetPadded(bar, 1);
    e->add_btn = toolbar_button(bar, "+", on_add_row);
    e->del_btn = toolbar_button(bar, "\xE2\x88\x92", on_delete_rows); /* "−" */
    e->reload_btn = toolbar_button(bar, "Reload", on_reload_rows);
    e->clear_btn = toolbar_button(bar, "Delete All Rows\xE2\x80\xA6", on_clear_rows);
    const char *hint = e->readonly ? e->readonly
        : e->info.npk ? "Double-click a cell to edit: Return saves, Tab goes to the next column, "
                        "Esc cancels. Right-click for Set to NULL."
        : "Double-click a cell to edit (Return saves, Esc cancels). No primary key: rows are "
          "found by their values, one at a time.";
    uiBoxAppend(bar, uiControl(uiNewLabel(hint)), 0);
    uiBoxAppend(page, uiControl(bar), 0);
    update_edit_buttons();
    return uiControl(page);
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

static void on_saved_selection(uiTable *t, void *data);
static void on_saved_double_click(uiTable *t, int row, void *data);

static void fill_saved_list_now(const char *name) {
    if (app.saved_table) {
        uiBoxDelete(app.saved_box, uiBoxNumChildren(app.saved_box) - 1);
        uiControlDestroy(uiControl(app.saved_table));
        app.saved_table = NULL;
    }
    free_set_model(app.saved_model);
    app.saved_model = NULL;
    db_outcome_free(&app.saved_data);
    app.profile_sel = 0;

    size_t n = app.profiles.n + 1; /* "New connection" first */
    if (n > (size_t)INT_MAX) return;
    db_set *s = calloc(1, sizeof *s);
    if (!s) return;
    app.saved_data.sets = s; /* from here on db_outcome_free cleans up */
    app.saved_data.nsets = 1;
    s->has_rows = 1;
    s->ncols = 1;
    s->names = calloc(1, sizeof *s->names);
    s->numeric = calloc(1, sizeof *s->numeric);
    s->cells = calloc(n, sizeof *s->cells);
    if (!s->names || !s->numeric || !s->cells) { db_outcome_free(&app.saved_data); return; }
    s->nrows = n;
    s->names[0] = strdup("Saved connections");
    s->cells[0] = strdup("New connection");
    for (size_t i = 0; i < app.profiles.n; i++) {
        s->cells[i + 1] = strdup(app.profiles.items[i].name);
        if (name && strcmp(app.profiles.items[i].name, name) == 0) app.profile_sel = (int)i + 1;
    }
    if (!s->names[0]) { db_outcome_free(&app.saved_data); return; }

    app.saved_model = new_set_model(s);
    if (!app.saved_model) { db_outcome_free(&app.saved_data); return; }
    app.saved_table = new_grid(app.saved_model);
    uiTableSetSelectionMode(app.saved_table, uiTableSelectionModeOne);
    sqlc_table_fill_width(app.saved_table); /* column follows the list's width */
    uiTableOnSelectionChanged(app.saved_table, on_saved_selection, NULL);
    uiTableOnRowDoubleClicked(app.saved_table, on_saved_double_click, NULL);
    uiBoxAppend(app.saved_box, uiControl(app.saved_table), 1);
    int row = app.profile_sel;
    uiTableSelection sel = { .NumRows = 1, .Rows = &row };
    uiTableSetSelection(app.saved_table, &sel);
}

/* Refill the saved-connections list and select the profile called name (or
 * "New connection"). Doesn't touch the form. */
static void fill_saved_list(const char *name) {
    app.filling_saved = 1; /* selecting from code calls on_saved_selection */
    fill_saved_list_now(name);
    app.filling_saved = 0;
}

/* Fill the connection fields from the selected saved connection. */
static void apply_selected_profile(void) {
    int sel = app.profile_sel;
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

    uiEntrySetText(app.pass, p->remember_password && p->password ? p->password : "");
}

/* The selected row of the saved-connections list, or -1. */
static int saved_selected_row(uiTable *t) {
    uiTableSelection *sel = uiTableGetSelection(t);
    if (!sel) return -1;
    int row = sel->NumRows == 1 ? sel->Rows[0] : -1;
    uiFreeTableSelection(sel);
    return row >= 0 && (size_t)row <= app.profiles.n ? row : -1;
}

static void on_saved_selection(uiTable *t, void *data) {
    (void)data;
    if (app.filling_saved) return;
    int row = saved_selected_row(t);
    if (row < 0 || row == app.profile_sel) return;
    app.profile_sel = row;
    apply_selected_profile();
}

/* Double-click a saved connection: connect with it, when it has what it needs. */
static void on_saved_double_click(uiTable *t, int row, void *data) {
    (void)t; (void)data;
    if (row <= 0 || (size_t)row > app.profiles.n || app.busy) return;
    if (row != app.profile_sel) {
        app.profile_sel = row;
        apply_selected_profile();
    }
    if (!app.profiles.items[row - 1].remember_password) {
        set_status("Type the password for \"%s\", then press Connect.",
                   app.profiles.items[row - 1].name);
        return;
    }
    on_connect(app.connect_btn, NULL);
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
    p.password = p.remember_password ? entry_dup(app.pass) : NULL;
    int existed = profiles_find(&app.profiles, name) != NULL;

    if (profiles_put(&app.profiles, &p) < 0) {
        set_status("Out of memory");
    } else {
        profiles_set_last(&app.profiles, name);
        if (save_profiles_file() == 0) {
            fill_saved_list(name);
            uiCheckboxSetChecked(app.remember_pw, p.remember_password);
            set_status("%s connection \"%s\".", existed ? "Updated" : "Saved", name);
        }
    }
    free(p.name); free(p.host); free(p.user); free(p.database);
    if (p.password) { memset(p.password, 0, strlen(p.password)); free(p.password); }
}

static void on_delete_profile(uiButton *b, void *data) {
    (void)b; (void)data;
    int sel = app.profile_sel;
    if (sel <= 0 || (size_t)sel > app.profiles.n) {
        set_status("Pick a saved connection to delete.");
        return;
    }
    char *name = strdup(app.profiles.items[sel - 1].name);
    if (!name) return;
    char msg[300];
    snprintf(msg, sizeof msg, "Delete the saved connection \"%s\"? "
             "Its saved password is deleted too.", name);
#ifndef SQLC_AUTOTEST
    if (!sqlc_confirm("Delete saved connection?", msg, "Delete")) { free(name); return; }
#endif
    profiles_remove(&app.profiles, name);
    if (save_profiles_file() == 0) set_status("Deleted connection \"%s\".", name);
    fill_saved_list(NULL);
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
    fill_saved_list(app.profiles.last);
    apply_selected_profile();
}

/* Close the connection and go back to the welcome screen. The editor keeps
 * its text; the results, table list and structure belonged to the server. */
static void on_disconnect(uiButton *b, void *data) {
    (void)b; (void)data;
    if (app.busy || !app.conn) return; /* the worker may be using it */
    int had_new_row = app.edit.new_row >= 0;
    mysql_close(app.conn);
    app.conn = NULL;
    free(app.conn_user);
    free(app.conn_host);
    app.conn_user = app.conn_host = NULL;
    app.conn_port = 0;

    clear_results();
    db_outcome_free(&app.tables_raw);
    app.tables_loaded = 0;
    uiButtonSetText(app.load_tables_btn, "Load tables");
    rebuild_sidebar_keeping(NULL, 0);
    free(app.browsing);
    app.browsing = NULL;
    free(app.pending_browse);
    app.pending_browse = NULL;
    free(app.current_db);
    app.current_db = NULL;
    structure_message("");

    show_screen();
    set_status(had_new_row ? "Disconnected - the unsaved new row was discarded."
                           : "Disconnected.");
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

static void autotest_status(const char *what) {
    char *st = uiLabelText(app.status);
    printf("[%s] status: %s\n", what, st);
    uiFreeText(st);
    fflush(stdout);
}

static void autotest_editor(const char *what) {
    int row, col;
    char *text = NULL;
    if (app.edit.grid && sqlc_grid_editing(app.edit.grid, &row, &col, &text))
        printf("[editor] %s: row %d, column %s, text [%s]%s\n", what, row,
               app.edit.info.cols[col].name, text, sqlc_grid_editor_has_calendar(app.edit.grid) ? ", calendar" : "");
    else
        printf("[editor] %s: closed\n", what);
    free(text);
    fflush(stdout);
}

static void autotest_row_print(const char *what, int row) {
    const db_set *s = app.edit.set;
    printf("[row] %s:", what);
    for (unsigned c = 0; s && row >= 0 && (size_t)row < s->nrows && c < s->ncols; c++) {
        const char *v = db_cell(s, (size_t)row, c);
        int dflt = row == app.edit.new_row && app.edit.new_set && !app.edit.new_set[c];
        printf(" %s=%s", s->names[c], dflt ? "(default)" : v ? v : "NULL");
    }
    printf("\n");
    autotest_status(what);
}

static void autotest_structure(const char *what) {
    char *title = uiLabelText(app.st.title);
    printf("[structure] %s: %s\n", what, title);
    uiFreeText(title);
    const db_set *sets[2] = { app.st.data.nsets > 0 ? &app.st.data.sets[0] : NULL,
                              app.st.data.nsets > 1 ? &app.st.data.sets[1] : NULL };
    for (int k = 0; k < 2; k++) {
        const db_set *t = sets[k];
        for (size_t r = 0; t && r < t->nrows; r++) {
            printf("[structure]   %s:", k ? "index" : "column");
            for (unsigned c = 0; c < t->ncols; c++) {
                const char *v = db_cell(t, r, c);
                printf(" %s=%s", t->names[c], v ? v : "NULL");
            }
            printf("\n");
        }
    }
    printf("[structure] grids: %s, %s\n", app.st.cols_model ? "columns" : "-",
           app.st.idx_model ? "indexes" : "-");
    autotest_status(what);
}

static void autotest_resume(void *data) {
    (void)data;
    autotest_next();
}

static void autotest_next(void) {
    if (app.busy) return; /* a follow-up job is running; its end calls us again */
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
                   "last: %s, list rows: %zu, selected row: %d\n",
                   p ? "yes" : "no", p ? p->database : "-", p ? p->hide_views : -1,
                   p ? p->no_auto_rehash : -1, back.last ? back.last : "-",
                   app.saved_data.nsets ? app.saved_data.sets[0].nrows : 0, app.profile_sel);
            profiles_free(&back);
        }
        on_delete_profile(NULL, NULL);
        printf("[profiles] after delete: %zu saved, list rows: %zu, selected row: %d\n",
               app.profiles.n, app.saved_data.nsets ? app.saved_data.sets[0].nrows : 0,
               app.profile_sel);
        /* sidebar divider: min width, and the table column follows it */
        sqlc_split_set_position(app.split, 400);
        printf("[sidebar] width 400 -> %d, column %d\n",
               sqlc_split_position(app.split), uiTableColumnWidth(app.side_table, 0));
        sqlc_split_set_position(app.split, 50);
        printf("[sidebar] width 50 -> %d (min 120), column %d\n",
               sqlc_split_position(app.split), uiTableColumnWidth(app.side_table, 0));
        sqlc_split_set_position(app.split, 5000);
        printf("[sidebar] width 5000 -> %d (window leaves room for the editor)\n",
               sqlc_split_position(app.split));
        /* editor / results divider: a 20000-row grid must not squeeze the
         * query pane away (the drag limits are checked at the end) */
        printf("[split] editor height after a big result: %d (min 100)\n",
               sqlc_split_position(app.query_split));
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
        /* export the whole database (the dialog is skipped) */
        printf("[export db] current database: %s\n", app.current_db ? app.current_db : "-");
        {
            export_opts o = export_default_opts();
            if (app.current_db) start_export_db(app.current_db, &o, "build/autotest-export-db.sql");
        }
        break;
    case 7:
        autotest_print("export db");
        {
            FILE *f = fopen("build/autotest-export-db.sql", "r");
            int creates = 0, views = 0, inserts = 0;
            char line[4096];
            while (f && fgets(line, sizeof line, f)) {
                creates += strncmp(line, "CREATE TABLE ", 13) == 0;
                views += strncmp(line, "-- View ", 8) == 0;
                inserts += strncmp(line, "INSERT INTO ", 12) == 0;
            }
            if (f) fclose(f);
            printf("[export db] CREATE TABLE: %d, views: %d, INSERT: %d\n", creates, views, inserts);
            remove("build/autotest-export-db.sql");
        }
        /* reconnect with -A (still checked): no table list until asked */
        on_connect(app.connect_btn, NULL);
        break;
    case 8:
        autotest_print("connect -A");
        load_tables_now();
        break;
    case 9:
        autotest_print("load tables");
        /* context menu: work on a scratch table, with -A off so the list follows */
        uiCheckboxSetChecked(app.no_rehash, 0);
        run_sql("DROP TABLE IF EXISTS ctx_tmp; CREATE TABLE ctx_tmp (id INT); "
                "INSERT INTO ctx_tmp VALUES (1), (2);");
        break;
    case 10: {
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
    case 11:
        autotest_print("browse ctx_tmp");
        start_alter(ALTER_RENAME, "ctx_tmp", 0, "ctx `odd` name");
        break;
    case 12: {
        autotest_print("rename");
        char *ed = uiMultilineEntryText(app.editor);
        printf("[rename] editor: %s, browsing: %s\n", ed, app.browsing ? app.browsing : "-");
        uiFreeText(ed);
        start_alter(ALTER_TRUNCATE, "ctx `odd` name", 0, NULL);
        break;
    }
    case 13:
        autotest_print("truncate");
        run_sql("SELECT COUNT(*) FROM `ctx ``odd`` name`");
        break;
    case 14:
        printf("[truncate] rows left: %s\n", db_cell(&app.shown.sets[0], 0, 0));
        on_table_double_click(app.side_table, autotest_row("ctx `odd` name"), NULL);
        break;
    case 15:
        start_alter(ALTER_DROP, "ctx `odd` name", 0, NULL);
        break;
    case 16:
        autotest_print("drop");
        printf("[drop] browsing: %s\n", app.browsing ? app.browsing : "-");
        start_alter(ALTER_DROP, "ctx `odd` name", 0, NULL); /* again: server error */
        break;
    case 17:
        autotest_print("drop again");
        /* views: Truncate is disabled */
        uiCheckboxSetChecked(app.hide_views, 0);
        on_hide_views(app.hide_views, NULL);
        break;
    case 18: {
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
    case 19:
        on_connect(app.connect_btn, NULL);
        break;
    case 20:
        autotest_print("connect tables once");
        run_sql("DROP TABLE IF EXISTS once_tmp; CREATE TABLE once_tmp (id INT);");
        break;
    case 21:
        autotest_print("query, tables once");
        printf("[tables once] once_tmp in sidebar: %s\n", autotest_row("once_tmp") >= 0 ? "yes" : "no");
        load_tables_now();
        break;
    case 22:
        printf("[tables once] after refresh, once_tmp in sidebar: %s\n",
               autotest_row("once_tmp") >= 0 ? "yes" : "no");
        uiCheckboxSetChecked(app.tables_once, 0);
        on_tables_once(app.tables_once, NULL);
        run_sql("DROP TABLE once_tmp;");
        break;
    case 23: {
        /* the Normal / AI / Structure switch: Normal and AI swap the query
         * pane in place above the same results; Structure replaces both */
        show_mode(MODE_AI);
        printf("[view] AI: ai view %d, editor view %d, results %d, structure %d, switch %d\n",
               uiControlVisible(app.ai_view), uiControlVisible(app.normal_view),
               uiControlVisible(app.query_split), uiControlVisible(app.structure_view),
               sqlc_segmented_selected(app.mode_switch));
        show_mode(MODE_NORMAL);
        printf("[view] Normal: ai view %d, editor view %d, results %d, structure %d, switch %d\n",
               uiControlVisible(app.ai_view), uiControlVisible(app.normal_view),
               uiControlVisible(app.query_split), uiControlVisible(app.structure_view),
               sqlc_segmented_selected(app.mode_switch));
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
    case 24:
        autotest_print("ai read-only");
        /* row editing: scratch tables with and without a primary key */
        run_sql("DROP TABLE IF EXISTS edit_tmp; "
                "CREATE TABLE edit_tmp (id INT AUTO_INCREMENT PRIMARY KEY, "
                "name VARCHAR(20) NOT NULL DEFAULT 'anon', qty INT NULL, born DATE NULL, "
                "seen DATETIME NULL, data BLOB NULL); "
                "INSERT INTO edit_tmp (name, qty, born) VALUES ('Ann', 1, '2000-01-01'), "
                "('Bob', 2, NULL), ('Cy', 3, NULL); "
                "DROP TABLE IF EXISTS nopk_tmp; CREATE TABLE nopk_tmp (a INT, b VARCHAR(10)); "
                "INSERT INTO nopk_tmp VALUES (1, 'x'), (1, 'x'), (2, 'y');");
        break;
    case 25:
        browse_table("edit_tmp");
        break;
    case 26: {
        autotest_print("browse edit_tmp");
        grid_edit_state *e = &app.edit;
        printf("[edit] grid: %s, read-only: %s, columns: %u, key columns: %u\n",
               e->grid ? "yes" : "no", e->readonly ? e->readonly : "no", e->info.ncols, e->info.npk);
        on_cell_double_clicked(0, 5, NULL); /* blob: refused */
        autotest_status("blob");
        /* double-click a cell, type, Return */
        on_cell_double_clicked(0, 1, NULL);
        autotest_editor("double-click name");
        sqlc_grid_test_finish(e->grid, "Annie", GRID_END_RETURN);
        break;
    }
    case 27:
        autotest_row_print("after edit", 0);
        /* a date: the calendar, then Tab to the datetime column */
        begin_cell_edit(1, 3, NULL);
        autotest_editor("born (NULL)");
        sqlc_grid_test_pick_date(app.edit.grid, "2024-02-29");
        autotest_editor("picked in calendar");
        sqlc_grid_test_finish(app.edit.grid, "2024-02-29", GRID_END_TAB);
        break;
    case 28:
        autotest_row_print("after date", 1);
        autotest_editor("tabbed to seen");
        sqlc_grid_test_finish(app.edit.grid, "2024-1-5 9:30", GRID_END_RETURN);
        break;
    case 29:
        autotest_row_print("after datetime", 1);
        /* not a number: refused, the editor stays open with the text */
        begin_cell_edit(0, 2, NULL);
        sqlc_grid_test_finish(app.edit.grid, "abc", GRID_END_RETURN);
        uiQueueMain(autotest_resume, NULL); /* runs after the editor's callback */
        break;
    case 30:
        autotest_editor("after bad number");
        sqlc_grid_close_editor(app.edit.grid);
        {
            char menu[16];
            unsigned bits = on_cell_menu_open(0, 2, NULL);
            snprintf(menu, sizeof menu, "%c%c%c%c", bits & GRID_MENU_BIT(GRID_MENU_EDIT) ? 'E' : '-',
                     bits & GRID_MENU_BIT(GRID_MENU_SET_NULL) ? 'N' : '-',
                     bits & GRID_MENU_BIT(GRID_MENU_COPY) ? 'C' : '-',
                     bits & GRID_MENU_BIT(GRID_MENU_DELETE_ROWS) ? 'D' : '-');
            printf("[edit] menu on qty: %s, on name (NOT NULL): %s\n", menu,
                   on_cell_menu_open(0, 1, NULL) & GRID_MENU_BIT(GRID_MENU_SET_NULL) ? "Set to NULL" : "no Set to NULL");
        }
        on_cell_menu_action(GRID_MENU_SET_NULL, 0, 2, NULL);
        break;
    case 31:
        autotest_row_print("after set NULL", 0);
        /* + : a new row, typed in place */
        on_add_row(NULL, NULL);
        autotest_editor("new row");
        autotest_row_print("new row", app.edit.new_row);
        sqlc_grid_test_finish(app.edit.grid, "Dee", GRID_END_TAB);
        uiQueueMain(autotest_resume, NULL);
        break;
    case 32:
        autotest_editor("new row, tab");
        sqlc_grid_test_finish(app.edit.grid, "7", GRID_END_RETURN);
        break;
    case 33: {
        autotest_row_print("inserted", 3);
        printf("[edit] unsaved row: %d, rows: %zu\n", app.edit.new_row, app.edit.set->nrows);
        /* select rows 1 and 3 (Annie, Cy) and delete them */
        int pick[] = { 0, 2 };
        uiTableSelection sel = { .NumRows = 2, .Rows = pick };
        uiTableSetSelection(app.edit.grid, &sel);
        delete_selected_rows();
        break;
    }
    case 34: {
        autotest_status("delete 2");
        for (size_t r = 0; r < app.edit.set->nrows; r++) autotest_row_print("left", (int)r);
        /* Cancel in the "are you sure?" keeps the rows */
        autotest_confirm = 0;
        int pick = 0;
        uiTableSelection sel = { .NumRows = 1, .Rows = &pick };
        uiTableSetSelection(app.edit.grid, &sel);
        delete_selected_rows();
        autotest_confirm = 1;
        printf("[edit] after cancelled delete: %zu rows, busy: %d\n", app.edit.set->nrows, app.busy);
        /* an unsaved new row is just discarded */
        on_add_row(NULL, NULL);
        sqlc_grid_close_editor(app.edit.grid);
        pick = app.edit.new_row;
        uiTableSetSelection(app.edit.grid, &sel);
        delete_selected_rows();
        printf("[edit] after discarding the new row: %zu rows, unsaved row: %d, busy: %d\n",
               app.edit.set->nrows, app.edit.new_row, app.busy);
        on_clear_rows(NULL, NULL); /* truncate, then the grid reloads */
        break;
    }
    case 35:
        autotest_status("delete all");
        printf("[edit] rows: %zu, grid editable: %s\n", app.edit.set ? app.edit.set->nrows : 0,
               app.edit.grid && !app.edit.readonly ? "yes" : "no");
        /* a row with only defaults */
        on_add_row(NULL, NULL);
        sqlc_grid_test_finish(app.edit.grid, "", GRID_END_RETURN);
        break;
    case 36:
        autotest_row_print("defaults", 0);
        browse_table("nopk_tmp");
        break;
    case 37:
        printf("[nopk] key columns: %u, read-only: %s\n", app.edit.info.npk,
               app.edit.readonly ? app.edit.readonly : "no");
        begin_cell_edit(1, 1, NULL);
        sqlc_grid_test_finish(app.edit.grid, "z", GRID_END_RETURN);
        break;
    case 38:
        autotest_status("nopk edit");
        run_sql("SELECT GROUP_CONCAT(b ORDER BY b) FROM nopk_tmp");
        break;
    case 39:
        printf("[nopk] values now: %s\n", db_cell(&app.shown.sets[0], 0, 0));
        browse_table("customer_totals");
        break;
    case 40:
        printf("[view] grid: %s, read-only: %s\n", app.edit.grid ? "yes" : "no",
               app.edit.readonly ? app.edit.readonly : "no");
        on_cell_double_clicked(0, 0, NULL);
        printf("[view] editor opened: %s\n", sqlc_grid_editing(app.edit.grid, NULL, NULL, NULL) ? "yes" : "no");
        run_sql("DROP TABLE edit_tmp; DROP TABLE nopk_tmp;");
        break;
    case 41: {
        autotest_print("cleanup");
        /* sidebar multi-select: two rows, kept across a rebuild, and the
         * Structure page asks for one */
        {
            int a = autotest_row("customers"), b = autotest_row("orders");
            int two[2] = { a < b ? a : b, a < b ? b : a };
            if (two[0] >= 0) {
                uiTableSelection sel2 = { .NumRows = 2, .Rows = two };
                uiTableSetSelection(app.side_table, &sel2);
            }
            rebuild_sidebar();
            char **names = NULL;
            size_t n = 0;
            int rc = selected_tables(&names, &n);
            printf("[multi] selected: %zu (rc %d):", n, rc);
            for (size_t i = 0; i < n; i++) printf(" [%s]", names[i]);
            printf(", single: %s\n", selected_table(NULL) ? selected_table(NULL) : "-");
            free_names(names, n);
            show_mode(MODE_STRUCTURE);
            char *title = uiLabelText(app.st.title);
            printf("[multi] structure: %s, loading: %s\n", title,
                   app.busy && app.job_kind == JOB_STRUCTURE ? "yes" : "no");
            uiFreeText(title);
            show_mode(MODE_NORMAL);
        }
        /* Structure page: select a table, switch to it */
        int row = autotest_row("orders");
        uiTableSelection sel = { .NumRows = 1, .Rows = &row };
        uiTableSetSelection(app.side_table, &sel);
        show_mode(MODE_STRUCTURE);
        printf("[structure] switch: %d, page shown: %d, query page shown: %d, loading: %s\n",
               sqlc_segmented_selected(app.mode_switch), uiControlVisible(app.structure_view),
               uiControlVisible(app.query_split),
               app.busy && app.job_kind == JOB_STRUCTURE ? "yes" : "no");
        break;
    }
    case 42: {
        autotest_structure("orders");
        char menu[512];
        int row = autotest_row("customers");
        sqlc_table_menu_describe(app.side_table, row, menu, sizeof menu);
        printf("[structure] menu: %s\n", menu);
        on_table_menu_action(TABLE_MENU_STRUCTURE, row, NULL); /* loads customers */
        break;
    }
    case 43: {
        autotest_structure("menu");
        /* the same table again: nothing to load */
        structure_sync(); /* what the queued selection callback does */
        printf("[structure] reselect same table, loading: %s\n", app.busy ? "yes" : "no");
        /* a view */
        int row = autotest_row("customer_totals");
        if (row < 0) { /* views hidden earlier: bring them back first */
            uiCheckboxSetChecked(app.hide_views, 0);
            on_hide_views(app.hide_views, NULL);
            autotest_step--; /* come back here once the list is reloaded */
            break;
        }
        uiTableSelection sel = { .NumRows = 1, .Rows = &row };
        uiTableSetSelection(app.side_table, &sel);
        structure_sync(); /* what the queued selection callback does */
        break;
    }
    case 44: {
        autotest_structure("view");
        /* double-click while on Structure: the click loads the structure,
         * the double-click browses once that's done */
        int row = autotest_row("customers");
        uiTableSelection sel = { .NumRows = 1, .Rows = &row };
        uiTableSetSelection(app.side_table, &sel);
        structure_sync(); /* what the queued selection callback does */
        on_table_double_click(app.side_table, row, NULL);
        printf("[structure] double-click while loading, queued: %s\n",
               app.pending_browse ? app.pending_browse : "-");
        break;
    }
    case 45:
        autotest_print("browse after structure");
        printf("[structure] mode: %s, browsing: %s, sidebar selection kept: %s\n",
               app.mode == MODE_NORMAL ? "Normal" : app.mode == MODE_AI ? "AI" : "Structure",
               app.browsing ? app.browsing : "-",
               selected_table(NULL) ? selected_table(NULL) : "-");
        /* the editor / results divider */
        sqlc_split_set_position(app.query_split, 300);
        printf("[split] editor height: %d\n", sqlc_split_position(app.query_split));
        sqlc_split_set_position(app.query_split, 5);
        printf("[split] dragged to the top: %d (min 100)\n", sqlc_split_position(app.query_split));
        sqlc_split_set_position(app.query_split, 5000);
        printf("[split] dragged to the bottom: editor %d (results keep their minimum)\n",
               sqlc_split_position(app.query_split));
        sqlc_split_set_position(app.query_split, 200);
        /* ⌘↩ on Structure runs nothing (the editor is hidden), only a hint;
         * customers' structure is already shown, so nothing loads either */
        show_mode(MODE_STRUCTURE);
        on_run_menu();
        printf("[structure] cmd-return on Structure: busy %d, results page shown %d\n",
               app.busy, uiControlVisible(app.query_split));
        /* Disconnect: back to the welcome screen, nothing of the server left */
        on_disconnect(app.disconnect_btn, NULL);
        printf("[welcome] after disconnect: connected %d, welcome shown %d, layout shown %d, "
               "sidebar rows %zu, result sets %zu\n",
               app.conn != NULL, uiControlVisible(app.welcome), uiControlVisible(app.workspace),
               app.side_data.nsets ? app.side_data.sets[0].nrows : 0, app.shown.nsets);
        autotest_status("disconnect");
        fflush(stdout);
        if (on_should_quit(NULL)) uiQuit();
        break;
    }
}

static void autotest_start(void *data) {
    (void)data;
    const char *db = getenv("SQLC_TEST_DB");
    printf("[welcome] at launch: welcome shown %d, layout shown %d\n",
           uiControlVisible(app.welcome), uiControlVisible(app.workspace));
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

static void build_window(void) {
    uiMenu *file = uiNewMenu("File");
    uiMenuItemOnClicked(uiMenuAppendItem(file, "Export\xE2\x80\xA6"), on_export_menu, NULL);
    uiMenuItemOnClicked(uiMenuAppendItem(file, "Export Database\xE2\x80\xA6"), on_export_db_menu, NULL);
    uiMenuAppendSeparator(file);
    uiMenuAppendQuitItem(file);
    uiMenu *ai = uiNewMenu("AI");
    uiMenuItemOnClicked(uiMenuAppendItem(ai, "Ask Claude"), on_ask_claude_menu, NULL);
    uiMenuAppendSeparator(ai);
    uiMenuItemOnClicked(uiMenuAppendItem(ai, "Set Claude API Key\xE2\x80\xA6"), on_set_api_key, NULL);
    uiMenuItemOnClicked(uiMenuAppendItem(ai, "Remove Claude API Key"), on_remove_api_key, NULL);
    uiOnShouldQuit(on_should_quit, NULL);

    app.win = uiNewWindow("sqlc", 1100, 720, 1);
    uiWindowSetMargined(app.win, 1);
    uiWindowOnClosing(app.win, on_closing, NULL);

    uiBox *root = uiNewVerticalBox();
    uiBoxSetPadded(root, 1);
    uiWindowSetChild(app.win, uiControl(root));

    /* ----- welcome screen: saved connections | new connection form ----- */
    app.saved_box = uiNewVerticalBox(); /* fill_saved_list puts the list in it */

    app.profile_name = uiNewEntry();
    app.host = uiNewEntry();  uiEntrySetText(app.host, "localhost");
    app.port = uiNewEntry();  uiEntrySetText(app.port, "3306");
    app.user = uiNewEntry();
    app.pass = uiNewPasswordEntry();
    app.db   = uiNewEntry();
    const char *login = getenv("USER");
    if (login) uiEntrySetText(app.user, login);
    app.remember_pw = uiNewCheckbox("Remember password");
    uiCheckboxSetChecked(app.remember_pw, 1);
    /* connection options, set before connecting (saved with profiles) */
    app.hide_views = uiNewCheckbox("Hide views");
    uiCheckboxOnToggled(app.hide_views, on_hide_views, NULL);
    app.no_rehash = uiNewCheckbox("No auto-rehash (-A)");
    app.tables_once = uiNewCheckbox("Tables only, load once");
    uiCheckboxOnToggled(app.tables_once, on_tables_once, NULL);

    uiForm *form = uiNewForm();
    uiFormSetPadded(form, 1);
    uiFormAppend(form, "Name", uiControl(app.profile_name), 0);
    uiFormAppend(form, "Host", uiControl(app.host), 0);
    uiFormAppend(form, "Port", uiControl(app.port), 0);
    uiFormAppend(form, "User", uiControl(app.user), 0);
    uiFormAppend(form, "Password", uiControl(app.pass), 0);
    uiFormAppend(form, "Database", uiControl(app.db), 0);
    uiFormAppend(form, "", uiControl(app.remember_pw), 0);
    uiFormAppend(form, "", uiControl(app.hide_views), 0);
    uiFormAppend(form, "", uiControl(app.no_rehash), 0);
    uiFormAppend(form, "", uiControl(app.tables_once), 0);

    uiBox *form_buttons = uiNewHorizontalBox();
    uiBoxSetPadded(form_buttons, 1);
    uiButton *save_btn = uiNewButton("Save");
    uiButtonOnClicked(save_btn, on_save_profile, NULL);
    uiButton *delete_btn = uiNewButton("Delete");
    uiButtonOnClicked(delete_btn, on_delete_profile, NULL);
    app.connect_btn = uiNewButton("Connect");
    uiButtonOnClicked(app.connect_btn, on_connect, NULL);
    uiBoxAppend(form_buttons, uiControl(save_btn), 0);
    uiBoxAppend(form_buttons, uiControl(delete_btn), 0);
    uiBoxAppend(form_buttons, uiControl(uiNewHorizontalBox()), 1); /* pushes Connect right */
    uiBoxAppend(form_buttons, uiControl(app.connect_btn), 0);

    /* the form in the middle: empty stretchy boxes around it */
    uiBox *form_col = uiNewVerticalBox();
    uiBoxSetPadded(form_col, 1);
    uiBoxAppend(form_col, uiControl(uiNewVerticalBox()), 1);
    uiBoxAppend(form_col, uiControl(uiNewLabel("Connect to a MySQL or MariaDB server")), 0);
    uiBoxAppend(form_col, uiControl(form), 0);
    uiBoxAppend(form_col, uiControl(form_buttons), 0);
    /* spinner + tip while connecting; takes its space all the time, so the
     * form doesn't move when it starts */
    app.loading = sqlc_new_loading();
    uiBoxAppend(form_col, app.loading, 0);
    uiBoxAppend(form_col, uiControl(uiNewVerticalBox()), 1);
    app.form_col = uiControl(form_col);
    uiBox *center = uiNewHorizontalBox();
    uiBoxAppend(center, uiControl(uiNewHorizontalBox()), 1);
    uiBoxAppend(center, uiControl(form_col), 1);
    uiBoxAppend(center, uiControl(uiNewHorizontalBox()), 1);

    app.welcome = sqlc_new_sidebar_split(uiControl(app.saved_box), uiControl(center),
                                         240, 160, "WelcomeListWidth");
    uiBoxAppend(root, app.welcome, 1);

    /* ----- full layout, once connected ----- */
    uiBox *workspace = uiNewVerticalBox();
    uiBoxSetPadded(workspace, 1);

    /* connected bar: where we are, Hide views (it filters the live sidebar),
     * Disconnect */
    uiBox *bar = uiNewHorizontalBox();
    uiBoxSetPadded(bar, 1);
    app.conn_label = uiNewLabel("");
    app.hide_views_bar = uiNewCheckbox("Hide views");
    uiCheckboxOnToggled(app.hide_views_bar, on_hide_views_bar, NULL);
    app.disconnect_btn = uiNewButton("Disconnect");
    uiButtonOnClicked(app.disconnect_btn, on_disconnect, NULL);
    sqlc_button_set_small(app.disconnect_btn);
    uiBoxAppend(bar, uiControl(app.conn_label), 1);
    uiBoxAppend(bar, uiControl(app.hide_views_bar), 0);
    uiBoxAppend(bar, uiControl(app.disconnect_btn), 0);
    uiBoxAppend(workspace, uiControl(bar), 0);
    uiBoxAppend(workspace, uiControl(uiNewHorizontalSeparator()), 0);

    /* main area: sidebar | editor + results, with a draggable divider */
    app.side_box = uiNewVerticalBox();
    uiBoxSetPadded(app.side_box, 1);
    app.load_tables_btn = uiNewButton("Load tables");
    uiButtonOnClicked(app.load_tables_btn, on_load_tables, NULL);
    uiBoxAppend(app.side_box, uiControl(app.load_tables_btn), 0);
    app.export_btn = uiNewButton("Export\xE2\x80\xA6");
    uiButtonOnClicked(app.export_btn, on_export, NULL);
    uiBoxAppend(app.side_box, uiControl(app.export_btn), 0);
    /* spinner + tip while the list loads; before the list, which
     * rebuild_sidebar_keeping replaces as the box's last child */
    app.side_loading = sqlc_new_loading();
    uiControlHide(app.side_loading);
    uiBoxAppend(app.side_box, app.side_loading, 0);

    /* right side: a slim bar with the Normal | AI | Structure switch at its
     * right edge, above one page at a time */
    uiBox *main_box = uiNewVerticalBox();
    uiBoxSetPadded(main_box, 1);
    uiBox *switch_row = uiNewHorizontalBox();
    uiBoxSetPadded(switch_row, 1);
    static const char *const modes[] = { "Normal", "AI", "Structure" }; /* MODE_* order */
    app.mode_switch = sqlc_new_segmented(modes, 3);
    sqlc_segmented_on_changed(app.mode_switch, on_mode_changed, NULL);
    uiBoxAppend(switch_row, uiControl(uiNewHorizontalBox()), 1); /* pushes it right */
    uiBoxAppend(switch_row, app.mode_switch, 0);
    uiBoxAppend(main_box, uiControl(switch_row), 0);

    app.split = sqlc_new_sidebar_split(uiControl(app.side_box), uiControl(main_box),
                                       220, 120, "SidebarWidth");
    uiBoxAppend(workspace, app.split, 1);
    app.workspace = uiControl(workspace);
    uiBoxAppend(root, app.workspace, 1); /* shown until main() has laid it out */

    /* Normal / AI page: the query pane above, results below, with a draggable
     * divider. The query pane has two views in the same space, one shown at
     * a time: Normal (SQL editor + run row) and AI (Ask Claude). */
    uiBox *editor_pane = uiNewVerticalBox();
    uiBoxSetPadded(editor_pane, 1);

    /* Normal view: the SQL editor and the run row */
    uiBox *normal = uiNewVerticalBox();
    uiBoxSetPadded(normal, 1);
    app.editor = uiNewNonWrappingMultilineEntry();
    uiMultilineEntrySetText(app.editor, "SHOW TABLES;");
    uiBoxAppend(normal, uiControl(app.editor), 1);

    uiBox *run_row = uiNewHorizontalBox();
    uiBoxSetPadded(run_row, 1);
    app.run_btn = uiNewButton("Run (\xE2\x8C\x98\xE2\x86\xA9)"); /* "Run (⌘↩)" */
    uiButtonOnClicked(app.run_btn, on_run, NULL);
    uiBoxAppend(run_row, uiControl(app.run_btn), 0);
    uiBoxAppend(run_row, uiControl(uiNewLabel("Double-click a table to browse it. "
                                              "Drag the line below to resize.")), 0);
    uiBoxAppend(normal, uiControl(run_row), 0);
    app.normal_view = uiControl(normal);
    uiBoxAppend(editor_pane, app.normal_view, 1);

    /* AI view: describe it, get SQL (with an explanation) in the editor */
    uiBox *ai_box = uiNewVerticalBox();
    uiBoxSetPadded(ai_box, 1);
    uiBoxAppend(ai_box, uiControl(uiNewLabel("Describe the query you need (e.g. top 10 customers "
                                             "by total order value last month)")), 0);
    app.ai_task = uiNewMultilineEntry();
    uiBoxAppend(ai_box, uiControl(app.ai_task), 1);
    uiBox *ai_row = uiNewHorizontalBox();
    uiBoxSetPadded(ai_row, 1);
    app.ai_btn = uiNewButton("Ask Claude (\xE2\x8C\x98\xE2\x86\xA9)"); /* "Ask Claude (⌘↩)" */
    uiButtonOnClicked(app.ai_btn, on_ask_claude, NULL);
    uiBoxAppend(ai_row, uiControl(app.ai_btn), 0);
    uiBoxAppend(ai_row, uiControl(uiNewLabel("The SQL appears in the editor (Normal view); "
                                             "nothing runs until you press Run.")), 0);
    uiBoxAppend(ai_box, uiControl(ai_row), 0);
    app.ai_view = uiControl(ai_box);
    uiControlHide(app.ai_view);
    uiBoxAppend(editor_pane, app.ai_view, 1);

    /* Half and half to start (0 = half); 100 points each at least, so a
     * big result grid can't squeeze the editor away, or the other way round. */
    app.results_box = uiNewVerticalBox(); /* the pane stays, empty, until a query runs */
    app.query_split = sqlc_new_stacked_split(uiControl(editor_pane), uiControl(app.results_box),
                                             0, 100, 100, "QueryEditorHeight");
    uiBoxAppend(main_box, app.query_split, 1);

    /* Structure page */
    uiBox *structure = uiNewVerticalBox();
    uiBoxSetPadded(structure, 1);
    uiBox *st_row = uiNewHorizontalBox();
    uiBoxSetPadded(st_row, 1);
    app.st.title = uiNewLabel("");
    app.st.refresh_btn = uiNewButton("Refresh");
    uiButtonOnClicked(app.st.refresh_btn, on_structure_refresh, NULL);
    uiBoxAppend(st_row, uiControl(app.st.title), 1);
    uiBoxAppend(st_row, uiControl(app.st.refresh_btn), 0);
    uiBoxAppend(structure, uiControl(st_row), 0);

    uiBox *cols_pane = uiNewVerticalBox();
    uiBoxSetPadded(cols_pane, 1);
    uiBoxAppend(cols_pane, uiControl(uiNewLabel("Columns")), 0);
    app.st.cols_box = uiNewVerticalBox();
    uiBoxAppend(cols_pane, uiControl(app.st.cols_box), 1);
    uiBox *idx_pane = uiNewVerticalBox();
    uiBoxSetPadded(idx_pane, 1);
    uiBoxAppend(idx_pane, uiControl(uiNewLabel("Indexes")), 0);
    app.st.idx_box = uiNewVerticalBox();
    uiBoxAppend(idx_pane, uiControl(app.st.idx_box), 1);
    uiBoxAppend(structure, sqlc_new_stacked_split(uiControl(cols_pane), uiControl(idx_pane),
                                                  280, 80, 80, "StructureColumnsHeight"), 1);
    app.structure_view = uiControl(structure);
    uiControlHide(app.structure_view);
    uiBoxAppend(main_box, app.structure_view, 1);
    app.mode = MODE_NORMAL;

    app.status = uiNewLabel("Not connected.");
    uiBoxAppend(root, uiControl(app.status), 0);

    app.edit.new_row = -1;
    app.last_tip = SIZE_MAX; /* no tip shown yet */
    rebuild_sidebar();
    load_profiles();
    /* Lay out the full layout once while the window is still off screen, so
     * its splits place their dividers (restoring the saved sizes) before
     * anything is drawn, then switch to the welcome screen. */
    uiControlHide(app.welcome);
    sqlc_window_layout_now(app.win, 0);
    show_screen();
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
        table_info_free(&app.edit.info);
        free(app.edit.new_set);
        db_outcome_free(&app.shown);
        db_outcome_free(&app.side_data);
        db_outcome_free(&app.tables_raw);
        free(app.side_index);
        free(app.menu_table);
        free(app.browsing);
        free(app.current_db);
        free(app.pending_browse);
        free(app.st.table);
        free_set_model(app.st.cols_model);
        free_set_model(app.st.idx_model);
        db_outcome_free(&app.st.data);
        free_set_model(app.saved_model);
        db_outcome_free(&app.saved_data);
        free(app.conn_user);
        free(app.conn_host);
        profiles_free(&app.profiles);
        free(app.profiles_file);
        if (app.conn) mysql_close(app.conn);
        uiUninit();
    }
    mysql_library_end();
    curl_global_cleanup();
    return 0;
}
