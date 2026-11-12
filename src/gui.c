/*
 * sqlc-gui - a simple MySQL/MariaDB GUI built with libui-ng.
 *
 * One window with tabs (File > New Tab, Cmd+T). Each tab is a workspace with
 * its own connection and jobs; its page has two screens (one shown at a time):
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
#include <stdatomic.h>
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
#include "browse.h"
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
#include "sidebar.h"
#include "table_menu.h"
#include "profiles.h"
#include "tabbar.h"
#include "tabs.h"

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

/* The grid of a browsed table, or of a query's rows from one table, when
 * its rows can be edited. */
typedef struct {
    uiTable *grid;           /* NULL = the results can't be edited */
    char *table;             /* the table the edits go to */
    char *query;             /* a query's rows: its SQL, run again by Reload;
                              * NULL for a browsed table (t->browsing) */
    set_model *model;
    db_set *set;             /* t->shown.sets[0] */
    table_info info;
    const char *readonly;    /* why it can't be edited, or NULL */
    int new_row;             /* the unsaved new row, or -1 */
    unsigned char *new_set;  /* ... its columns typed into */
    uiButton *add_btn, *del_btn, *reload_btn, *clear_btn;
    /* the page bar under a browsed table's grid (all NULL for a query's rows) */
    uiButton *prev_btn, *next_btn;
    uiLabel *page_label;
} grid_edit_state;

/* One tab: a workspace with its own connection (welcome screen until it
 * connects). Everything here belongs to that tab and is only touched on the
 * UI thread. A closed tab is freed once no job or queued callback still
 * points at it (see jobs, tab_release). */
typedef struct tab {
    uiControl *page;         /* the tab's root control, in app.pages */
    char *profile;           /* saved connection it connected with, or NULL */
    int jobs;                /* outstanding jobs and queued callbacks using it */
    int closed;              /* closed: no widgets left, freed when jobs is 0 */
    int sync_queued;         /* structure_sync_later is queued */

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
    uiControl *side_loading; /* spinner + tip in the sidebar, while refreshing */
    int refreshing;          /* a table-list job is running */
    uiEntry *profile_name;
    uiCheckbox *remember_pw;
    uiButton *connect_btn, *run_btn, *load_tables_btn, *export_btn;
    uiCheckbox *hide_views;     /* views left out of the sidebar and SHOW TABLES */
    uiCheckbox *no_rehash;      /* -A: only load the table list on request */
    uiCheckbox *tables_once;    /* tables only, loaded on connect, then like -A */

    /* the connected bar above the full layout */
    uiLabel *conn_label;
    uiCheckbox *hide_views_bar; /* mirrors hide_views, which stays the real one */
    uiButton *disconnect_btn;
    /* The login the connection was opened with, kept for the session so an
     * export can open its own connection (see forget_login). */
    char *conn_user, *conn_host, *conn_pass, *conn_db; /* conn_db: NULL = none */
    unsigned int conn_port;

    uiMultilineEntry *editor;
    uiLabel *status;

    /* The filter bar above the editor (Normal view), for the browsed table:
     * column, operator, value, Apply, Reset. Enabled only while browsing. */
    uiCombobox *filter_col, *filter_op;
    uiEntry *filter_value;
    uiButton *filter_apply, *filter_reset;
    char **filter_cols;      /* the column dropdown's items (malloc'd copies) */
    size_t nfilter_cols;
    size_t filter_default;   /* its default item: the first key column, else 0 */
    /* What the browse shows (see start_browse): the applied filter
     * (applied_col NULL = none), the page's first row, and from the last
     * browse read: a next page exists, the table has no primary key (pages
     * aren't in a fixed order), the key columns it was ordered by. */
    char *applied_col, *applied_value;
    browse_op applied_op;
    unsigned long long page_offset;
    int page_has_next, page_keyless;
    char **page_keys;
    size_t npage_keys;

    /* next to the status line while an export runs */
    uiProgressBar *export_bar;
    uiButton *cancel_export_btn;
    /* The running export, or NULL (UI thread only). At most one; it runs on
     * its own connection, beside the main connection's jobs (busy). */
    struct job *export_job;
    int confirming_cancel;   /* the "Cancel the export?" alert is open */
    char *pending_title, *pending_msg; /* finish alert held back until it closes */
    int pending_failed;

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
    uiEntry *side_search;    /* filters the list by table name as you type */
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
    int busy;                /* a job runs on conn (exports don't count) */
    int job_kind;            /* of the running job, while busy */
    /* The server closed the connection (idle timeout, restart, killed):
     * conn is NULL, but the layout stays (editor, results, sidebar, the
     * browsed table) and Reconnect, next to the status line, opens a new
     * connection with the kept login. */
    int lost;
    uiButton *reconnect_btn;
} tab;

/* What all tabs share. */
static struct {
    uiWindow *win;
    uiControl *tabbar;       /* the tab strip (tabbar.h), one item per tab */
    uiBox *pages;            /* every tab's page, in list order; only the
                              * selected one is shown */
    tab **list;
    size_t ntabs;
    profile_list profiles;   /* saved connections, the same in every tab */
    char *profiles_file;
    size_t last_tip;         /* index of the tip shown last time, or SIZE_MAX */
    int quitting;            /* quit asked for: waiting for the exports to stop */
    int exports;             /* exports running, in all tabs (closed ones too) */
    size_t closed_tabs;      /* closed tabs not freed yet (a job still runs) */
    int gone;                /* the window is destroyed (quitting): no widget left */
} app;

/* The tab in front, which menus and shortcuts act on. */
static tab *cur_tab(void) {
    int i = sqlc_tabbar_selected(app.tabbar);
    return i >= 0 && (size_t)i < app.ntabs ? app.list[i] : app.list[0];
}

/* Drop the kept login: the password is zeroed before it is freed. */
static void forget_login(tab *t) {
    if (t->conn_pass) {
        memset(t->conn_pass, 0, strlen(t->conn_pass));
        free(t->conn_pass);
    }
    free(t->conn_user);
    free(t->conn_host);
    free(t->conn_db);
    t->conn_user = t->conn_host = t->conn_pass = t->conn_db = NULL;
    t->conn_port = 0;
}

static void free_strings(char **v, size_t n) {
    for (size_t i = 0; v && i < n; i++) free(v[i]);
    free(v);
}

/* The browse's applied filter, page and key columns (memory only). */
static void free_browse_state(tab *t) {
    free(t->applied_col);
    free(t->applied_value);
    t->applied_col = t->applied_value = NULL;
    t->applied_op = BROWSE_EQ;
    t->page_offset = 0;
    t->page_has_next = t->page_keyless = 0;
    free_strings(t->page_keys, t->npage_keys);
    t->page_keys = NULL;
    t->npage_keys = 0;
}

/* Free a tab's memory and close its connection. Only once its widgets are
 * gone (closed, or the window destroyed: the models outlive their grids)
 * and nothing still points at it (jobs == 0). */
static void tab_free(tab *t) {
    for (size_t i = 0; i < t->nmodels; i++) free_set_model(t->models[i]);
    free(t->models);
    free_set_model(t->side_model);
    table_info_free(&t->edit.info);
    free(t->edit.new_set);
    db_outcome_free(&t->shown);
    db_outcome_free(&t->side_data);
    db_outcome_free(&t->tables_raw);
    free(t->side_index);
    free(t->menu_table);
    free(t->browsing);
    free_browse_state(t);
    free_strings(t->filter_cols, t->nfilter_cols);
    free(t->current_db);
    free(t->pending_browse);
    free(t->pending_title);
    free(t->pending_msg);
    free(t->st.table);
    free_set_model(t->st.cols_model);
    free_set_model(t->st.idx_model);
    db_outcome_free(&t->st.data);
    free_set_model(t->saved_model);
    db_outcome_free(&t->saved_data);
    free(t->profile);
    forget_login(t);
    if (t->conn) mysql_close(t->conn);
    free(t);
}

/* A job or queued callback of t is over: frees a closed tab when it was the
 * last one. t must not be used afterwards unless it is still open. */
static void tab_release(tab *t) {
    if (--t->jobs == 0 && t->closed) {
        app.closed_tabs--;
        tab_free(t);
    }
}

/* Quit asked for and every export (all tabs) has stopped: quit now. The
 * window goes away first (as in on_should_quit), then uiMain returns and
 * main() frees the rest. Returns 1 if quitting (touch no widget then). */
static int quit_if_done(void) {
    if (!app.quitting || app.exports > 0) return 0;
    app.quitting = 0;
    app.gone = 1;
    uiControlDestroy(uiControl(app.win));
    uiQuit();
    return 1;
}

static void set_status(tab *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_status(tab *t, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    uiLabelSetText(t->status, buf);
}

static void show_error(const char *title, const char *msg) {
#ifdef SQLC_AUTOTEST
    printf("[dialog] %s: %s\n", title, msg);
#else
    uiMsgBoxError(app.win, title, msg);
#endif
}

static void enable_if(uiControl *c, int on) {
    if (on) uiControlEnable(c);
    else uiControlDisable(c);
}

/* The filter bar above the editor and the page bar under a browsed
 * table's grid: usable only while a table is browsed (not for the user's
 * own query), connected and idle. */
static void update_browse_bars(tab *t) {
    int on = t->browsing && t->conn && !t->busy;
    enable_if(uiControl(t->filter_col), on && t->nfilter_cols > 0);
    enable_if(uiControl(t->filter_op), on);
    enable_if(uiControl(t->filter_apply), on);
    int op = uiComboboxSelected(t->filter_op);
    enable_if(uiControl(t->filter_value), on && (op < 0 || browse_op_takes_value((browse_op)op)));
    enable_if(uiControl(t->filter_reset), on && (t->applied_col || t->page_offset > 0));

    grid_edit_state *e = &t->edit;
    if (!e->page_label) return;
    enable_if(uiControl(e->prev_btn), on && t->page_offset > 0);
    enable_if(uiControl(e->next_btn), on && t->page_has_next);
    /* the rows on screen: after deleting some, fewer than were read */
    size_t n = e->set ? e->set->nrows - (e->new_row >= 0 ? 1 : 0) : 0;
    char label[160];
    browse_page_label(label, sizeof label, t->page_offset, n);
    if (t->page_keyless && (t->page_offset > 0 || t->page_has_next)) {
        size_t l = strlen(label);
        snprintf(label + l, sizeof label - l, " (no primary key: order not guaranteed)");
    }
    uiLabelSetText(e->page_label, label);
}

/* Toolbar under a browsed table's grid. */
static void update_edit_buttons(tab *t) {
    grid_edit_state *e = &t->edit;
    if (e->grid) {
        int can = !t->busy && !e->readonly;
        uiButton *editing[] = { e->add_btn, e->del_btn, e->clear_btn };
        for (int i = 0; i < 3; i++) enable_if(uiControl(editing[i]), can);
        enable_if(uiControl(e->reload_btn), !t->busy);
    }
    update_browse_bars(t);
}

/* No table is browsed any more (the user's own SQL, Claude's, a drop, a
 * new connection, Disconnect): forget it with its filter and page, and
 * empty the filter bar (disabled until the next browse fills it). */
static void browse_forget(tab *t) {
    free(t->browsing);
    t->browsing = NULL;
    free_browse_state(t);
    free_strings(t->filter_cols, t->nfilter_cols);
    t->filter_cols = NULL;
    t->nfilter_cols = 0;
    t->filter_default = 0;
    uiComboboxClear(t->filter_col);
    uiComboboxSetSelected(t->filter_op, BROWSE_EQ);
    uiEntrySetText(t->filter_value, "");
    update_browse_bars(t);
}

static void set_connecting(tab *t, int on);
static void set_refreshing(tab *t, int on);

/* A job on the main connection starts (1) or ends (0). Exports run on their
 * own connection and don't go through here, so Export… stays enabled (it
 * refuses a second export with a status message, see export_running). */
static void set_busy(tab *t, int busy) {
    t->busy = busy;
    if (!busy) {
        set_connecting(t, 0);
        set_refreshing(t, 0);
    }
    update_edit_buttons(t);
    if (busy) {
        uiControlDisable(uiControl(t->reconnect_btn));
        uiControlDisable(uiControl(t->connect_btn));
        uiControlDisable(uiControl(t->disconnect_btn));
        uiControlDisable(uiControl(t->run_btn));
        uiControlDisable(uiControl(t->load_tables_btn));
        uiControlDisable(uiControl(t->ai_btn));
        uiControlDisable(uiControl(t->st.refresh_btn));
    } else {
        uiControlEnable(uiControl(t->reconnect_btn));
        uiControlEnable(uiControl(t->connect_btn));
        uiControlEnable(uiControl(t->disconnect_btn));
        uiControlEnable(uiControl(t->run_btn));
        uiControlEnable(uiControl(t->load_tables_btn));
        uiControlEnable(uiControl(t->ai_btn));
        uiControlEnable(uiControl(t->st.refresh_btn));
    }
}

/* Load the table list on connect? Not with -A, but "Tables only, load once" does. */
static int load_on_connect(tab *t) {
    return !uiCheckboxChecked(t->no_rehash) || uiCheckboxChecked(t->tables_once);
}

/* Reload it after every query? Neither -A nor "Tables only, load once" does. */
static int load_after_queries(tab *t) {
    return !uiCheckboxChecked(t->no_rehash) && !uiCheckboxChecked(t->tables_once);
}

/* ---------- results area ---------- */

static void clear_edit_state(tab *t) {
    grid_edit_state *e = &t->edit;
    if (e->grid) sqlc_grid_detach(e->grid); /* before the grid goes away */
    table_info_free(&e->info);
    free(e->new_set);
    free(e->table);
    free(e->query);
    memset(e, 0, sizeof *e);
    e->new_row = -1;
}

static void clear_results(tab *t) {
    clear_edit_state(t);
    if (t->results) {
        uiBoxDelete(t->results_box, 0);
        uiControlDestroy(t->results); /* destroys the grids first ... */
        t->results = NULL;
    }
    for (size_t i = 0; i < t->nmodels; i++) free_set_model(t->models[i]); /* ... then models */
    free(t->models);
    t->models = NULL;
    t->nmodels = 0;
    db_outcome_free(&t->shown);
}

static void show_results_control(tab *t, uiControl *c) {
    t->results = c;
    uiBoxAppend(t->results_box, c, 1);
}

static uiControl *message_page(const char *text) {
    uiBox *b = uiNewVerticalBox();
    uiBoxSetPadded(b, 1);
    uiBoxAppend(b, uiControl(uiNewLabel(text)), 0);
    return uiControl(b);
}

static uiControl *editable_page(tab *t, set_model *sm, const db_outcome *meta,
                               const char *table, const char *query);

/* Takes ownership of out. meta: the columns of `table` (see
 * fetch_table_meta), to make its grid editable; NULL for other results.
 * query: the SQL that read a query's rows (Reload runs it again), NULL for
 * a browsed table. */
static void show_outcome(tab *t, db_outcome *out, const db_outcome *meta,
                         const char *table, const char *query) {
    clear_results(t);
    t->shown = *out;
    memset(out, 0, sizeof *out);

    size_t npages = t->shown.nsets + (t->shown.error ? 1 : 0);
    t->models = calloc(t->shown.nsets ? t->shown.nsets : 1, sizeof *t->models);

    uiControl **pages = calloc(npages ? npages : 1, sizeof *pages);
    char (*titles)[32] = calloc(npages ? npages : 1, sizeof *titles);
    if (!pages || !titles || !t->models) { free(pages); free(titles); return; }

    size_t p = 0;
    for (size_t i = 0; i < t->shown.nsets; i++, p++) {
        const db_set *s = &t->shown.sets[i];
        snprintf(titles[p], sizeof titles[p], "Result %zu", i + 1);
        if (s->has_rows) {
            set_model *sm = new_set_model(&t->shown.sets[i]);
            t->models[t->nmodels++] = sm;
            int editable = meta && table && t->shown.nsets == 1 && !t->shown.error;
            pages[p] = editable ? editable_page(t, sm, meta, table, query)
                                : uiControl(new_grid(sm));
        } else {
            char msg[128];
            snprintf(msg, sizeof msg, "Query OK, %llu row%s affected",
                     s->affected, s->affected == 1 ? "" : "s");
            pages[p] = message_page(msg);
        }
    }
    if (t->shown.error) {
        snprintf(titles[p], sizeof titles[p], "Error");
        pages[p++] = message_page(t->shown.error);
    }

    if (npages == 1) {
        show_results_control(t, pages[0]);
    } else if (npages > 1) {
        uiTab *tab = uiNewTab();
        for (size_t i = 0; i < npages; i++) uiTabAppend(tab, titles[i], pages[i]);
        show_results_control(t, uiControl(tab));
    }
    free(pages);
    free(titles);
}

/* ---------- sidebar ---------- */

static void on_table_double_click(uiTable *tbl, int row, void *data);
static void on_side_selection(uiTable *tbl, void *data);
static const char *selected_table(tab *t, int *is_view_out);
static size_t selected_count(tab *t);
static int selected_tables(tab *t, char ***names_out, size_t *n_out);
static void free_names(char **names, size_t n);
static unsigned on_table_menu_open(int row, void *data);
static void on_table_menu_action(table_menu_item item, int row, void *data);

static int is_view(const char *type) { return type && strstr(type, "VIEW") != NULL; }

/* Rebuild the visible table list from tables_raw, the "Hide views" box and
 * the search field (names containing its text, any case), selecting the
 * nkeep tables named in keep (those still in the list). */
static void rebuild_sidebar_keeping(tab *t, char *const *keep, size_t nkeep) {
    if (t->side_table) {
        uiBoxDelete(t->side_box, uiBoxNumChildren(t->side_box) - 1);
        uiControlDestroy(uiControl(t->side_table));
        t->side_table = NULL;
    }
    free_set_model(t->side_model);
    t->side_model = NULL;
    db_outcome_free(&t->side_data);
    free(t->side_index);
    t->side_index = NULL;

    int hide = uiCheckboxChecked(t->hide_views);
    char *query = uiEntryText(t->side_search);
    const db_set *raw = t->tables_raw.nsets ? &t->tables_raw.sets[0] : NULL;
    size_t n = raw ? raw->nrows : 0;

    db_set *s = calloc(1, sizeof *s);
    t->side_index = calloc(n ? n : 1, sizeof *t->side_index);
    if (!s || !t->side_index) { free(s); uiFreeText(query); return; }
    s->has_rows = 1;
    s->ncols = 1;
    s->names = calloc(1, sizeof *s->names);
    s->numeric = calloc(1, sizeof *s->numeric);
    s->cells = calloc(n ? n : 1, sizeof *s->cells);
    s->names[0] = strdup("Tables");

    for (size_t r = 0; r < n; r++) {
        const char *name = db_cell(raw, r, 0);
        const char *type = db_cell(raw, r, 1);
        if (!name || (hide && is_view(type)) || !sidebar_name_matches(name, query)) continue;
        char buf[512];
        snprintf(buf, sizeof buf, is_view(type) ? "%s  (view)" : "%s", name);
        s->cells[s->nrows] = strdup(buf);
        t->side_index[s->nrows] = r;
        s->nrows++;
    }
    uiFreeText(query);
    t->side_data.sets = s;
    t->side_data.nsets = 1;

    t->side_model = new_set_model(s);
    t->side_table = new_grid(t->side_model);
    /* Shift-click selects a range, Command-click adds or removes a row */
    uiTableSetSelectionMode(t->side_table, uiTableSelectionModeZeroOrMany);
    sqlc_table_fill_width(t->side_table); /* column follows the sidebar width */
    uiTableOnRowDoubleClicked(t->side_table, on_table_double_click, t);
    uiTableOnSelectionChanged(t->side_table, on_side_selection, t);
    sqlc_table_set_context_menu(t->side_table, on_table_menu_open, on_table_menu_action, t);
    uiBoxAppend(t->side_box, uiControl(t->side_table), 1);
    if (nkeep == 0 || s->nrows == 0 || s->nrows > (size_t)INT_MAX) return;

    /* the visible rows whose table is in keep */
    const char **names = calloc(s->nrows, sizeof *names);
    unsigned char *kept = calloc(s->nrows, 1);
    int *rows = calloc(s->nrows, sizeof *rows);
    if (names && kept && rows) {
        for (size_t v = 0; v < s->nrows; v++) names[v] = db_cell(raw, t->side_index[v], 0);
        if (export_select_names(s->nrows, names, nkeep, (const char *const *)keep, kept, NULL) >= 0) {
            int nrows = 0;
            for (size_t v = 0; v < s->nrows; v++)
                if (kept[v]) rows[nrows++] = (int)v;
            if (nrows) {
                uiTableSelection sel = { .NumRows = nrows, .Rows = rows };
                uiTableSetSelection(t->side_table, &sel);
            }
        }
    }
    free(rows);
    free(kept);
    free(names);
}

/* The same, keeping the current selection (tables_raw must be unchanged). */
static void rebuild_sidebar(tab *t) {
    char **keep = NULL;
    size_t nkeep = 0;
    if (selected_tables(t, &keep, &nkeep) != 0) nkeep = 0; /* out of memory: drop it */
    rebuild_sidebar_keeping(t, keep, nkeep);
    free_names(keep, nkeep);
}

/* ---------- welcome screen / connected layout ---------- */

/* The connected bar's Hide views follows the real checkbox (welcome form). */
static void sync_bar_hide_views(tab *t) {
    uiCheckboxSetChecked(t->hide_views_bar, uiCheckboxChecked(t->hide_views));
    if (uiControlEnabled(uiControl(t->hide_views))) uiControlEnable(uiControl(t->hide_views_bar));
    else uiControlDisable(uiControl(t->hide_views_bar));
}

static void update_titles(tab *t);

/* "Connected to user@host:port - database: db", kept up to date after each job.
 * Also the tab's title, which names the database. */
static void update_conn_label(tab *t) {
    update_titles(t);
    if (!t->conn && !t->lost) return;
    char port[16] = "";
    if (t->conn_port) snprintf(port, sizeof port, ":%u", t->conn_port);
    char buf[1024];
    snprintf(buf, sizeof buf, "%s %s%s%s%s - %s%s",
             t->conn ? "Connected to" : "Connection lost:",
             t->conn_user ? t->conn_user : "", t->conn_user && *t->conn_user ? "@" : "",
             t->conn_host ? t->conn_host : "", port,
             t->current_db ? "database: " : "no database selected",
             t->current_db ? t->current_db : "");
    uiLabelSetText(t->conn_label, buf);
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
 * shows under the form. Off again from set_busy(t, 0), whatever the outcome. */
static void set_connecting(tab *t, int on) {
    if (on == t->connecting) return;
    t->connecting = on;
    if (t->saved_table) sqlc_table_set_enabled(t->saved_table, !on);
    if (!on) {
        uiControlEnable(t->form_col);
        sqlc_loading_stop(t->loading);
        return;
    }
    uiControlDisable(t->form_col);
    start_loading_with_tip(t->loading);
}

/* While the table list loads (Load / Refresh tables, Hide views): the list
 * is dimmed and a spinner with a SQL tip shows above it in the sidebar.
 * Off again from set_busy(t, 0); the list is rebuilt enabled afterwards. */
static void set_refreshing(tab *t, int on) {
    if (on == t->refreshing) return;
    t->refreshing = on;
    if (t->side_table) sqlc_table_set_enabled(t->side_table, !on);
    if (!on) {
        sqlc_loading_stop(t->side_loading);
        uiControlHide(t->side_loading);
        return;
    }
    uiControlShow(t->side_loading);
    start_loading_with_tip(t->side_loading);
}

/* Welcome screen while not connected, the full layout once connected (and
 * after the connection was lost, until Reconnect or Disconnect). */
static void show_screen(tab *t) {
    if (t->conn || t->lost) {
        update_conn_label(t);
        sync_bar_hide_views(t);
        uiControlHide(t->welcome);
        uiControlShow(t->workspace);
        sqlc_window_layout_now(app.win, 1); /* no divider drawn where it was */
    } else {
        uiControlHide(t->workspace);
        uiControlShow(t->welcome);
        update_titles(t);
    }
}

/* ---------- worker thread ---------- */

typedef enum { JOB_CONNECT, JOB_QUERY, JOB_TABLES, JOB_EXPORT, JOB_EXPORT_DB, JOB_ALTER,
               JOB_AI, JOB_ROWS, JOB_STRUCTURE, JOB_PING } job_kind;

/* JOB_ROWS: a change to the rows of the browsed table's grid */
typedef enum { ROW_UPDATE, ROW_INSERT, ROW_DELETE } row_op;

/* JOB_ALTER: what the context menu asked for */
typedef enum { ALTER_RENAME, ALTER_TRUNCATE, ALTER_DROP } alter_kind;

typedef struct job {
    job_kind kind;
    struct tab *t;            /* the tab it runs for: UI thread only, the worker
                               * never touches it */
    MYSQL *conn;              /* existing connection, or the new one */
    char *host, *user, *pass, *dbname;
    unsigned int port;
    /* JOB_EXPORT*: the worker opens its own connection with the login above
     * (conn is NULL until then) and closes it before job_done, so the main
     * connection is never used by an export. */
    int own_conn;
    char *sql;
    db_exec_opts eo;
    char *profile;            /* saved connection used to connect, if any */
    int reconnect;            /* JOB_CONNECT: Reconnect after the connection was lost */
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
    atomic_int cancel;        /* JOB_EXPORT*: set on the UI thread to stop it */
    int cancelled;            /* ... and it did stop (EXPORT_CANCELLED) */
    char **names;             /* ... only these tables/views (malloc'd), if nnames */
    size_t nnames;
    /* JOB_AI */
    char *task, *api_key;
    ai_result ai;
    /* JOB_QUERY browsing a table, or whose rows come from one table: also
     * fetch that table's columns into meta (to edit them) */
    char *browse;
    db_outcome meta;
    /* ... browsing (see run_browse): the filter (filter_col NULL = none) and
     * the page's first row; the worker builds sql (the page plus one row, to
     * see if there's a next) and editor_sql (what the editor shows) */
    char *filter_col, *filter_value;
    browse_op filter_op;
    unsigned long long offset;
    int fresh;                /* from the sidebar: the filter bar starts over */
    int keep_editor;          /* reconnected: the editor keeps the user's text */
    char *editor_sql;
    const char **keys;        /* the primary key's columns, pointers into meta */
    size_t nkeys;
    int has_next;             /* more rows after this page */
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
    /* jobs on the main connection: the server closed it (see db_conn_lost);
     * no table list or current database was read then */
    int lost;
    char *lost_error;         /* ... the server's message for it */
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
    free(j->filter_col);
    free(j->filter_value);
    free(j->editor_sql);
    free(j->keys);            /* the names are in meta */
    db_outcome_free(&j->meta);
    free(j->value);
    free(j->typed);
    free(j->del_rows);
    db_outcome_free(&j->out);
    db_outcome_free(&j->tables);
    free(j->lost_error);
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
static void ask_claude_now(tab *t);

/* JOB_AI on the worker thread: schema, then the Claude API call. */
static void ask_claude(job *j) {
    db_exec_opts eo = { 0 };
    db_outcome cur;
    char *dbname = NULL;
    if (db_execute(j->conn, "SELECT DATABASE()", &eo, &cur) == 0 && cur.nsets &&
        cur.sets[0].nrows && db_cell(&cur.sets[0], 0, 0))
        dbname = strdup(db_cell(&cur.sets[0], 0, 0));
    db_outcome_free(&cur);
    if (db_conn_lost(j->conn)) { /* no schema to send: don't ask (worker() says why) */
        j->ai.error = strdup("The connection to the server was lost.");
        free(dbname);
        return;
    }

    char *schema = dbname ? ai_schema(j->conn, AI_SCHEMA_BYTES) : NULL;
    char *body = ai_build_request(AI_MODEL, mysql_get_server_info(j->conn), dbname,
                                  schema, j->task);
    if (!body) j->ai.error = strdup("Out of memory");
    else j->ok = ai_generate(j->api_key, body, &j->ai) == 0;
    free(body);
    free(schema);
    free(dbname);
}

/* malloc'd 'value' as a quoted SQL string literal, or NULL. Worker thread. */
static char *sql_string(MYSQL *conn, const char *value) {
    size_t n = strlen(value);
    char *q = malloc(2 * n + 3);
    if (!q) return NULL;
    q[0] = '\'';
    unsigned long m = mysql_real_escape_string(conn, q + 1, value, (unsigned long)n);
    if (m == (unsigned long)-1) { free(q); return NULL; }
    q[m + 1] = '\'';
    q[m + 2] = '\0';
    return q;
}

/* Columns and type of table `table` into j->meta: in database db, or the
 * current one (db NULL). Runs on the worker thread. */
static void fetch_table_meta(job *j, const char *table, const char *db) {
    char *name = sql_string(j->conn, table);
    char *schema = db ? sql_string(j->conn, db) : strdup("DATABASE()");
    char *sql = NULL;
    if (name && schema &&
        asprintf(&sql,
                 "SELECT COLUMN_NAME, DATA_TYPE, IS_NULLABLE, COLUMN_KEY, EXTRA "
                 "FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = %s "
                 "AND TABLE_NAME = %s ORDER BY ORDINAL_POSITION;"
                 "SELECT TABLE_TYPE FROM information_schema.TABLES "
                 "WHERE TABLE_SCHEMA = %s AND TABLE_NAME = %s",
                 schema, name, schema, name) < 0)
        sql = NULL;
    if (sql) {
        db_exec_opts eo = { 0 };
        db_execute(j->conn, sql, &eo, &j->meta);
    }
    free(sql);
    free(schema);
    free(name);
}

/* The rows of a query that can be edited: one result set, every column a
 * real column of one table (see db_set's org_table). */
static const db_set *editable_query_set(const job *j) {
    if (j->kind != JOB_QUERY || j->browse || j->out.error || j->out.nsets != 1) return NULL;
    const db_set *s = &j->out.sets[0];
    return s->has_rows && s->org_table && s->org_db ? s : NULL;
}

static unsigned long escape_value(char *to, const char *from, unsigned long len, void *data);
static row_edit_escaper escaper(tab *t);

/* A browse (JOB_QUERY with j->browse) on the worker thread. The table's
 * columns come first: the page is ordered by its primary key, so every
 * page's SQL is known only then. The value is escaped on this connection
 * (it knows the server's charset and sql_mode); the editor gets the same
 * SELECT with the page's LIMIT 1000, the server is asked for 1001 rows and
 * the set keeps 1000 (truncated = there is a next page). */
static void run_browse(job *j) {
    fetch_table_meta(j, j->browse, NULL);
    if (j->meta.error) { /* e.g. the connection is gone */
        j->out.error = strdup(j->meta.error);
        return;
    }
    /* No columns (the table is gone, or out of memory): no key, and the
     * SELECT below brings the server's own error. */
    const db_set *cols = j->meta.nsets ? &j->meta.sets[0] : NULL;
    if (cols && browse_key_columns(cols, &j->keys, &j->nkeys) != 0) {
        j->out.error = strdup("Out of memory");
        return;
    }
    browse_filter f = { j->filter_col, j->filter_op, j->filter_value };
    row_edit_escaper esc = { escape_value, j->conn };
    char err[256] = "Out of memory";
    const char *const *order = (const char *const *)j->keys;
    j->sql = browse_select_sql(j->browse, &f, order, j->nkeys, j->offset,
                               BROWSE_PAGE_ROWS + 1, &esc, err, sizeof err);
    if (j->sql)
        j->editor_sql = browse_select_sql(j->browse, &f, order, j->nkeys, j->offset,
                                          BROWSE_PAGE_ROWS, &esc, err, sizeof err);
    if (!j->sql || !j->editor_sql) {
        j->out.error = strdup(err);
        return;
    }
    j->eo.max_rows = BROWSE_PAGE_ROWS;
    j->ok = db_execute(j->conn, j->sql, &j->eo, &j->out) == 0;
    j->has_next = j->ok && j->out.nsets == 1 && j->out.sets[0].has_rows &&
                  j->out.sets[0].truncated;
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

/* Export progress: called on the worker thread, shown on the UI thread
 * (the status sentence and the progress bar). */
typedef struct {
    struct tab *t;            /* the export's tab (only passed along, never read here) */
    char *table;              /* copy of p.table */
    int dump;                 /* JOB_EXPORT_DB */
    const char *what;         /* ... "database" or "tables" (static strings) */
    export_progress p;
} export_progress_msg;

static void show_export_progress(void *data) {
    export_progress_msg *m = data;
    tab *t = m->t;
    /* t is alive: these messages are queued from the export's worker thread
     * before its job_done, uiQueueMain runs them in order, and a tab is only
     * freed once its last job_done has run (see tab_release). A closed tab
     * has no widgets left. Not once the export is over, and not over
     * "Cancelling export...". */
    if (!t->closed && !app.gone && t->export_job && !atomic_load(&t->export_job->cancel)) {
        if (m->dump)
            set_status(t, "Exporting %s... %s (%zu of %zu), %llu rows",
                       m->what, m->table, m->p.index, m->p.count, m->p.rows);
        else if (m->p.rows > 0) /* keep "Exporting t..." until rows come */
            set_status(t, "Exporting... %llu rows", m->p.rows);
        uiProgressBarSetValue(t->export_bar, export_progress_percent(&m->p));
    }
    free(m->table);
    free(m);
}

/* data is the job; returns nonzero (stop) once Cancel export was confirmed. */
static int export_progress_cb(const export_progress *p, void *data) {
    job *j = data;
    export_progress_msg *m = malloc(sizeof *m);
    if (m && !(m->table = strdup(p->table ? p->table : ""))) {
        free(m);
        m = NULL;
    }
    if (m) {
        m->t = j->t; /* set before the thread started, never changed */
        m->dump = j->kind == JOB_EXPORT_DB;
        m->what = j->nnames ? "tables" : "database";
        m->p = *p;
        m->p.table = m->table;
        uiQueueMain(show_export_progress, m);
    }
    return atomic_load(&j->cancel);
}

/* Run one statement on j->conn; on failure, j->error (if not set yet) gets
 * the server's message. Returns 0 or -1. Worker thread. */
static int export_sql(job *j, const char *sql) {
    db_exec_opts eo = { 0 };
    db_outcome out;
    int rc = db_execute(j->conn, sql, &eo, &out);
    if (rc != 0 && !j->error) {
        j->error = out.error ? out.error : strdup("Unknown error");
        out.error = NULL;
    }
    db_outcome_free(&out);
    return rc;
}

/* An export's own connection, inside a consistent-snapshot transaction
 * (InnoDB: every table is read as of the same moment). Returns 0, or -1
 * with j->error set and no connection left open. Worker thread. */
static int export_open(job *j) {
    char *err = NULL;
    j->conn = db_connect(j->host, j->user, j->pass, j->dbname, j->port, &err);
    if (!j->conn) {
        if (asprintf(&j->error, "Could not open a connection for the export: %s",
                     err ? err : "out of memory") < 0)
            j->error = NULL;
        free(err);
        return -1;
    }
    if (export_sql(j, "SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ") != 0 ||
        export_sql(j, "START TRANSACTION WITH CONSISTENT SNAPSHOT") != 0) {
        mysql_close(j->conn);
        j->conn = NULL;
        return -1;
    }
    return 0;
}

/* Ends the transaction (it only read: COMMIT if the export worked, else
 * ROLLBACK) and closes the export's connection. Worker thread. */
static void export_close(job *j) {
    db_exec_opts eo = { 0 };
    db_outcome out;
    db_execute(j->conn, j->ok ? "COMMIT" : "ROLLBACK", &eo, &out);
    db_outcome_free(&out);
    mysql_close(j->conn);
    j->conn = NULL; /* so no load_tables, and job_done leaves current_db alone */
}

static void *worker(void *data) {
    job *j = data;
    double t0 = now_seconds();
    int exporting = j->kind == JOB_EXPORT || j->kind == JOB_EXPORT_DB;

    if (exporting && j->own_conn && export_open(j) != 0) {
        j->ok = 0; /* the export didn't start; nothing was written */
    } else if (j->kind == JOB_CONNECT) {
        j->conn = db_connect(j->host, j->user, j->pass, j->dbname, j->port, &j->error);
        j->ok = j->conn != NULL;
    } else if (j->kind == JOB_QUERY && j->browse) {
        run_browse(j);
    } else if (j->kind == JOB_QUERY || j->kind == JOB_ALTER) {
        j->ok = db_execute(j->conn, j->sql, &j->eo, &j->out) == 0;
        if (j->ok) {
            const db_set *s = editable_query_set(j);
            if (s) fetch_table_meta(j, s->org_table, s->org_db);
        }
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
    } else if (j->kind == JOB_PING) {
        /* MYSQL_OPT_RECONNECT is off: a dead connection fails, it isn't
         * silently replaced (which would lose the session's state) */
        j->ok = mysql_ping(j->conn) == 0;
    } else if (j->kind == JOB_EXPORT) {
        int rc = export_table(j->conn, j->table, j->is_view, j->fmt, &j->xopts, j->path,
                              export_progress_cb, j, &j->rows, &j->error);
        j->ok = rc == 0;
        j->cancelled = rc == EXPORT_CANCELLED;
    } else if (j->kind == JOB_EXPORT_DB) {
        int rc;
        if (j->nnames)
            rc = export_tables(j->conn, (const char *const *)j->names, j->nnames, &j->xopts,
                               j->path, export_progress_cb, j, &j->db_stats, &j->error);
        else
            rc = export_database(j->conn, &j->xopts, j->path, export_progress_cb, j,
                                 &j->db_stats, &j->error);
        j->ok = rc == 0;
        j->cancelled = rc == EXPORT_CANCELLED;
    } else {
        j->ok = 1; /* JOB_TABLES: just the refresh below */
    }
    if (exporting && j->own_conn && j->conn) export_close(j);
    j->seconds = now_seconds() - t0;

    /* A job on the main connection may find it closed by the server (most
     * often wait_timeout after sitting idle). Then nothing more is read on
     * it, and job_done offers Reconnect. */
    int main_conn = !exporting && j->kind != JOB_CONNECT && j->conn;
    if (main_conn) j->lost = j->kind == JOB_PING ? !j->ok : db_conn_lost(j->conn);
    if (j->conn && !j->lost && j->kind != JOB_PING) {
        load_tables(j);
        if (main_conn) j->lost = db_conn_lost(j->conn);
    }
    if (j->lost) {
        j->lost_error = db_error_string(j->conn);
        j->load_tables = 0; /* the list read (if any) failed: keep the old one */
    }
    uiQueueMain(job_done, j);
    return NULL;
}

/* The progress bar and Cancel next to the status line, for export j. */
static void show_export_bar(tab *t, job *j) {
    t->export_job = j;
    uiProgressBarSetValue(t->export_bar, 0);
    uiControlEnable(uiControl(t->cancel_export_btn));
    uiControlShow(uiControl(t->export_bar));
    uiControlShow(uiControl(t->cancel_export_btn));
}

/* ... hidden and reset when it ends, whatever the outcome. */
static void hide_export_bar(tab *t) {
    t->export_job = NULL;
    uiProgressBarSetValue(t->export_bar, 0); /* also ends an indeterminate bar */
    uiControlHide(uiControl(t->export_bar));
    uiControlHide(uiControl(t->cancel_export_btn));
}

/* Exports run beside the main connection's jobs: they show the export bar
 * instead of making the app busy. */
/* Runs j for tab t on a worker thread; job_done gets it back. While it runs
 * it counts in t->jobs (and app.exports), so a closed tab outlives it. */
static void start_job(tab *t, job *j) {
    pthread_t th;
    int exporting = j->kind == JOB_EXPORT || j->kind == JOB_EXPORT_DB;
    j->t = t;
    if (j->kind == JOB_PING) {
        t->job_kind = j->kind;
        t->busy = 1; /* quietly (it takes milliseconds): no buttons dimmed */
    } else if (!exporting) {
        t->job_kind = j->kind;
        set_busy(t, 1);
        if (j->kind == JOB_CONNECT) set_connecting(t, 1);
        else if (j->kind == JOB_TABLES) set_refreshing(t, 1);
    }
    atomic_init(&j->cancel, 0);
    if (exporting) show_export_bar(t, j);
    t->jobs++;
    if (exporting) app.exports++;
    if (pthread_create(&th, NULL, worker, j) != 0) {
        t->jobs--;
        if (exporting) app.exports--;
        if (exporting) hide_export_bar(t);
        else set_busy(t, 0);
        set_status(t, "Could not start worker thread");
        job_free(j);
        return;
    }
    pthread_detach(th);
}

static void reload_browse(tab *t);
static void reload_rows(tab *t);
static int browse_table(tab *t, const char *name);
enum { BROWSE_FRESH = 1, BROWSE_KEEP_EDITOR = 2 }; /* start_browse flags */
static int start_browse(tab *t, const char *name, const char *col, browse_op op,
                        const char *value, unsigned long long offset, int flags);
static void browse_page(tab *t, unsigned long long offset);
static void rows_done(tab *t, job *j);

static void show_message_page(tab *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void show_message_page(tab *t, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    clear_results(t);
    show_results_control(t, message_page(buf));
}

/* ---------- Structure page ---------- */

static void structure_clear(tab *t) {
    structure_page *st = &t->st;
    /* the boxes' children (grids or labels) first, then the models */
    if (st->cols) { uiBoxDelete(st->cols_box, 0); uiControlDestroy(st->cols); st->cols = NULL; }
    if (st->idx) { uiBoxDelete(st->idx_box, 0); uiControlDestroy(st->idx); st->idx = NULL; }
    free_set_model(st->cols_model);
    free_set_model(st->idx_model);
    st->cols_model = st->idx_model = NULL;
    db_outcome_free(&st->data);
}

/* Empty page with a message instead of the structure; forgets the table. */
static void structure_message(tab *t, const char *text) {
    structure_clear(t);
    free(t->st.table);
    t->st.table = NULL;
    uiLabelSetText(t->st.title, text);
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
static void structure_done(tab *t, job *j) {
    structure_page *st = &t->st;
    structure_clear(t);
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
        set_status(t, "Could not load the structure of %s: %s", st->table, st->data.error);
    } else if (!cols || !cols->nrows) {
        snprintf(title, sizeof title, "%s: no such table in this database", st->table);
        set_status(t, "%s", title);
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
        set_status(t, "Structure of %s: %zu column%s, %zu index%s", st->table, cols->nrows,
                   cols->nrows == 1 ? "" : "s", ni, ni == 1 ? "" : "es");
    }
    uiLabelSetText(st->title, title);
    st->cols = structure_grid(st->cols_box, cols, &st->cols_model, "No columns.");
    st->idx = structure_grid(st->idx_box, idx, &st->idx_model,
                             st->data.error ? "" : "No indexes.");
}

static void start_structure(tab *t, const char *table) {
    job *j = calloc(1, sizeof *j);
    if (!j) return;
    j->kind = JOB_STRUCTURE;
    j->conn = t->conn;
    j->table = strdup(table);
    set_status(t, "Loading the structure of %s...", table);
    start_job(t, j);
}

/* On the Structure page, show the table selected in the sidebar (loading
 * it if it isn't the one shown). Called whenever that might have changed. */
static void structure_sync(tab *t) {
    if (t->mode != MODE_STRUCTURE) return;
    if (!t->conn) {
        structure_message(t, t->lost ? "The connection to the server was lost - press Reconnect."
                                     : "Not connected.");
        return;
    }
    size_t nsel = selected_count(t);
    if (nsel > 1) {
        char msg[128];
        snprintf(msg, sizeof msg, "%zu tables selected - select one to see its structure.", nsel);
        structure_message(t, msg);
        return;
    }
    const char *name = selected_table(t, NULL);
    if (!name) {
        if (!t->st.table)
            uiLabelSetText(t->st.title, t->tables_loaded
                           ? "Select a table in the sidebar to see its structure."
                           : "Load the table list and select a table to see its structure.");
        return;
    }
    if (t->busy || (t->st.table && strcmp(t->st.table, name) == 0)) return;
    start_structure(t, name);
}

/* Shows one mode of the main area and keeps the switch in step (setting it
 * from code doesn't call on_mode_changed). The results stay as they are
 * across Normal / AI; Structure loads the selected table if needed. */
static void show_mode(tab *t, int mode) {
    if (mode != MODE_AI && mode != MODE_STRUCTURE) mode = MODE_NORMAL;
    t->mode = mode;
    if (mode == MODE_STRUCTURE) {
        uiControlHide(t->query_split);
        uiControlShow(t->structure_view);
    } else {
        if (mode == MODE_AI) {
            uiControlHide(t->normal_view);
            uiControlShow(t->ai_view);
        } else {
            uiControlHide(t->ai_view);
            uiControlShow(t->normal_view);
        }
        uiControlHide(t->structure_view);
        uiControlShow(t->query_split);
    }
    if (sqlc_segmented_selected(t->mode_switch) != mode)
        sqlc_segmented_set_selected(t->mode_switch, mode);
    structure_sync(t);
}

static void on_mode_changed(uiControl *seg, int index, void *data) {
    (void)seg; tab *t = data;
    show_mode(t, index);
}

/* Queued by on_side_selection, which counted it in t->jobs: t is alive. */
static void structure_sync_later(void *data) {
    tab *t = data;
    t->sync_queued = 0;
    if (t->closed || app.gone) { tab_release(t); return; } /* may free t */
    tab_release(t);
    structure_sync(t);
}

/* Not right away: a right-click selects the row and then builds its menu,
 * which must not see the app busy loading the structure. */
static void on_side_selection(uiTable *tbl, void *data) {
    (void)tbl; tab *t = data;
    if (!t->sync_queued) {
        t->sync_queued = 1;
        t->jobs++; /* t stays alive until it runs, even if closed */
        uiQueueMain(structure_sync_later, t);
    }
}

static void on_structure_refresh(uiButton *b, void *data) {
    (void)b; tab *t = data;
    if (t->busy || !t->conn) return;
    const char *name = selected_table(t, NULL);
    if (!name) name = t->st.table;
    if (name) start_structure(t, name);
    else structure_sync(t);
}

/* A rename/truncate/drop from the context menu finished. */
static void alter_done(tab *t, job *j) {
    static const char *const verbs[] = { "Rename", "Truncate", "Drop" };
    if (!j->ok) {
        const char *err = j->out.error ? j->out.error : "unknown error";
        char title[64];
        snprintf(title, sizeof title, "%s failed", verbs[j->alter]);
        set_status(t, "%s: %s", title, err);
        show_error(title, err);
        return;
    }
    const char *what = j->is_view ? "view" : "table";
    int shown = t->browsing && strcmp(t->browsing, j->table) == 0;
    int structure = t->st.table && strcmp(t->st.table, j->table) == 0;
    if (structure) {
        if (j->alter == ALTER_DROP) {
            char msg[600];
            snprintf(msg, sizeof msg, "The %s %s was dropped.", what, j->table);
            structure_message(t, msg);
        } else {
            /* forget it: reloaded below if it's still selected (by its new name) */
            free(t->st.table);
            t->st.table = NULL;
        }
    }
    switch (j->alter) {
    case ALTER_RENAME:
        set_status(t, "Renamed %s %s to %s", what, j->table, j->new_name);
        if (t->edit.table && strcmp(t->edit.table, j->table) == 0) { /* edits go there now */
            char *renamed = strdup(j->new_name);
            if (renamed) { free(t->edit.table); t->edit.table = renamed; }
        }
        if (shown) { /* same rows, new name: just follow it */
            char *renamed = strdup(j->new_name);
            if (renamed) { free(t->browsing); t->browsing = renamed; }
            /* the editor's SELECT with the new name (the value is escaped
             * on the connection: none if it was lost, then it stays) */
            browse_filter f = { t->applied_col, t->applied_op, t->applied_value };
            row_edit_escaper esc = escaper(t);
            char err[256];
            char *sql = t->conn
                ? browse_select_sql(t->browsing, &f, (const char *const *)t->page_keys,
                                    t->npage_keys, t->page_offset, BROWSE_PAGE_ROWS,
                                    &esc, err, sizeof err)
                : NULL;
            if (sql) {
                uiMultilineEntrySetText(t->editor, sql);
                t->ai_in_editor = 0;
            }
            free(sql);
        }
        break;
    case ALTER_TRUNCATE:
        set_status(t, "Truncated table %s", j->table);
        if (shown) browse_page(t, 0); /* an empty grid, still editable */
        break;
    case ALTER_DROP:
        set_status(t, "Dropped %s %s", what, j->table);
        if (shown) {
            show_message_page(t, "The %s %s was dropped.", what, j->table);
            browse_forget(t);
        }
        break;
    }
}

/* Point the filter bar's column dropdown at name (its default if name is
 * NULL or not a column). */
static void select_filter_column(tab *t, const char *name) {
    size_t sel = t->filter_default;
    for (size_t i = 0; name && i < t->nfilter_cols; i++)
        if (strcmp(t->filter_cols[i], name) == 0) { sel = i; break; }
    if (sel < t->nfilter_cols) uiComboboxSetSelected(t->filter_col, (int)sel);
}

/* The filter bar's column dropdown from the browsed table's columns (meta's
 * first set), refilled only when they changed (another table, or ALTER). */
static void fill_filter_columns(tab *t, const db_set *cols) {
    size_t n = cols->nrows;
    int same = n == t->nfilter_cols;
    for (size_t i = 0; same && i < n; i++) {
        const char *name = db_cell(cols, i, 0);
        same = name && strcmp(name, t->filter_cols[i]) == 0;
    }
    t->filter_default = browse_default_column(cols);
    if (same) return;
    free_strings(t->filter_cols, t->nfilter_cols);
    t->filter_cols = NULL;
    t->nfilter_cols = 0;
    uiComboboxClear(t->filter_col);
    char **names = calloc(n ? n : 1, sizeof *names);
    if (!names) return;
    for (size_t i = 0; i < n; i++) {
        const char *name = db_cell(cols, i, 0);
        names[i] = strdup(name ? name : "");
        if (!names[i]) { free_strings(names, i); uiComboboxClear(t->filter_col); return; }
        uiComboboxAppend(t->filter_col, names[i]);
    }
    t->filter_cols = names;
    t->nfilter_cols = n;
    select_filter_column(t, t->applied_col);
}

/* A browse of t->browsing came back (before its rows are shown): the
 * editor's SELECT, the filter bar's columns, and what the page bar needs. */
static void browse_done(tab *t, job *j) {
    if (j->editor_sql && !j->keep_editor) {
        uiMultilineEntrySetText(t->editor, j->editor_sql);
        t->ai_in_editor = 0;
    }
    const db_set *cols = j->meta.nsets && !j->meta.error ? &j->meta.sets[0] : NULL;
    if (cols && cols->ncols >= 4) fill_filter_columns(t, cols);
    if (j->fresh) { /* from the sidebar: the bar starts over too */
        select_filter_column(t, NULL);
        uiComboboxSetSelected(t->filter_op, BROWSE_EQ);
        uiEntrySetText(t->filter_value, "");
    }
    t->page_has_next = j->has_next;
    t->page_keyless = j->ok && j->nkeys == 0;
    if (j->ok) { /* the ORDER BY columns, to write the SELECT again after a rename */
        free_strings(t->page_keys, t->npage_keys);
        t->page_keys = NULL;
        t->npage_keys = 0;
        char **keys = j->nkeys ? calloc(j->nkeys, sizeof *keys) : NULL;
        size_t n = 0;
        while (keys && n < j->nkeys && (keys[n] = strdup(j->keys[n]))) n++;
        if (keys && n < j->nkeys) { free_strings(keys, n); keys = NULL; n = 0; }
        t->page_keys = keys;
        t->npage_keys = n;
    }
}

/* Back on the UI thread. */
/* The alert at the end of an export: sound, Dock bounce if in the
 * background, modal alert. Held back while the "Cancel the export?" alert
 * is open (job_done can run during it) and shown when that closes. */
static void notify_export(tab *t, const char *title, const char *message, int failed) {
#ifdef SQLC_AUTOTEST
    printf("[notify] %s: %s\n", title, message);
    (void)failed; (void)t;
#else
    if (t->confirming_cancel) {
        free(t->pending_title);
        free(t->pending_msg);
        t->pending_title = strdup(title);
        t->pending_msg = strdup(message);
        t->pending_failed = failed;
        return;
    }
    sqlc_notify_done(title, message, failed);
#endif
}

/* "Export finished in 2m 14s - 12 tables, 1,204,331 rows, saved to ~/dump.sql." */
static void notify_export_done(tab *t, const job *j) {
    char took[32], rows[40], where[1024], msg[2048];
    export_format_duration(took, sizeof took, j->seconds);
    export_tilde_path(where, sizeof where, j->path, getenv("HOME"));
    if (j->kind == JOB_EXPORT) {
        export_format_count(rows, sizeof rows, j->rows);
        snprintf(msg, sizeof msg, "Export finished in %s - %s row%s of %s, saved to %s.",
                 took, rows, j->rows == 1 ? "" : "s", j->table, where);
    } else {
        const export_db_stats *st = &j->db_stats;
        char views[64] = "";
        if (st->views) snprintf(views, sizeof views, ", %zu view%s", st->views, st->views == 1 ? "" : "s");
        export_format_count(rows, sizeof rows, st->rows);
        snprintf(msg, sizeof msg, "Export finished in %s - %zu table%s%s, %s row%s, saved to %s.",
                 took, st->tables, st->tables == 1 ? "" : "s", views,
                 rows, st->rows == 1 ? "" : "s", where);
    }
    notify_export(t, "Export finished", msg, 0);
}

static void notify_export_failed(tab *t, const char *title, const job *j) {
    char took[32], msg[2048];
    export_format_duration(took, sizeof took, j->seconds);
    snprintf(msg, sizeof msg, "%s\n\nFailed after %s.", j->error ? j->error : "Unknown error", took);
    notify_export(t, title, msg, 1);
}

/* Cancel next to the progress bar: ask first, Keep exporting is the default.
 * The export goes on while the alert is open; if it ends meanwhile,
 * confirming does nothing. */
static void on_cancel_export(uiButton *b, void *data) {
    (void)b; tab *t = data;
    if (!t->export_job || t->confirming_cancel) return;
    t->confirming_cancel = 1;
    int yes = sqlc_confirm_keep("Cancel the export?", "The partial file will be deleted.",
                                "Cancel export", "Keep exporting");
    t->confirming_cancel = 0;
    if (yes && t->export_job) {
        atomic_store(&t->export_job->cancel, 1);
        uiControlDisable(uiControl(t->cancel_export_btn));
        set_status(t, "Cancelling export...");
    }
    if (t->pending_msg) { /* it ended while the alert was open */
        char *title = t->pending_title, *msg = t->pending_msg;
        t->pending_title = t->pending_msg = NULL;
        sqlc_notify_done(title ? title : "Export", msg, t->pending_failed);
        free(title);
        free(msg);
    }
}

/* JOB_EXPORT* finished (from job_done). The export ran on its own
 * connection, already closed: the main connection's state (busy, sidebar,
 * current database, structure) is not touched here. The finish alert is
 * left out when quitting; the app quits once the export is over. */
static void export_done(tab *t, job *j) {
    if (t->export_job == j) hide_export_bar(t);
    int notify = !app.quitting;
    if (j->cancelled) {
        set_status(t, "Export cancelled.");
    } else if (j->kind == JOB_EXPORT && j->ok) {
        set_status(t, "Exported %llu row%s of %s to %s (%.2f s)", j->rows, j->rows == 1 ? "" : "s",
                   j->table, j->path, j->seconds);
        if (notify) notify_export_done(t, j);
    } else if (j->kind == JOB_EXPORT) {
        set_status(t, "Export failed: %s", j->error ? j->error : "unknown error");
        if (notify) notify_export_failed(t, "Export failed", j);
    } else if (j->ok) {
        const export_db_stats *st = &j->db_stats;
        if (j->nnames)
            set_status(t, "Exported %zu selected table%s from %s: %zu table%s, %zu view%s, %llu row%s "
                       "to %s (%.2f s)%s",
                       j->nnames, j->nnames == 1 ? "" : "s",
                       j->table, st->tables, st->tables == 1 ? "" : "s",
                       st->views, st->views == 1 ? "" : "s",
                       st->rows, st->rows == 1 ? "" : "s", j->path, j->seconds,
                       st->skipped ? " - some objects skipped, see the file" : "");
        else
            set_status(t, "Exported database %s: %zu table%s, %zu view%s, %llu row%s to %s (%.2f s)%s",
                       j->table, st->tables, st->tables == 1 ? "" : "s",
                       st->views, st->views == 1 ? "" : "s",
                       st->rows, st->rows == 1 ? "" : "s", j->path, j->seconds,
                       st->skipped ? " - some objects skipped, see the file" : "");
        if (notify) notify_export_done(t, j);
    } else {
        const char *title = j->nnames ? "Export failed" : "Database export failed";
        set_status(t, "%s: %s", title, j->error ? j->error : "unknown error");
        if (notify) notify_export_failed(t, title, j);
    }
    job_free(j);
    if (quit_if_done()) return;
#ifdef SQLC_AUTOTEST
    autotest_next();
#endif
}

/* The server closed t's connection (err: its message, may be NULL). Says so
 * at once and shows Reconnect; everything on screen stays as it is. */
static void connection_lost(tab *t, const char *err) {
    if (t->conn) mysql_close(t->conn);
    t->conn = NULL;
    t->lost = 1;
    update_conn_label(t);
    uiControlShow(uiControl(t->reconnect_btn));
    set_status(t, "Lost the connection to the server%s%s - press Reconnect.",
               err ? ": " : "", err ? err : "");
    update_browse_bars(t); /* nothing can be browsed until Reconnect */
}

/* Why nothing runs while not connected. */
static const char *not_connected_msg(const tab *t) {
    return t->lost ? "The connection to the server was lost - press Reconnect."
                   : "Not connected - connect from the welcome screen first.";
}

static void job_done(void *data) {
    job *j = data;
    tab *t = j->t;
    int exporting = j->kind == JOB_EXPORT || j->kind == JOB_EXPORT_DB;
    if (exporting) app.exports--;
    if (t->closed || app.gone) { /* nothing left to show it in */
        if (j->kind == JOB_CONNECT && j->conn) mysql_close(j->conn);
        job_free(j);
        tab_release(t); /* may free t */
        quit_if_done();
        return;
    }
    tab_release(t); /* an open tab stays */
    if (exporting) { export_done(t, j); return; }
    if (j->kind == JOB_PING) { /* quiet unless the connection is gone */
        t->busy = 0;
        if (j->lost) connection_lost(t, j->lost_error);
        job_free(j);
        return;
    }
    set_busy(t, 0);
    /* Before the handlers below: they may start a follow-up job (a reload),
     * which then finds no connection instead of the dead one. */
    if (j->lost) connection_lost(t, j->lost_error);

    /* The sidebar's selection, to keep when its list is replaced below
     * (following a rename). Not across connections, but across a reconnect. */
    char **keep = NULL;
    size_t nkeep = 0;
    if (j->load_tables && (j->kind != JOB_CONNECT || j->reconnect)) {
        if (selected_tables(t, &keep, &nkeep) != 0) nkeep = 0;
        if (j->kind == JOB_ALTER && j->ok && j->alter == ALTER_RENAME) {
            for (size_t i = 0; i < nkeep; i++) {
                if (strcmp(keep[i], j->table) != 0) continue;
                char *renamed = strdup(j->new_name);
                if (renamed) { free(keep[i]); keep[i] = renamed; }
            }
        }
    }

    int rebrowse = 0; /* reconnected: show the browsed table again */
    if (j->kind == JOB_CONNECT) {
        if (!j->ok) {
            if (j->reconnect) { /* still lost: Reconnect stays */
                set_status(t, "Could not reconnect: %s - press Reconnect to try again.",
                           j->error ? j->error : "connection failed");
                show_error("Could not reconnect", j->error ? j->error : "");
            } else {
                set_status(t, "%s", j->error ? j->error : "Connection failed");
                uiMsgBoxError(app.win, "Connection failed", j->error ? j->error : "");
            }
            free_names(keep, nkeep);
            job_free(j);
            return;
        }
        if (t->conn) mysql_close(t->conn);
        t->conn = j->conn;
        /* The connection's login, for the connected bar and an export's own
         * connection; it replaces the old one. */
        forget_login(t);
        t->conn_user = j->user;
        t->conn_host = j->host;
        t->conn_pass = j->pass;
        t->conn_db = j->dbname;
        j->user = j->host = j->pass = j->dbname = NULL;
        t->conn_port = j->port;
        remember_last_profile(j->profile);
        free(t->profile); /* the tab's title names it */
        t->profile = j->profile;
        j->profile = NULL;
        if (j->reconnect) {
            /* The same server: the editor, results, structure and table
             * list stay (the list is reloaded unless -A); the browsed table
             * is read again below. */
            t->lost = 0;
            uiControlHide(uiControl(t->reconnect_btn));
            set_status(t, "Reconnected to %s%s%s", mysql_get_host_info(t->conn),
                       j->current_db ? " - database: " : "",
                       j->current_db ? j->current_db : "");
            rebrowse = t->browsing != NULL;
        } else {
            set_status(t, "Connected to %s (server %s)%s%s%s",
                       mysql_get_host_info(t->conn), mysql_get_server_info(t->conn),
                       j->current_db ? " - database: " : "",
                       j->current_db ? j->current_db : "",
                       !j->load_tables ? " - table list not loaded (-A)"
                       : uiCheckboxChecked(t->tables_once) ? " - tables only, not refreshed after queries"
                       : "");
            /* The old connection's list is stale whether or not we reload it. */
            db_outcome_free(&t->tables_raw);
            t->tables_loaded = 0;
            browse_forget(t);
            structure_message(t, "");
        }
        show_screen(t); /* off the welcome screen */
    } else if (j->kind == JOB_STRUCTURE) {
        structure_done(t, j);
    } else if (j->kind == JOB_ALTER) {
        alter_done(t, j);
    } else if (j->kind == JOB_ROWS) {
        rows_done(t, j);
    } else if (j->kind == JOB_AI) {
        if (!j->ok) {
            const char *err = j->ai.error ? j->ai.error : "unknown error";
            set_status(t, "Ask Claude failed: %s", err);
            show_error("Ask Claude failed", err);
        } else {
            char *text = ai_format(j->task, &j->ai);
            if (text) {
                uiMultilineEntrySetText(t->editor, text);
                t->ai_in_editor = 1;
                browse_forget(t);
                show_mode(t, MODE_NORMAL); /* the SQL is in the editor: show it */
            }
            free(text);
            if (j->ai.sql && *j->ai.sql)
                set_status(t, "%s wrote a query in %.1f s - review it, then press Run.",
                           j->ai.model ? j->ai.model : "Claude", j->seconds);
            else
                set_status(t, "Claude didn't write a query - see the comments in the editor.");
        }
    } else if (j->kind == JOB_TABLES) {
        const db_set *tl = j->tables.nsets ? &j->tables.sets[0] : NULL;
        if (j->tables.error)
            set_status(t, "Could not load tables: %s", j->tables.error);
        else
            set_status(t, "%zu table%s%s%s", tl ? tl->nrows : 0, tl && tl->nrows == 1 ? "" : "s",
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
        int browsed = j->browse && t->browsing && strcmp(j->browse, t->browsing) == 0;
        if (browsed) browse_done(t, j);
        if (j->out.error) {
            set_status(t, "%s", j->out.error);
        } else if (browsed && row_sets) { /* the page, not the 10000-row cap */
            char page[64];
            browse_page_label(page, sizeof page, j->offset, rows);
            page[0] = (char)tolower((unsigned char)page[0]); /* "rows 1,001–2,000" */
            set_status(t, "%zu row%s in %.3f s - %s%s%s", rows, rows == 1 ? "" : "s", j->seconds,
                       page, j->current_db ? " - database: " : "",
                       j->current_db ? j->current_db : "");
        } else if (row_sets)
            set_status(t, "%zu row%s in %.3f s%s%s%s", rows, rows == 1 ? "" : "s", j->seconds,
                       truncated ? " (grid shows the first 10000 rows per result)" : "",
                       j->current_db ? " - database: " : "",
                       j->current_db ? j->current_db : "");
        else
            set_status(t, "Query OK, %llu row%s affected in %.3f s", affected,
                       affected == 1 ? "" : "s", j->seconds);
        /* A query's rows from one table of the current database (the edits
         * name the table without its database) can be edited too. */
        const db_set *qs = browsed || j->lost ? NULL : editable_query_set(j);
        char *qtable = qs && j->current_db && strcmp(qs->org_db, j->current_db) == 0
                       ? strdup(qs->org_table) : NULL;
        if (browsed)
            show_outcome(t, &j->out, &j->meta, t->browsing, NULL);
        else
            show_outcome(t, &j->out, qtable ? &j->meta : NULL, qtable, j->sql);
        free(qtable);
    }

    /* The worker read DATABASE() after the job (see load_tables), unless the
     * connection was lost: the last one known is the one to reconnect to. */
    if (j->conn && !j->lost) {
        free(t->current_db);
        t->current_db = j->current_db;
        j->current_db = NULL;
        update_conn_label(t); /* the database may have changed via USE */
    }

    /* Refresh the sidebar (the database may have changed via USE). */
    if (j->load_tables) {
        db_outcome_free(&t->tables_raw);
        t->tables_raw = j->tables;
        memset(&j->tables, 0, sizeof j->tables);
        t->tables_loaded = 1;
    }
    uiButtonSetText(t->load_tables_btn, t->tables_loaded ? "Refresh tables" : "Load tables");
    /* Only when the list changed, so the selection survives e.g. an export. */
    if ((j->kind == JOB_CONNECT && !j->reconnect) || j->load_tables)
        rebuild_sidebar_keeping(t, keep, nkeep);
    free_names(keep, nkeep);

    /* again after the job's own messages, so this one is left in the status line */
    if (j->lost) connection_lost(t, j->lost_error);
    job_free(j);
    if (t->lost) { /* nothing more can run until Reconnect */
        free(t->pending_browse);
        t->pending_browse = NULL;
    }
    /* a double-click that came while the structure was loading */
    if (t->pending_browse) {
        char *name = t->pending_browse;
        t->pending_browse = NULL;
        browse_table(t, name);
        free(name);
    } else if (rebrowse) {
        /* Read the browsed table again (its page, with its filter), but
         * leave the editor's text and the mode as they were: they may hold
         * the user's own query. */
        start_browse(t, t->browsing, t->applied_col, t->applied_op, t->applied_value,
                     t->page_offset, BROWSE_KEEP_EDITOR);
    }
    structure_sync(t);
    update_browse_bars(t); /* browsing may have started or ended above */
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
    (void)b; tab *t = data;
    if (t->busy) return;
    job *j = calloc(1, sizeof *j);
    if (!j) return;
    j->kind = JOB_CONNECT;
    j->host = entry_dup(t->host);
    j->user = entry_dup(t->user);
    j->pass = entry_dup(t->pass);
    j->dbname = entry_dup(t->db);
    char *port = entry_dup(t->port);
    j->port = (unsigned int)strtoul(port, NULL, 10);
    free(port);
    if (!j->host[0]) { free(j->host); j->host = strdup("localhost"); }
    if (!j->dbname[0]) { free(j->dbname); j->dbname = NULL; }
    int sel = t->profile_sel;
    if (sel > 0 && (size_t)sel <= app.profiles.n)
        j->profile = strdup(app.profiles.items[sel - 1].name);
    j->load_tables = load_on_connect(t);
    j->hide_views = uiCheckboxChecked(t->hide_views);
    set_status(t, "Connecting to %s...", j->host);
    start_job(t, j);
}

/* After the connection was lost: a new one with the kept login, to the
 * database in use when it was lost (it may have changed via USE). */
static void on_reconnect(uiButton *b, void *data) {
    (void)b; tab *t = data;
    if (t->busy || !t->lost) return;
    job *j = calloc(1, sizeof *j);
    if (!j) return;
    const char *db = t->current_db ? t->current_db : t->conn_db;
    j->kind = JOB_CONNECT;
    j->reconnect = 1;
    j->host = strdup(t->conn_host ? t->conn_host : "localhost");
    j->user = strdup(t->conn_user ? t->conn_user : "");
    j->pass = strdup(t->conn_pass ? t->conn_pass : "");
    j->dbname = db ? strdup(db) : NULL;
    j->profile = t->profile ? strdup(t->profile) : NULL;
    if (!j->host || !j->user || !j->pass || (db && !j->dbname) || (t->profile && !j->profile)) {
        job_free(j);
        set_status(t, "Out of memory");
        return;
    }
    j->port = t->conn_port;
    j->load_tables = load_on_connect(t);
    j->hide_views = uiCheckboxChecked(t->hide_views);
    set_status(t, "Reconnecting to %s...", j->host);
    start_job(t, j);
}

#ifndef SQLC_AUTOTEST /* it would race the scripted steps */
/* Every minute, each idle tab's connection is pinged: that keeps the server
 * from closing it for inactivity (wait_timeout) and, if it was closed anyway
 * (restart, killed, network, sleep), says so right away with Reconnect. */
enum { KEEPALIVE_MS = 60 * 1000 };

static int keepalive_tick(void *data) {
    (void)data;
    if (app.gone) return 0; /* the timer stops */
    for (size_t i = 0; i < app.ntabs; i++) {
        tab *t = app.list[i];
        if (!t->conn || t->busy || t->closed) continue;
        job *j = calloc(1, sizeof *j);
        if (!j) continue;
        j->kind = JOB_PING;
        j->conn = t->conn;
        start_job(t, j);
    }
    return 1;
}
#endif

/* Returns 1 if the query was started. browse: the table it shows (for editing). */
static int run_query(tab *t, const char *sql, const char *browse) {
    if (t->busy) return 0;
    if (!t->conn) { set_status(t, "%s", not_connected_msg(t)); return 0; }

    const char *p = sql;
    while (isspace((unsigned char)*p)) p++;
    if (!*p) { set_status(t, "Nothing to run."); return 0; }

    job *j = calloc(1, sizeof *j);
    if (!j) return 0;
    j->kind = JOB_QUERY;
    j->conn = t->conn;
    j->sql = strdup(sql);
    j->browse = browse ? strdup(browse) : NULL;
    j->eo.hide_views = uiCheckboxChecked(t->hide_views);
    j->eo.max_rows = GUI_MAX_ROWS;
    /* With -A (or "Tables only, load once") the list is only refreshed when
     * asked (Load/Refresh tables). */
    j->load_tables = load_after_queries(t);
    j->hide_views = j->eo.hide_views;
    set_status(t, "Running...");
    start_job(t, j);
    /* that's where the results go (AI shows them too: stay there) */
    if (t->mode == MODE_STRUCTURE) show_mode(t, MODE_NORMAL);
    return 1;
}

static int run_sql(tab *t, const char *sql) { return run_query(t, sql, NULL); }

/*
 * Browse table `name` into an editable grid: one page of BROWSE_PAGE_ROWS
 * rows from row offset, filtered by col / op / value (col NULL, or an
 * inactive filter, see browse_filter_active: none). The SQL is built on the
 * worker (run_browse) and written into the editor when the rows are back,
 * unless BROWSE_KEEP_EDITOR; BROWSE_FRESH also resets the filter bar.
 * The arguments may point at t's own browse state. Returns 1 if started.
 */
static int start_browse(tab *t, const char *name, const char *col, browse_op op,
                        const char *value, unsigned long long offset, int flags) {
    if (t->busy) return 0;
    if (!t->conn) { set_status(t, "%s", not_connected_msg(t)); return 0; }
    browse_filter f = { col, op, value };
    int filtered = browse_filter_active(&f);
    int takes_value = filtered && browse_op_takes_value(op);
    job *j = calloc(1, sizeof *j);
    char *table = strdup(name);
    if (j) {
        j->browse = strdup(name);
        j->filter_col = filtered ? strdup(col) : NULL;
        j->filter_value = takes_value ? strdup(value) : NULL;
    }
    char *applied_col = filtered ? strdup(col) : NULL;
    char *applied_value = takes_value ? strdup(value) : NULL;
    if (!j || !table || !j->browse || (filtered && (!j->filter_col || !applied_col)) ||
        (takes_value && (!j->filter_value || !applied_value))) {
        if (j) job_free(j);
        free(table);
        free(applied_col);
        free(applied_value);
        set_status(t, "Out of memory");
        return 0;
    }
    j->kind = JOB_QUERY;
    j->conn = t->conn;
    j->filter_op = filtered ? op : BROWSE_EQ;
    j->offset = offset;
    j->fresh = (flags & BROWSE_FRESH) != 0;
    j->keep_editor = (flags & BROWSE_KEEP_EDITOR) != 0;
    j->eo.hide_views = uiCheckboxChecked(t->hide_views);
    j->load_tables = load_after_queries(t);
    j->hide_views = j->eo.hide_views;

    /* only now: the arguments may be these */
    free(t->browsing);
    t->browsing = table;
    free(t->applied_col);
    free(t->applied_value);
    t->applied_col = applied_col;
    t->applied_value = applied_value;
    t->applied_op = j->filter_op;
    t->page_offset = offset;

    set_status(t, "Running...");
    /* the editor's text is replaced: show it (not when it is kept) */
    if (!j->keep_editor) show_mode(t, MODE_NORMAL);
    start_job(t, j);
    return 1;
}

/* SELECT * FROM a table, into an editable grid: its first page, no
 * filter. Returns 1 if started. */
static int browse_table(tab *t, const char *name) {
    return start_browse(t, name, NULL, BROWSE_EQ, NULL, 0, BROWSE_FRESH);
}

/* The browsed table's page from row offset, with the applied filter. */
static void browse_page(tab *t, unsigned long long offset) {
    if (t->browsing)
        start_browse(t, t->browsing, t->applied_col, t->applied_op, t->applied_value, offset, 0);
}

/* The browsed table's current page again (Reload). */
static void reload_browse(tab *t) {
    browse_page(t, t->page_offset);
}

/* ---- the filter bar ---- */

/* Apply (or Return in the value): the bar's filter, from the first page.
 * An empty value with a value operator shows every row. */
static void apply_filter(tab *t) {
    if (t->busy || !t->browsing) return;
    int ci = uiComboboxSelected(t->filter_col);
    int op = uiComboboxSelected(t->filter_op);
    const char *col = ci >= 0 && (size_t)ci < t->nfilter_cols ? t->filter_cols[ci] : NULL;
    if (op < 0 || op >= BROWSE_NOPS) op = BROWSE_EQ;
    char *value = uiEntryText(t->filter_value);
    start_browse(t, t->browsing, col, (browse_op)op, value ? value : "", 0, 0);
    uiFreeText(value);
}

static void on_filter_apply(uiButton *b, void *data) {
    (void)b;
    apply_filter(data);
}

static void on_filter_return(void *data) { apply_filter(data); }

/* Reset: no filter, the default column, =, the first page. */
static void on_filter_reset(uiButton *b, void *data) {
    (void)b; tab *t = data;
    if (t->busy || !t->browsing) return;
    select_filter_column(t, NULL);
    uiComboboxSetSelected(t->filter_op, BROWSE_EQ);
    uiEntrySetText(t->filter_value, "");
    start_browse(t, t->browsing, NULL, BROWSE_EQ, NULL, 0, 0);
}

/* IS NULL / IS NOT NULL take no value: its field is disabled. */
static void on_filter_op(uiCombobox *c, void *data) {
    (void)c;
    update_browse_bars(data);
}

/* Read the editable grid's rows again: its query, or the browsed table. */
static void reload_rows(tab *t) {
    if (!t->edit.query) { reload_browse(t); return; }
    char *sql = strdup(t->edit.query); /* the grid (and its query) is replaced */
    if (sql) run_query(t, sql, NULL);
    free(sql);
}

/* Claude's SQL is about to run: ask first if it destroys data or structure.
 * Returns 1 to go ahead. */
static int confirm_ai_sql(tab *t, const char *sql) {
    if (!t->ai_in_editor) return 1;
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

static void on_run_menu(tab *t) {
    if (t->mode == MODE_AI) { /* the editor is hidden: ⌘↩ sends the request instead */
        ask_claude_now(t);
        return;
    }
    if (t->mode == MODE_STRUCTURE) { /* the editor is hidden: don't run it unseen */
        set_status(t, "Switch to Normal to run SQL (\xE2\x8C\x98\xE2\x86\xA9 runs the editor there).");
        return;
    }
    char *sql = uiMultilineEntryText(t->editor);
    if (t->busy || !t->conn || confirm_ai_sql(t, sql ? sql : "")) {
        if (run_sql(t, sql ? sql : "")) { /* the user's own SQL: no longer browsing a table */
            browse_forget(t);
            t->ai_in_editor = 0;
        }
    }
    uiFreeText(sql);
}

static void on_run(uiButton *b, void *data) {
    (void)b; tab *t = data;
    on_run_menu(t);
}

/* ---------- Ask Claude ---------- */

static const char AI_KEY_MESSAGE[] =
    "Paste your Anthropic API key (from console.anthropic.com). It is saved in "
    "sqlc's settings folder (~/Library/Application Support/sqlc), readable only by you.\n\n"
    "When you ask Claude, sqlc sends your request, the server version and the current "
    "database's table and column names, types and keys to the Claude API. Row data is "
    "never sent.";

/* Prompts for the key and stores it. Returns the key (malloc'd) or NULL. */
static char *prompt_api_key(tab *t) {
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
        set_status(t, "Claude API key saved in %s.", path);
    }
    free(path);
    return key;
}

/* The saved key, else $ANTHROPIC_API_KEY, else ask for one. */
static char *api_key(tab *t) {
    char *path = config_path(AI_KEY_FILE);
    char *key = path ? secret_read(path) : NULL;
    free(path);
    if (key) return key;
    const char *env = getenv("ANTHROPIC_API_KEY");
    if (env && *env) return strdup(env);
    return prompt_api_key(t);
}

static void on_set_api_key(uiMenuItem *m, uiWindow *w, void *data) {
    (void)m; (void)w; (void)data;
    tab *t = cur_tab();
    char *key = prompt_api_key(t);
    if (key) { memset(key, 0, strlen(key)); free(key); }
}

static void on_remove_api_key(uiMenuItem *m, uiWindow *w, void *data) {
    (void)m; (void)w; (void)data;
    tab *t = cur_tab();
    char *path = config_path(AI_KEY_FILE);
    if (path && secret_remove(path) == 0) set_status(t, "Claude API key removed.");
    else set_status(t, "Could not remove the Claude API key: %s", strerror(errno));
    free(path);
}

static void ask_claude_now(tab *t) {
    if (t->busy) return;
    if (!t->conn) {
        set_status(t, "%s", t->lost ? not_connected_msg(t)
                                    : "Connect first, so Claude can see the database's tables.");
        return;
    }
    char *txt = uiMultilineEntryText(t->ai_task);
    char *task = strdup(txt ? txt : "");
    uiFreeText(txt);
    if (!task) return;
    const char *p = task;
    while (isspace((unsigned char)*p)) p++;
    if (!*p) {
        set_status(t, "Describe the query you need in the Ask Claude field first.");
        free(task);
        return;
    }
    char *key = api_key(t);
    if (!key) { set_status(t, "Ask Claude needs an API key (AI \xE2\x80\xBA Set Claude API Key\xE2\x80\xA6)."); free(task); return; }

    job *j = calloc(1, sizeof *j);
    if (!j) { free(task); free(key); return; }
    j->kind = JOB_AI;
    j->conn = t->conn;
    j->task = task;
    j->api_key = key;
    set_status(t, "Asking Claude...");
    start_job(t, j);
}

static void on_ask_claude(uiButton *b, void *data) {
    (void)b; tab *t = data;
    ask_claude_now(t);
}

static void on_ask_claude_menu(uiMenuItem *m, uiWindow *w, void *data) {
    (void)m; (void)w; (void)data;
    tab *t = cur_tab();
    show_mode(t, MODE_AI); /* on the welcome screen: shown once connected */
    if (!t->conn) set_status(t, "%s", not_connected_msg(t));
}

static void load_tables_now(tab *t) {
    if (t->busy) return;
    if (!t->conn) { set_status(t, "%s", not_connected_msg(t)); return; }
    job *j = calloc(1, sizeof *j);
    if (!j) return;
    j->kind = JOB_TABLES;
    j->conn = t->conn;
    j->load_tables = 1;
    j->hide_views = uiCheckboxChecked(t->hide_views);
    set_status(t, "Loading tables...");
    start_job(t, j);
}

static void on_load_tables(uiButton *b, void *data) {
    (void)b; tab *t = data;
    load_tables_now(t);
}

/* One export at a time: returns 1 (and says so in the status line) if one
 * is running. */
static int export_running(tab *t) {
    if (!t->export_job) return 0;
    set_status(t, "An export is already running - wait for it or cancel it.");
    return 1;
}

/* A new export job: it opens its own connection with the kept login, to db
 * (NULL: the current database, else the one connected to). NULL if not
 * connected or out of memory. */
static job *new_export_job(tab *t, job_kind kind, const char *db) {
    if (!t->conn) return NULL;
    if (!db) db = t->current_db ? t->current_db : t->conn_db;
    job *j = calloc(1, sizeof *j);
    if (!j) return NULL;
    j->kind = kind;
    j->own_conn = 1;
    j->conn = NULL;
    j->host = strdup(t->conn_host ? t->conn_host : "localhost");
    j->user = strdup(t->conn_user ? t->conn_user : "");
    j->pass = strdup(t->conn_pass ? t->conn_pass : "");
    j->dbname = db ? strdup(db) : NULL;
    j->port = t->conn_port;
    if (!j->host || !j->user || !j->pass || (db && !j->dbname)) { job_free(j); return NULL; }
    return j;
}

/* Export a table on the worker thread (reusable from other entry points). */
static void start_export(tab *t, const char *table, int is_view, export_format fmt,
                         const export_opts *opts, const char *path) {
    if (!t->conn || export_running(t)) return;
    job *j = new_export_job(t, JOB_EXPORT, NULL);
    if (!j) return;
    j->table = strdup(table);
    j->path = strdup(path);
    j->is_view = is_view;
    j->fmt = fmt;
    j->xopts = *opts;
    if (!j->table || !j->path) { job_free(j); return; }
    set_status(t, "Exporting %s...", table);
    start_job(t, j);
}

static const char *table_at_row(tab *t, int row, int *is_view_out);

/* The table selected in the sidebar when exactly one row is selected, else
 * NULL (pointer into tables_raw). */
static const char *selected_table(tab *t, int *is_view_out) {
    if (!t->side_table || !t->side_data.nsets) return NULL;
    uiTableSelection *sel = uiTableGetSelection(t->side_table);
    int row = sel && sel->NumRows == 1 ? sel->Rows[0] : -1;
    uiFreeTableSelection(sel);
    return table_at_row(t, row, is_view_out);
}

/* How many sidebar rows are selected. */
static size_t selected_count(tab *t) {
    if (!t->side_table || !t->side_data.nsets) return 0;
    uiTableSelection *sel = uiTableGetSelection(t->side_table);
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
static int selected_tables(tab *t, char ***names_out, size_t *n_out) {
    *names_out = NULL;
    *n_out = 0;
    if (!t->side_table || !t->side_data.nsets) return 0;
    uiTableSelection *sel = uiTableGetSelection(t->side_table);
    if (!sel) return 0;
    size_t nrows = sel->NumRows > 0 ? (size_t)sel->NumRows : 0;
    char **names = nrows ? calloc(nrows, sizeof *names) : NULL;
    size_t n = 0;
    int rc = 0;
    if (nrows && !names) rc = -1;
    for (size_t i = 0; names && i < nrows; i++) {
        const char *nm = table_at_row(t, sel->Rows[i], NULL);
        if (!nm) continue;
        if (!(names[n] = strdup(nm))) { rc = -1; break; }
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
static void export_table_flow(tab *t, const char *table, int view) {
    if (!t->conn || export_running(t)) return;
    char *tc = strdup(table); /* the dialog runs a modal loop; keep our own copy */
    if (!tc) return;
    export_format fmt;
    export_opts opts;
    char *path = NULL;
    if (sqlc_export_dialog(tc, view, &fmt, &opts, &path)) start_export(t, tc, view, fmt, &opts, path);
    free(path);
    free(tc);
}

static void export_database_flow(tab *t);
static void export_tables_flow(tab *t, char **names, size_t n);

/* Export…: the whole database with nothing selected in the sidebar, the one
 * selected table (SQL / CSV / XML), or the selected tables into one SQL file. */
static void export_sidebar_selection(tab *t) {
    if (!t->conn) { set_status(t, "%s", not_connected_msg(t)); return; }
    if (export_running(t)) return;
    char **names = NULL;
    size_t n = 0;
    if (selected_tables(t, &names, &n) != 0) { set_status(t, "Out of memory"); return; }
    if (n == 0) {
        export_database_flow(t);
    } else if (n == 1) {
        int view = 0;
        const char *name = selected_table(t, &view);
        export_table_flow(t, name ? name : names[0], view);
    } else {
        export_tables_flow(t, names, n); /* takes names */
        names = NULL;
        n = 0;
    }
    free_names(names, n);
}

static void on_export(uiButton *b, void *data) {
    (void)b; tab *t = data;
    export_sidebar_selection(t);
}

static void on_export_menu(uiMenuItem *m, uiWindow *w, void *data) {
    (void)m; (void)w; (void)data;
    tab *t = cur_tab();
    export_sidebar_selection(t);
}

/* Export the current database (all tables, then views) on the worker thread. */
static void start_export_db(tab *t, const char *db, const export_opts *opts, const char *path) {
    if (!t->conn || export_running(t)) return;
    job *j = new_export_job(t, JOB_EXPORT_DB, db); /* db: the one asked for */
    if (!j) return;
    j->table = strdup(db);
    j->path = strdup(path);
    j->fmt = EXPORT_SQL;
    j->xopts = *opts;
    if (!j->table || !j->path) { job_free(j); return; }
    set_status(t, "Exporting database %s...", db);
    start_job(t, j);
}

/* Export the n named tables/views of database db into one SQL file on the
 * worker thread. Takes names (malloc'd, see selected_tables) in every case. */
static void start_export_tables(tab *t, const char *db, char **names, size_t n,
                                const export_opts *opts, const char *path) {
    job *j = !t->conn || !n || export_running(t) ? NULL : new_export_job(t, JOB_EXPORT_DB, db);
    if (!j) { free_names(names, n); return; }
    j->names = names;
    j->nnames = n;
    j->table = strdup(db);
    j->path = strdup(path);
    j->fmt = EXPORT_SQL;
    j->xopts = *opts;
    if (!j->table || !j->path) { job_free(j); return; }
    set_status(t, "Exporting %zu tables from %s...", n, db);
    start_job(t, j);
}

/* Options dialog + save panel for the selected tables. Takes names. */
static void export_tables_flow(tab *t, char **names, size_t n) {
    if (!t->conn || export_running(t)) { free_names(names, n); return; }
    if (!t->current_db) {
        free_names(names, n);
        uiMsgBoxError(app.win, "Export",
                      "No database selected. Pick one in the Database field and connect, "
                      "or run USE <database>; first.");
        return;
    }
    char *db = strdup(t->current_db); /* the dialog runs a modal loop; keep our own copy */
    if (!db) { free_names(names, n); return; }
    export_opts opts;
    char *path = NULL;
    if (sqlc_export_database_dialog(db, n, &opts, &path))
        start_export_tables(t, db, names, n, &opts, path);
    else
        free_names(names, n);
    free(path);
    free(db);
}

static void export_database_flow(tab *t) {
    if (!t->conn) { set_status(t, "%s", not_connected_msg(t)); return; }
    if (export_running(t)) return;
    if (!t->current_db) {
        uiMsgBoxError(app.win, "Export Database",
                      "No database selected. Pick one in the Database field and connect, "
                      "or run USE <database>; first.");
        return;
    }
    char *db = strdup(t->current_db); /* the dialog runs a modal loop; keep our own copy */
    if (!db) return;
    export_opts opts;
    char *path = NULL;
    if (sqlc_export_database_dialog(db, 0, &opts, &path)) start_export_db(t, db, &opts, path);
    free(path);
    free(db);
}

static void on_export_db_menu(uiMenuItem *m, uiWindow *w, void *data) {
    (void)m; (void)w; (void)data;
    tab *t = cur_tab();
    export_database_flow(t);
}

/* Typing in the sidebar's search field: filter the list we have (no reload). */
static void on_side_search(uiEntry *e, void *data) {
    (void)e; tab *t = data;
    rebuild_sidebar(t);
    if (t->refreshing && t->side_table) sqlc_table_set_enabled(t->side_table, 0);
}

static void on_hide_views(uiCheckbox *c, void *data) {
    (void)c; tab *t = data;
    /* Views are filtered on the server, so turning this off needs a reload
     * to bring them back. Filter what we have meanwhile (or if busy). */
    sync_bar_hide_views(t);
    rebuild_sidebar(t);
    if (t->tables_loaded) load_tables_now(t);
}

/* The connected bar's copy of Hide views: drives the real one. */
static void on_hide_views_bar(uiCheckbox *c, void *data) {
    tab *t = data;
    uiCheckboxSetChecked(t->hide_views, uiCheckboxChecked(c));
    on_hide_views(t->hide_views, t);
}

/* "Tables only, load once" implies Hide views and replaces -A: lock both. */
static void sync_tables_once(tab *t, int reload) {
    if (uiCheckboxChecked(t->tables_once)) {
        uiCheckboxSetChecked(t->no_rehash, 0);
        if (!uiCheckboxChecked(t->hide_views)) {
            uiCheckboxSetChecked(t->hide_views, 1);
            if (reload) on_hide_views(t->hide_views, t);
        }
        uiControlDisable(uiControl(t->hide_views));
        uiControlDisable(uiControl(t->no_rehash));
    } else {
        uiControlEnable(uiControl(t->hide_views));
        uiControlEnable(uiControl(t->no_rehash));
    }
    sync_bar_hide_views(t);
}

static void on_tables_once(uiCheckbox *c, void *data) {
    (void)c; tab *t = data;
    sync_tables_once(t, 1);
}

/* Double-click a table: SELECT * FROM it. */
static void on_table_double_click(uiTable *tbl, int row, void *data) {
    (void)tbl; tab *t = data;
    if (row < 0 || !t->side_data.nsets || (size_t)row >= t->side_data.sets[0].nrows) return;
    const char *name = db_cell(&t->tables_raw.sets[0], t->side_index[row], 0);
    if (!name) return;
    /* The first click of it may have started loading the structure: browse
     * once that's done. */
    if (t->busy && t->job_kind == JOB_STRUCTURE) {
        free(t->pending_browse);
        t->pending_browse = strdup(name);
        return;
    }
    browse_table(t, name);
}

/* ---------- sidebar context menu ---------- */

/* Name and kind of the table on a visible sidebar row (pointer into tables_raw). */
static const char *table_at_row(tab *t, int row, int *is_view_out) {
    if (row < 0 || !t->side_data.nsets || !t->tables_raw.nsets ||
        (size_t)row >= t->side_data.sets[0].nrows) return NULL;
    size_t r = t->side_index[row];
    if (is_view_out) *is_view_out = is_view(db_cell(&t->tables_raw.sets[0], r, 1));
    return db_cell(&t->tables_raw.sets[0], r, 0);
}

/* Rename, truncate or drop a table on the worker thread. */
static void start_alter(tab *t, alter_kind kind, const char *table, int view, const char *new_name) {
    if (t->busy || !t->conn) return;
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
    j->conn = t->conn;
    j->sql = sql;
    j->alter = kind;
    j->table = strdup(table);
    j->new_name = new_name ? strdup(new_name) : NULL;
    j->is_view = view;
    j->load_tables = t->tables_loaded; /* show the change in the sidebar */
    j->hide_views = uiCheckboxChecked(t->hide_views);
    set_status(t, "Running %s...", sql);
    start_job(t, j);
}

static unsigned on_table_menu_open(int row, void *data) {
    tab *t = data;
    int view = 0;
    const char *name = table_at_row(t, row, &view);
    free(t->menu_table);
    t->menu_table = name ? strdup(name) : NULL;
    t->menu_is_view = view;
    if (!t->menu_table) return 0;
    unsigned items = TABLE_MENU_BIT(TABLE_MENU_COPY_NAME);
    /* an export has its own connection: not held back by a running job */
    if (t->conn) items |= TABLE_MENU_BIT(TABLE_MENU_STRUCTURE) | TABLE_MENU_BIT(TABLE_MENU_EXPORT);
    if (t->conn && !t->busy) {
        items |= TABLE_MENU_BIT(TABLE_MENU_RENAME) | TABLE_MENU_BIT(TABLE_MENU_DELETE);
        if (!view) items |= TABLE_MENU_BIT(TABLE_MENU_TRUNCATE); /* not for views */
    }
    return items;
}

static void export_table_flow(tab *t, const char *table, int view);

static void on_table_menu_action(table_menu_item item, int row, void *data) {
    (void)row; tab *t = data;
    if (!t->menu_table) return;
    /* the name as it was when the menu opened, in case the list changed since */
    char *name = strdup(t->menu_table);
    int view = t->menu_is_view;
    if (!name) return;
    const char *what = view ? "view" : "table";
    char title[600], msg[600];

    switch (item) {
    case TABLE_MENU_STRUCTURE:
        show_mode(t, MODE_STRUCTURE); /* the menu selected its row */
        break;
    case TABLE_MENU_COPY_NAME:
        sqlc_copy_to_clipboard(name);
        set_status(t, "Copied \"%s\" to the clipboard", name);
        break;
    case TABLE_MENU_RENAME: {
        snprintf(title, sizeof title, "Rename %s `%s`", what, name);
        char *new_name = sqlc_prompt(title, "New name:", name, "Rename");
        if (new_name && new_name[0] && strcmp(new_name, name) != 0)
            start_alter(t, ALTER_RENAME, name, view, new_name);
        free(new_name);
        break;
    }
    case TABLE_MENU_TRUNCATE:
        if (view) break;
        snprintf(title, sizeof title, "Truncate table `%s`?", name);
        if (sqlc_confirm_danger(title, "All rows will be deleted. This cannot be undone.", "Truncate", 0))
            start_alter(t, ALTER_TRUNCATE, name, view, NULL);
        break;
    case TABLE_MENU_DELETE:
        snprintf(title, sizeof title, "Drop %s `%s`?", what, name);
        snprintf(msg, sizeof msg, view ? "The view will be permanently deleted."
                                       : "The table and all its data will be permanently deleted.");
        if (sqlc_confirm_danger(title, msg, "Delete", 1))
            start_alter(t, ALTER_DROP, name, view, NULL);
        break;
    case TABLE_MENU_EXPORT:
        export_table_flow(t, name, view);
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

static row_edit_escaper escaper(tab *t) {
    row_edit_escaper e = { escape_value, t->conn };
    return e;
}

static int cell_editable(tab *t, int row, int col) {
    grid_edit_state *e = &t->edit;
    return e->grid && !e->readonly && row >= 0 && (size_t)row < e->set->nrows &&
           col >= 0 && (unsigned)col < e->info.ncols && row_edit_editable(&e->info, (unsigned)col);
}

static grid_editor_kind editor_kind(col_kind k) {
    return k == COL_DATE ? GRID_EDITOR_DATE : k == COL_DATETIME ? GRID_EDITOR_DATETIME
                                                                : GRID_EDITOR_TEXT;
}

/* Open the editor on a cell, with `text` (NULL = the cell's value). */
static void begin_cell_edit(tab *t, int row, int col, const char *text) {
    grid_edit_state *e = &t->edit;
    if (!e->grid) return;
    if (!cell_editable(t, row, col)) {
        if (e->readonly) set_status(t, "%s", e->readonly);
        else if (col >= 0 && (unsigned)col < e->info.ncols)
            set_status(t, "Column `%s` can't be edited here (binary or generated).", e->info.cols[col].name);
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
static int next_editable(tab *t, int row, int col, int dir) {
    for (int c = col + dir; c >= 0 && (unsigned)c < t->edit.info.ncols; c += dir)
        if (cell_editable(t, row, c)) return c;
    return -1;
}

static int edit_job_ok(tab *t) {
    if (t->busy) { set_status(t, "Busy - wait for the running query to finish, then try again."); return 0; }
    if (!t->conn || !t->edit.table) { set_status(t, "%s", not_connected_msg(t)); return 0; }
    return 1;
}

static job *new_rows_job(tab *t, row_op op, char *sql) {
    job *j = calloc(1, sizeof *j);
    if (!j) { free(sql); return NULL; }
    j->kind = JOB_ROWS;
    j->op = op;
    j->conn = t->conn;
    j->sql = sql;
    j->edit_set = t->edit.set;
    j->next_col = -1;
    return j;
}

/* Save one cell of a saved row (value NULL = SQL NULL). */
static void start_row_update(tab *t, int row, int col, const char *value, const char *typed, int next_col) {
    if (!edit_job_ok(t)) { begin_cell_edit(t, row, col, typed); return; }
    char err[512];
    row_edit_escaper esc = escaper(t);
    char *sql = row_edit_update_sql(t->edit.table, &t->edit.info, t->edit.set, (size_t)row,
                                    (unsigned)col, value, &esc, err, sizeof err);
    if (!sql) { set_status(t, "%s", err); show_error("Can't save this change", err); return; }
    job *j = new_rows_job(t, ROW_UPDATE, sql);
    if (!j) return;
    j->row = row;
    j->col = col;
    j->next_col = next_col;
    j->value = value ? strdup(value) : NULL;
    j->typed = strdup(typed);
    set_status(t, "Saving %s...", t->edit.info.cols[col].name);
    start_job(t, j);
}

/* INSERT the unsaved new row; col is the cell the user was in. */
static void start_row_insert(tab *t, int col) {
    grid_edit_state *e = &t->edit;
    if (e->new_row < 0) return;
    if (!edit_job_ok(t)) return;
    char err[512];
    row_edit_escaper esc = escaper(t);
    char *sql = row_edit_insert_sql(t->edit.table, &e->info, e->set, (size_t)e->new_row,
                                    e->new_set, &esc, err, sizeof err);
    if (!sql) { set_status(t, "%s", err); show_error("Can't add this row", err); return; }
    job *j = new_rows_job(t, ROW_INSERT, sql);
    if (!j) return;
    j->row = e->new_row;
    j->col = col;
    set_status(t, "Adding the row...");
    start_job(t, j);
}

static void set_cell(tab *t, int row, int col, char *value /* taken */) {
    char **cell = &t->edit.set->cells[(size_t)row * t->edit.set->ncols + (unsigned)col];
    free(*cell);
    *cell = value;
}

/* The editor closed with text in (row, col). */
static void on_cell_committed(int row, int col, const char *text, grid_end how, void *data) {
    tab *t = data;
    grid_edit_state *e = &t->edit;
    if (!cell_editable(t, row, col)) return;
    int is_new = row == e->new_row;
    int next = how == GRID_END_TAB ? next_editable(t, row, col, 1)
             : how == GRID_END_BACKTAB ? next_editable(t, row, col, -1) : -1;
    const char *old = db_cell(e->set, (size_t)row, (unsigned)col);

    int unchanged = is_new ? !e->new_set[col] && !*text
                           : old ? strcmp(old, text) == 0 : !*text;
    if (!unchanged) {
        char err[512];
        int is_null = 0;
        char *v = row_edit_normalize(&e->info.cols[col], text, &is_null, err, sizeof err);
        if (!v && !is_null) {
            set_status(t, "%s", err);
            show_error("Invalid value", err);
            begin_cell_edit(t, row, col, text); /* keep what was typed, to fix it */
            return;
        }
        if (!is_new) { /* saved row: UPDATE now, then go on to `next` */
            start_row_update(t, row, col, v, text, next);
            free(v);
            return;
        }
        set_cell(t, row, col, v);
        e->new_set[col] = 1;
        uiTableModelRowChanged(e->model->model, row);
    }
    if (is_new && how == GRID_END_RETURN) start_row_insert(t, col);
    else if (next >= 0) begin_cell_edit(t, row, next, NULL);
}

static void on_cell_cancelled(int row, int col, void *data) {
    (void)row; (void)col; tab *t = data;
    if (t->edit.new_row >= 0)
        set_status(t, "The new row isn't saved yet: press Return in one of its cells to add it, "
                   "or select it and press \xE2\x88\x92 to discard it.");
}

static void on_cell_double_clicked(int row, int col, void *data) {
    tab *t = data;
    begin_cell_edit(t, row, col, NULL);
}

/* + : a new row under the last one, edited in place. */
static void on_add_row(uiButton *b, void *data) {
    (void)b; tab *t = data;
    grid_edit_state *e = &t->edit;
    if (!e->grid || e->readonly || t->busy) return;
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
        set_status(t, "New row: type the values (Tab moves to the next column) and press Return "
                   "to save it. Cells left as (default) get the column's default.");
    } else {
        set_status(t, "Finish the new row first: press Return in one of its cells to save it.");
    }
    /* start in the first column the user would fill in (not an auto-increment id) */
    for (unsigned c = 0; c < s->ncols && first < 0; c++)
        if (cell_editable(t, e->new_row, (int)c) && !e->info.cols[c].auto_increment) first = (int)c;
    if (first < 0) first = next_editable(t, e->new_row, -1, 1);
    sqlc_grid_scroll_to_row(e->grid, e->new_row);
    if (first >= 0) begin_cell_edit(t, e->new_row, first, NULL);
}

static void remove_grid_row(tab *t, int row) {
    grid_edit_state *e = &t->edit;
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
static void delete_selected_rows(tab *t) {
    grid_edit_state *e = &t->edit;
    if (!e->grid || e->readonly) return;
    if (!edit_job_ok(t)) return;
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
        set_status(t, "Select the rows to delete first (\xE2\x8C\x98-click or \xE2\x87\xA7-click selects several).");
        return;
    }
    for (size_t i = 1; i < nsaved; i++) /* ascending, for removing them later */
        for (size_t k = i; k > 0 && rows[k - 1] > rows[k]; k--) {
            size_t tmp = rows[k]; rows[k] = rows[k - 1]; rows[k - 1] = tmp;
        }

    char title[600], msg[300];
    size_t total = nsaved + (size_t)drop_new;
    if (total == 1) snprintf(title, sizeof title, "Delete 1 row from `%s`?", e->table);
    else snprintf(title, sizeof title, "Delete %zu rows from `%s`?", total, e->table);
    snprintf(msg, sizeof msg, "%s", nsaved ? "This can't be undone." : "The new row isn't saved yet; it will be discarded.");
    if (!confirm_danger(title, msg, "Delete", 1)) { free(rows); return; }

    if (nsaved == 0) { /* only the unsaved row: nothing to tell the server */
        remove_grid_row(t, e->new_row);
        free(rows);
        set_status(t, "Discarded the new row.");
        return;
    }
    char err[512];
    row_edit_escaper esc = escaper(t);
    char *sql = row_edit_delete_sql(e->table, &e->info, e->set, rows, nsaved, &esc, err, sizeof err);
    if (!sql) { free(rows); set_status(t, "%s", err); show_error("Can't delete these rows", err); return; }
    job *j = new_rows_job(t, ROW_DELETE, sql);
    if (!j) { free(rows); return; }
    j->del_rows = rows;
    j->ndel = nsaved;
    j->drop_new = drop_new;
    set_status(t, "Deleting %zu row%s...", nsaved, nsaved == 1 ? "" : "s");
    start_job(t, j);
}

static void on_delete_rows(uiButton *b, void *data) {
    (void)b; tab *t = data;
    delete_selected_rows(t);
}

static void on_reload_rows(uiButton *b, void *data) {
    (void)b; tab *t = data;
    if (t->busy) return;
    if (t->edit.new_row >= 0) set_status(t, "Reloading: the unsaved new row was discarded.");
    reload_rows(t);
}

/* Delete all rows: TRUNCATE TABLE, then the grid reloads (see alter_done). */
static void on_clear_rows(uiButton *b, void *data) {
    (void)b; tab *t = data;
    if (!t->edit.grid || t->edit.readonly || !edit_job_ok(t)) return;
    if (t->edit.query) return; /* hidden for a query's rows */
    char *name = strdup(t->edit.table);
    if (!name) return;
    char title[600];
    snprintf(title, sizeof title, "Delete all rows of `%s`?", name);
    if (confirm_danger(title,
                       "TRUNCATE TABLE removes every row, including those not shown here, "
                       "and resets AUTO_INCREMENT. This can't be undone.",
                       "Delete All Rows", 1))
        start_alter(t, ALTER_TRUNCATE, name, 0, NULL);
    free(name);
}

/* Copy row 0 of src (a SELECT * of the table) into grid row `row`. */
static void replace_grid_row(tab *t, int row, const db_set *src) {
    for (unsigned c = 0; c < t->edit.set->ncols; c++) {
        const char *v = db_cell(src, 0, c);
        set_cell(t, row, (int)c, v ? strdup(v) : NULL);
    }
    uiTableModelRowChanged(t->edit.model->model, row);
}

/* The row read back after an UPDATE / INSERT, if there is exactly one. */
static const db_set *refreshed_row(tab *t, const job *j) {
    const db_set *s = j->out.nsets >= 2 ? &j->out.sets[1] : NULL;
    return s && s->has_rows && s->nrows == 1 && s->ncols == t->edit.set->ncols ? s : NULL;
}

static void rows_done(tab *t, job *j) {
    grid_edit_state *e = &t->edit;
    const char *err = j->out.error ? j->out.error : "unknown error";
    if (!e->grid || e->set != j->edit_set) { /* the grid was replaced meanwhile */
        set_status(t, j->ok ? "Saved." : "Failed: %s", err);
        return;
    }
    switch (j->op) {
    case ROW_UPDATE: {
        if (!j->ok) {
            set_status(t, "Update failed: %s", err);
            show_error("Update failed", err);
            begin_cell_edit(t, j->row, j->col, j->typed); /* keep the edit, to fix it */
            break;
        }
        const db_set *r = refreshed_row(t, j);
        if (r) replace_grid_row(t, j->row, r);
        else { /* not found again (e.g. a trigger changed it): show what we sent */
            set_cell(t, j->row, j->col, j->value ? strdup(j->value) : NULL);
            uiTableModelRowChanged(e->model->model, j->row);
        }
        unsigned long long n = j->out.nsets ? j->out.sets[0].affected : 0;
        set_status(t, n ? "Saved %s in row %d of %s." : "%s in row %d of %s: no change.",
                   e->info.cols[j->col].name, j->row + 1, e->table);
        if (j->next_col >= 0) begin_cell_edit(t, j->row, j->next_col, NULL);
        break;
    }
    case ROW_INSERT: {
        if (!j->ok) {
            set_status(t, "Insert failed: %s", err);
            show_error("Insert failed", err);
            if (e->new_row >= 0) begin_cell_edit(t, e->new_row, j->col, NULL);
            break;
        }
        const db_set *r = refreshed_row(t, j);
        if (!r) { /* can't find it again (no key): read the table back */
            set_status(t, "Added 1 row to %s.", e->table);
            reload_rows(t);
            break;
        }
        int row = e->new_row;
        e->new_row = -1;
        e->model->pending_row = -1;
        e->model->pending_set = NULL;
        free(e->new_set);
        e->new_set = NULL;
        replace_grid_row(t, row, r);
        set_status(t, "Added 1 row to %s.", e->table);
        break;
    }
    case ROW_DELETE: {
        if (!j->ok) {
            set_status(t, "Delete failed (nothing was deleted): %s", err);
            show_error("Delete failed", err);
            break;
        }
        unsigned long long gone = 0;
        for (size_t i = 0; i < j->out.nsets; i++) gone += j->out.sets[i].affected;
        if (j->drop_new && e->new_row >= 0) remove_grid_row(t, e->new_row);
        for (size_t i = j->ndel; i-- > 0;) remove_grid_row(t, (int)j->del_rows[i]);
        set_status(t, "Deleted %llu row%s from %s.", gone, gone == 1 ? "" : "s", e->table);
        break;
    }
    }
}

/* ---- cell context menu ---- */

static unsigned on_cell_menu_open(int row, int col, void *data) {
    tab *t = data;
    grid_edit_state *e = &t->edit;
    if (!e->grid || row < 0 || (size_t)row >= e->set->nrows) return 0;
    unsigned items = 0;
    if (col >= 0) items |= GRID_MENU_BIT(GRID_MENU_COPY);
    if (!e->readonly && !t->busy) {
        items |= GRID_MENU_BIT(GRID_MENU_DELETE_ROWS);
        if (cell_editable(t, row, col)) {
            items |= GRID_MENU_BIT(GRID_MENU_EDIT);
            int is_default = row == e->new_row && !e->new_set[col];
            if (e->info.cols[col].nullable && (is_default || db_cell(e->set, (size_t)row, (unsigned)col)))
                items |= GRID_MENU_BIT(GRID_MENU_SET_NULL);
        }
    }
    return items;
}

static void on_cell_menu_action(grid_menu_item item, int row, int col, void *data) {
    tab *t = data;
    grid_edit_state *e = &t->edit;
    if (!e->grid || row < 0 || (size_t)row >= e->set->nrows) return;
    switch (item) {
    case GRID_MENU_EDIT:
        begin_cell_edit(t, row, col, NULL);
        break;
    case GRID_MENU_SET_NULL:
        if (!cell_editable(t, row, col)) break;
        if (row == e->new_row) {
            set_cell(t, row, col, NULL);
            e->new_set[col] = 1;
            uiTableModelRowChanged(e->model->model, row);
        } else {
            start_row_update(t, row, col, NULL, "", -1);
        }
        break;
    case GRID_MENU_COPY: {
        if (col < 0 || (unsigned)col >= e->set->ncols) break;
        const char *v = db_cell(e->set, (size_t)row, (unsigned)col);
        sqlc_copy_to_clipboard(v ? v : "NULL");
        set_status(t, "Copied the value to the clipboard.");
        break;
    }
    case GRID_MENU_DELETE_ROWS:
        delete_selected_rows(t);
        break;
    default:
        break;
    }
}

/* ---- the page: grid + toolbar ---- */

static uiButton *toolbar_button(tab *t, uiBox *bar, const char *text, void (*f)(uiButton *, void *)) {
    uiButton *b = uiNewButton(text);
    uiButtonOnClicked(b, f, t);
    uiBoxAppend(bar, uiControl(b), 0);
    return b;
}

/* < and > under a browsed table's grid: the page before / after, with the
 * same filter. */
static void on_prev_page(uiButton *b, void *data) {
    (void)b; tab *t = data;
    if (t->busy || !t->browsing || t->page_offset == 0) return;
    browse_page(t, t->page_offset > BROWSE_PAGE_ROWS ? t->page_offset - BROWSE_PAGE_ROWS : 0);
}

static void on_next_page(uiButton *b, void *data) {
    (void)b; tab *t = data;
    if (t->busy || !t->browsing || !t->page_has_next) return;
    if (t->page_offset > ULLONG_MAX - BROWSE_PAGE_ROWS) return;
    browse_page(t, t->page_offset + BROWSE_PAGE_ROWS);
}

/* The grid of a browsed table, with + / - / Reload / Delete All Rows below,
 * and for a browse (query NULL) the page bar < label > under that.
 * meta: its columns (sets[0]) and TABLE_TYPE (sets[1]). */
static uiControl *editable_page(tab *t, set_model *sm, const db_outcome *meta,
                               const char *table, const char *query) {
    grid_edit_state *e = &t->edit;
    clear_edit_state(t);
    e->model = sm;
    e->set = sm->set;
    e->table = strdup(table);
    e->query = query ? strdup(query) : NULL;

    const db_set *cols = meta->nsets > 0 ? &meta->sets[0] : NULL;
    const db_set *type = meta->nsets > 1 ? &meta->sets[1] : NULL;
    const char *ttype = type && type->nrows ? db_cell(type, 0, 0) : NULL;
    if (!e->table || (query && !e->query))
        e->readonly = "Out of memory: these rows can't be edited.";
    else if (is_view(ttype))
        e->readonly = "Views are read-only here: browse the table itself to edit rows.";
    else if (table_info_load(&e->info, cols, sm->set) != 0)
        e->readonly = query ? "These rows can't be edited: only plain columns of one table can "
                              "(no expressions, and no column twice)."
                            : "This table's columns couldn't be read, so its rows can't be edited.";
    else if (!e->info.npk && !e->info.whole)
        e->readonly = "Add the table's primary key to the query to edit these rows.";

    uiTable *grid = new_grid(sm);
    e->grid = grid;
    uiTableSetSelectionMode(grid, uiTableSelectionModeZeroOrMany);
    grid_edit_callbacks cb = {
        .double_clicked = on_cell_double_clicked,
        .committed = on_cell_committed,
        .cancelled = on_cell_cancelled,
        .menu_open = on_cell_menu_open,
        .menu_action = on_cell_menu_action,
        .data = t,
    };
    sqlc_grid_enable_editing(grid, &cb);

    uiBox *page = uiNewVerticalBox();
    uiBoxSetPadded(page, 1);
    uiBoxAppend(page, uiControl(grid), 1);

    uiBox *bar = uiNewHorizontalBox();
    uiBoxSetPadded(bar, 1);
    e->add_btn = toolbar_button(t, bar, "+", on_add_row);
    e->del_btn = toolbar_button(t, bar, "\xE2\x88\x92", on_delete_rows); /* "−" */
    e->reload_btn = toolbar_button(t, bar, "Reload", on_reload_rows);
    e->clear_btn = toolbar_button(t, bar, "Delete All Rows\xE2\x80\xA6", on_clear_rows);
    /* a query may show only some rows: emptying the whole table from here
     * would be a surprise (browse the table for that) */
    if (e->query) uiControlHide(uiControl(e->clear_btn));
    const char *hint = e->readonly ? e->readonly
        : e->info.npk ? "Double-click a cell to edit: Return saves, Tab goes to the next column, "
                        "Esc cancels. Right-click for Set to NULL."
        : "Double-click a cell to edit (Return saves, Esc cancels). No primary key: rows are "
          "found by their values, one at a time.";
    char buf[1200];
    if (e->query && !e->readonly && e->table) { /* say which table the edits go to */
        snprintf(buf, sizeof buf, "Rows of `%s`. %s", e->table, hint);
        hint = buf;
    }
    uiBoxAppend(bar, uiControl(uiNewLabel(hint)), 0);
    uiBoxAppend(page, uiControl(bar), 0);
    if (!e->query) { /* a browsed table: its pages (labelled by update_browse_bars) */
        uiBox *pages = uiNewHorizontalBox();
        uiBoxSetPadded(pages, 1);
        e->prev_btn = toolbar_button(t, pages, "<", on_prev_page);
        e->page_label = uiNewLabel("");
        uiBoxAppend(pages, uiControl(e->page_label), 0);
        e->next_btn = toolbar_button(t, pages, ">", on_next_page);
        uiBoxAppend(page, uiControl(pages), 0);
    }
    update_edit_buttons(t);
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

static void on_saved_selection(uiTable *tbl, void *data);
static void on_saved_double_click(uiTable *tbl, int row, void *data);

static void fill_saved_list_now(tab *t, const char *name) {
    if (t->saved_table) {
        uiBoxDelete(t->saved_box, uiBoxNumChildren(t->saved_box) - 1);
        uiControlDestroy(uiControl(t->saved_table));
        t->saved_table = NULL;
    }
    free_set_model(t->saved_model);
    t->saved_model = NULL;
    db_outcome_free(&t->saved_data);
    t->profile_sel = 0;

    size_t n = app.profiles.n + 1; /* "New connection" first */
    if (n > (size_t)INT_MAX) return;
    db_set *s = calloc(1, sizeof *s);
    if (!s) return;
    t->saved_data.sets = s; /* from here on db_outcome_free cleans up */
    t->saved_data.nsets = 1;
    s->has_rows = 1;
    s->ncols = 1;
    s->names = calloc(1, sizeof *s->names);
    s->numeric = calloc(1, sizeof *s->numeric);
    s->cells = calloc(n, sizeof *s->cells);
    if (!s->names || !s->numeric || !s->cells) { db_outcome_free(&t->saved_data); return; }
    s->nrows = n;
    s->names[0] = strdup("Saved connections");
    s->cells[0] = strdup("New connection");
    for (size_t i = 0; i < app.profiles.n; i++) {
        s->cells[i + 1] = strdup(app.profiles.items[i].name);
        if (name && strcmp(app.profiles.items[i].name, name) == 0) t->profile_sel = (int)i + 1;
    }
    if (!s->names[0]) { db_outcome_free(&t->saved_data); return; }

    t->saved_model = new_set_model(s);
    if (!t->saved_model) { db_outcome_free(&t->saved_data); return; }
    t->saved_table = new_grid(t->saved_model);
    uiTableSetSelectionMode(t->saved_table, uiTableSelectionModeOne);
    sqlc_table_fill_width(t->saved_table); /* column follows the list's width */
    uiTableOnSelectionChanged(t->saved_table, on_saved_selection, t);
    uiTableOnRowDoubleClicked(t->saved_table, on_saved_double_click, t);
    uiBoxAppend(t->saved_box, uiControl(t->saved_table), 1);
    int row = t->profile_sel;
    uiTableSelection sel = { .NumRows = 1, .Rows = &row };
    uiTableSetSelection(t->saved_table, &sel);
    if (t->connecting) sqlc_table_set_enabled(t->saved_table, 0); /* as set_connecting left it */
}

/* Refill the saved-connections list and select the profile called name (or
 * "New connection"). Doesn't touch the form. */
static void fill_saved_list(tab *t, const char *name) {
    t->filling_saved = 1; /* selecting from code calls on_saved_selection */
    fill_saved_list_now(t, name);
    t->filling_saved = 0;
}

/* The saved connections are shared by all tabs. Before they change: the
 * name each tab has selected (malloc'd array, one per tab in app.list,
 * NULL for "New connection"; NULL if out of memory). */
static char **saved_selections(void) {
    char **names = calloc(app.ntabs ? app.ntabs : 1, sizeof *names);
    for (size_t i = 0; names && i < app.ntabs; i++) {
        int sel = app.list[i]->profile_sel;
        if (sel > 0 && (size_t)sel <= app.profiles.n)
            names[i] = strdup(app.profiles.items[sel - 1].name); /* NULL: "New connection" */
    }
    return names;
}

/* ... and after: t's list selects name, every other tab's list is refilled
 * keeping its selection where that still exists (their forms are left as
 * they are). Takes and frees names. */
static void refill_saved_lists(tab *t, const char *name, char **names) {
    for (size_t i = 0; i < app.ntabs; i++) {
        tab *u = app.list[i];
        fill_saved_list(u, u == t ? name : names ? names[i] : NULL);
    }
    for (size_t i = 0; names && i < app.ntabs; i++) free(names[i]);
    free(names);
}

/* Fill the connection fields from the selected saved connection. */
static void apply_selected_profile(tab *t) {
    int sel = t->profile_sel;
    if (sel <= 0 || (size_t)sel > app.profiles.n) {
        uiEntrySetText(t->profile_name, "");
        return;
    }
    const profile *p = &app.profiles.items[sel - 1];
    char port[16];
    snprintf(port, sizeof port, "%u", p->port ? p->port : 3306);
    uiEntrySetText(t->profile_name, p->name);
    uiEntrySetText(t->host, p->host);
    uiEntrySetText(t->port, port);
    uiEntrySetText(t->user, p->user);
    uiEntrySetText(t->db, p->database);
    uiCheckboxSetChecked(t->remember_pw, p->remember_password);
    uiCheckboxSetChecked(t->hide_views, p->hide_views);
    uiCheckboxSetChecked(t->no_rehash, p->no_auto_rehash);
    uiCheckboxSetChecked(t->tables_once, p->tables_once);
    sync_tables_once(t, 0);
    rebuild_sidebar(t);

    uiEntrySetText(t->pass, p->remember_password && p->password ? p->password : "");
}

/* The selected row of the saved-connections list, or -1. */
static int saved_selected_row(uiTable *t) {
    uiTableSelection *sel = uiTableGetSelection(t);
    if (!sel) return -1;
    int row = sel->NumRows == 1 ? sel->Rows[0] : -1;
    uiFreeTableSelection(sel);
    return row >= 0 && (size_t)row <= app.profiles.n ? row : -1;
}

static void on_saved_selection(uiTable *tbl, void *data) {
    tab *t = data;
    if (t->filling_saved) return;
    int row = saved_selected_row(tbl);
    if (row < 0 || row == t->profile_sel) return;
    t->profile_sel = row;
    apply_selected_profile(t);
}

/* Double-click a saved connection: connect with it, when it has what it needs. */
static void on_saved_double_click(uiTable *tbl, int row, void *data) {
    (void)tbl; tab *t = data;
    if (row <= 0 || (size_t)row > app.profiles.n || t->busy) return;
    if (row != t->profile_sel) {
        t->profile_sel = row;
        apply_selected_profile(t);
    }
    if (!app.profiles.items[row - 1].remember_password) {
        set_status(t, "Type the password for \"%s\", then press Connect.",
                   app.profiles.items[row - 1].name);
        return;
    }
    on_connect(t->connect_btn, t);
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
    (void)b; tab *t = data;
    char *name = trimmed_entry(t->profile_name);
    if (!name) return;
    if (!profile_name_valid(name)) {
        uiMsgBoxError(app.win, "Can't save connection",
                      "Enter a name for this connection (no line breaks, '[' or ']').");
        free(name);
        return;
    }

    char *port = entry_dup(t->port);
    unsigned long portn = port ? strtoul(port, NULL, 10) : 0;
    free(port);
    profile p = {
        .name = name,
        .host = entry_dup(t->host),
        .port = portn > 0 && portn <= 65535 ? (unsigned int)portn : 3306,
        .user = entry_dup(t->user),
        .database = entry_dup(t->db),
        .remember_password = uiCheckboxChecked(t->remember_pw),
        .hide_views = uiCheckboxChecked(t->hide_views),
        .no_auto_rehash = uiCheckboxChecked(t->no_rehash),
        .tables_once = uiCheckboxChecked(t->tables_once),
    };
    p.password = p.remember_password ? entry_dup(t->pass) : NULL;
    int existed = profiles_find(&app.profiles, name) != NULL;
    char **sels = saved_selections(); /* the list changes in every tab */

    if (profiles_put(&app.profiles, &p) < 0) {
        free_names(sels, app.ntabs);
        set_status(t, "Out of memory");
    } else {
        profiles_set_last(&app.profiles, name);
        int saved = save_profiles_file() == 0;
        refill_saved_lists(t, name, sels);
        if (saved) {
            uiCheckboxSetChecked(t->remember_pw, p.remember_password);
            set_status(t, "%s connection \"%s\".", existed ? "Updated" : "Saved", name);
        }
    }
    free(p.name); free(p.host); free(p.user); free(p.database);
    if (p.password) { memset(p.password, 0, strlen(p.password)); free(p.password); }
}

static void on_delete_profile(uiButton *b, void *data) {
    (void)b; tab *t = data;
    int sel = t->profile_sel;
    if (sel <= 0 || (size_t)sel > app.profiles.n) {
        set_status(t, "Pick a saved connection to delete.");
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
    char **sels = saved_selections(); /* the list changes in every tab */
    profiles_remove(&app.profiles, name);
    if (save_profiles_file() == 0) set_status(t, "Deleted connection \"%s\".", name);
    refill_saved_lists(t, NULL, sels);
    uiEntrySetText(t->profile_name, "");
    free(name);
}

/* After a successful connect: preselect this profile on the next launch. */
static void remember_last_profile(const char *name) {
    if (!name || !profiles_find(&app.profiles, name)) return;
    if (app.profiles.last && strcmp(app.profiles.last, name) == 0) return;
    if (profiles_set_last(&app.profiles, name) == 0) save_profiles_file();
}

/* Once, at launch, before the first tab. Returns 0, or an errno value if
 * the file exists but could not be read (the list starts empty then). */
static int load_profiles(void) {
    app.profiles_file = profiles_path();
    if (app.profiles_file && profiles_load(app.profiles_file, &app.profiles) != 0)
        return errno ? errno : EIO;
    return 0;
}

/* Close the connection and go back to the welcome screen. The editor keeps
 * its text; the results, table list and structure belonged to the server. */
static void on_disconnect(uiButton *b, void *data) {
    (void)b; tab *t = data;
    if (t->busy || (!t->conn && !t->lost)) return; /* the worker may be using it */
    /* An export runs on its own connection, so it doesn't stop a disconnect,
     * but it doesn't outlive it either: ask, then cancel it. It ends on its
     * own and says "Export cancelled.". */
    if (t->export_job) {
        if (!confirm_danger("Disconnect?",
                            "An export is running. Disconnecting cancels it and deletes the "
                            "partial file.", "Disconnect", 0))
            return;
        if (t->export_job) { /* it may have ended while the alert was open */
            atomic_store(&t->export_job->cancel, 1);
            uiControlDisable(uiControl(t->cancel_export_btn));
        }
    }
    int had_new_row = t->edit.new_row >= 0;
    if (t->conn) mysql_close(t->conn);
    t->conn = NULL;
    t->lost = 0;
    uiControlHide(uiControl(t->reconnect_btn));
    forget_login(t);
    free(t->profile);
    t->profile = NULL;

    clear_results(t);
    db_outcome_free(&t->tables_raw);
    t->tables_loaded = 0;
    uiButtonSetText(t->load_tables_btn, "Load tables");
    uiEntrySetText(t->side_search, ""); /* no OnChanged: rebuilt just below */
    rebuild_sidebar_keeping(t, NULL, 0);
    browse_forget(t);
    free(t->pending_browse);
    t->pending_browse = NULL;
    free(t->current_db);
    t->current_db = NULL;
    structure_message(t, "");

    show_screen(t);
    set_status(t, had_new_row ? "Disconnected - the unsaved new row was discarded."
                           : "Disconnected.");
}

#ifdef SQLC_AUTOTEST
/*
 * Scripted smoke test (make gui-test): drives the real callbacks against
 * the database in $SQLC_TEST_DB, prints what the UI would show, then quits.
 */
static int autotest_step;

static void autotest_print(const char *what) {
    tab *t = cur_tab(); /* the autotest drives the front (first) tab */
    char *st = uiLabelText(t->status);
    printf("[%s] status: %s\n", what, st);
    uiFreeText(st);
    printf("[%s] result sets: %zu, grids: %zu, error page: %s\n", what,
           t->shown.nsets, t->nmodels, t->shown.error ? "yes" : "no");
    const db_set *s = t->side_data.nsets ? &t->side_data.sets[0] : NULL;
    printf("[%s] sidebar:", what);
    for (size_t r = 0; s && r < s->nrows; r++) printf(" [%s]", db_cell(s, r, 0));
    printf("\n");
    printf("[%s] table list loaded: %s, rows from server: %zu\n", what,
           t->tables_loaded ? "yes" : "no",
           t->tables_raw.nsets ? t->tables_raw.sets[0].nrows : 0);
    fflush(stdout);
}

/* Visible sidebar row of a table, or -1. */
static int autotest_row(const char *name) {
    tab *t = cur_tab(); /* the autotest drives the front (first) tab */
    for (size_t r = 0; t->side_data.nsets && r < t->side_data.sets[0].nrows; r++) {
        const char *nm = table_at_row(t, (int)r, NULL);
        if (nm && strcmp(nm, name) == 0) return (int)r;
    }
    return -1;
}

static void autotest_status(const char *what) {
    tab *t = cur_tab(); /* the autotest drives the front (first) tab */
    char *st = uiLabelText(t->status);
    printf("[%s] status: %s\n", what, st);
    uiFreeText(st);
    fflush(stdout);
}

static void autotest_editor(const char *what) {
    tab *t = cur_tab(); /* the autotest drives the front (first) tab */
    int row, col;
    char *text = NULL;
    if (t->edit.grid && sqlc_grid_editing(t->edit.grid, &row, &col, &text))
        printf("[editor] %s: row %d, column %s, text [%s]%s\n", what, row,
               t->edit.info.cols[col].name, text, sqlc_grid_editor_has_calendar(t->edit.grid) ? ", calendar" : "");
    else
        printf("[editor] %s: closed\n", what);
    free(text);
    fflush(stdout);
}

static void autotest_row_print(const char *what, int row) {
    tab *t = cur_tab(); /* the autotest drives the front (first) tab */
    const db_set *s = t->edit.set;
    printf("[row] %s:", what);
    for (unsigned c = 0; s && row >= 0 && (size_t)row < s->nrows && c < s->ncols; c++) {
        const char *v = db_cell(s, (size_t)row, c);
        int dflt = row == t->edit.new_row && t->edit.new_set && !t->edit.new_set[c];
        printf(" %s=%s", s->names[c], dflt ? "(default)" : v ? v : "NULL");
    }
    printf("\n");
    autotest_status(what);
}

static void autotest_structure(const char *what) {
    tab *t = cur_tab(); /* the autotest drives the front (first) tab */
    char *title = uiLabelText(t->st.title);
    printf("[structure] %s: %s\n", what, title);
    uiFreeText(title);
    const db_set *sets[2] = { t->st.data.nsets > 0 ? &t->st.data.sets[0] : NULL,
                              t->st.data.nsets > 1 ? &t->st.data.sets[1] : NULL };
    for (int k = 0; k < 2; k++) {
        const db_set *s = sets[k];
        for (size_t r = 0; s && r < s->nrows; r++) {
            printf("[structure]   %s:", k ? "index" : "column");
            for (unsigned c = 0; c < s->ncols; c++) {
                const char *v = db_cell(s, r, c);
                printf(" %s=%s", s->names[c], v ? v : "NULL");
            }
            printf("\n");
        }
    }
    printf("[structure] grids: %s, %s\n", t->st.cols_model ? "columns" : "-",
           t->st.idx_model ? "indexes" : "-");
    autotest_status(what);
}

static void autotest_resume(void *data) {
    (void)data;
    autotest_next();
}

/* The filter bar, the page bar and the editor's first line. */
static void autotest_browse(const char *what) {
    tab *t = cur_tab(); /* the autotest drives the front (first) tab */
    char *sql = uiMultilineEntryText(t->editor);
    char *label = t->edit.page_label ? uiLabelText(t->edit.page_label) : NULL;
    int ci = uiComboboxSelected(t->filter_col);
    printf("[browse] %s: browsing %s, editor [%.*s]\n", what, t->browsing ? t->browsing : "-",
           sql ? (int)strcspn(sql, "\n") : 0, sql ? sql : "");
    printf("[browse] %s: filter bar %s, column %s, value field %s, reset %s; page bar %s%s%s, "
           "< %s, > %s\n", what,
           uiControlEnabled(uiControl(t->filter_apply)) ? "on" : "off",
           ci >= 0 && (size_t)ci < t->nfilter_cols ? t->filter_cols[ci] : "-",
           uiControlEnabled(uiControl(t->filter_value)) ? "on" : "off",
           uiControlEnabled(uiControl(t->filter_reset)) ? "on" : "off",
           label ? "[" : "none", label ? label : "", label ? "]" : "",
           t->edit.prev_btn && uiControlEnabled(uiControl(t->edit.prev_btn)) ? "on" : "off",
           t->edit.next_btn && uiControlEnabled(uiControl(t->edit.next_btn)) ? "on" : "off");
    uiFreeText(label);
    uiFreeText(sql);
    autotest_status(what);
}

/* A step that may not have started a job: go on anyway. */
static void autotest_continue_if_idle(void) {
    if (!cur_tab()->busy) uiQueueMain(autotest_resume, NULL);
}

static void autotest_next(void) {
    tab *t = cur_tab(); /* the autotest drives the front (first) tab */
    /* a follow-up job or an export is running; its end calls us again */
    if (t->busy || t->export_job) return;
    switch (autotest_step++) {
    case 0:
        autotest_print("connect");
        uiMultilineEntrySetText(t->editor,
            "SELECT * FROM customers; UPDATE orders SET total = total; SELECT nope;");
        on_run_menu(t);
        break;
    case 1:
        autotest_print("multi");
        uiCheckboxSetChecked(t->hide_views, 1);
        on_hide_views(t->hide_views, t); /* reloads without views */
        break;
    case 2:
        autotest_print("hide views");
        on_table_double_click(t->side_table, 0, t);
        break;
    case 3:
        autotest_print("double-click");
        uiMultilineEntrySetText(t->editor, "SHOW TABLES;");
        on_run_menu(t);
        break;
    case 4:
        autotest_print("show tables, views hidden");
        uiMultilineEntrySetText(t->editor, "SELECT seq FROM seq_1_to_20000;");
        on_run_menu(t);
        break;
    case 5:
        autotest_print("20000 rows");
        printf("[20000 rows] grid rows kept: %zu, truncated: %d\n",
               t->shown.sets[0].nrows, t->shown.sets[0].truncated);
        /* save the current connection as a profile, then read the file back */
        uiEntrySetText(t->profile_name, "autotest");
        uiCheckboxSetChecked(t->remember_pw, 0);
        uiCheckboxSetChecked(t->no_rehash, 1);
        on_save_profile(NULL, t);
        {
            profile_list back;
            int rc = profiles_load(app.profiles_file, &back);
            profile *p = rc == 0 ? profiles_find(&back, "autotest") : NULL;
            printf("[profiles] saved: %s, db: %s, hide_views: %d, no_auto_rehash: %d, "
                   "last: %s, list rows: %zu, selected row: %d\n",
                   p ? "yes" : "no", p ? p->database : "-", p ? p->hide_views : -1,
                   p ? p->no_auto_rehash : -1, back.last ? back.last : "-",
                   t->saved_data.nsets ? t->saved_data.sets[0].nrows : 0, t->profile_sel);
            profiles_free(&back);
        }
        on_delete_profile(NULL, t);
        printf("[profiles] after delete: %zu saved, list rows: %zu, selected row: %d\n",
               app.profiles.n, t->saved_data.nsets ? t->saved_data.sets[0].nrows : 0,
               t->profile_sel);
        /* sidebar divider: min width, and the table column follows it */
        sqlc_split_set_position(t->split, 400);
        printf("[sidebar] width 400 -> %d, column %d\n",
               sqlc_split_position(t->split), uiTableColumnWidth(t->side_table, 0));
        sqlc_split_set_position(t->split, 50);
        printf("[sidebar] width 50 -> %d (min 120), column %d\n",
               sqlc_split_position(t->split), uiTableColumnWidth(t->side_table, 0));
        sqlc_split_set_position(t->split, 5000);
        printf("[sidebar] width 5000 -> %d (window leaves room for the editor)\n",
               sqlc_split_position(t->split));
        /* editor / results divider: a 20000-row grid must not squeeze the
         * query pane away (the drag limits are checked at the end) */
        printf("[split] editor height after a big result: %d (min 100)\n",
               sqlc_split_position(t->query_split));
        /* export the selected sidebar table (the dialog is skipped) */
        {
            int r0 = 0;
            uiTableSelection sel = { .NumRows = 1, .Rows = &r0 };
            uiTableSetSelection(t->side_table, &sel);
            int view = -1;
            const char *nm = selected_table(t, &view);
            printf("[export] selected: %s (view: %d)\n", nm ? nm : "-", view);
            export_opts o = export_default_opts();
            if (nm) start_export(t, nm, view, EXPORT_CSV, &o, "build/autotest-export.csv");
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
        printf("[export db] current database: %s\n", t->current_db ? t->current_db : "-");
        {
            export_opts o = export_default_opts();
            if (t->current_db) start_export_db(t, t->current_db, &o, "build/autotest-export-db.sql");
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
        on_connect(t->connect_btn, t);
        break;
    case 8:
        autotest_print("connect -A");
        load_tables_now(t);
        break;
    case 9:
        autotest_print("load tables");
        /* context menu: work on a scratch table, with -A off so the list follows */
        uiCheckboxSetChecked(t->no_rehash, 0);
        run_sql(t, "DROP TABLE IF EXISTS ctx_tmp; CREATE TABLE ctx_tmp (id INT); "
                "INSERT INTO ctx_tmp VALUES (1), (2);");
        break;
    case 10: {
        int row = autotest_row("ctx_tmp");
        char menu[512];
        sqlc_table_menu_describe(t->side_table, row, menu, sizeof menu);
        printf("[menu] table: %s\n", menu);
        printf("[menu] selected after right-click: %s\n", selected_table(t, NULL) ? selected_table(t, NULL) : "-");
        sqlc_table_menu_describe(t->side_table, 999, menu, sizeof menu);
        printf("[menu] empty space: [%s]\n", menu);
        /* Copy table name (keeping the user's clipboard) */
        char *saved = sqlc_clipboard_text();
        sqlc_table_menu_describe(t->side_table, row, menu, sizeof menu);
        on_table_menu_action(TABLE_MENU_COPY_NAME, row, t);
        char *clip = sqlc_clipboard_text();
        printf("[menu] clipboard: %s\n", clip ? clip : "-");
        free(clip);
        if (saved) sqlc_copy_to_clipboard(saved);
        free(saved);
        on_table_double_click(t->side_table, row, t); /* browse it */
        break;
    }
    case 11:
        autotest_print("browse ctx_tmp");
        start_alter(t, ALTER_RENAME, "ctx_tmp", 0, "ctx `odd` name");
        break;
    case 12: {
        autotest_print("rename");
        char *ed = uiMultilineEntryText(t->editor);
        printf("[rename] editor: %s, browsing: %s\n", ed, t->browsing ? t->browsing : "-");
        uiFreeText(ed);
        start_alter(t, ALTER_TRUNCATE, "ctx `odd` name", 0, NULL);
        break;
    }
    case 13:
        autotest_print("truncate");
        run_sql(t, "SELECT COUNT(*) FROM `ctx ``odd`` name`");
        break;
    case 14:
        printf("[truncate] rows left: %s\n", db_cell(&t->shown.sets[0], 0, 0));
        on_table_double_click(t->side_table, autotest_row("ctx `odd` name"), t);
        break;
    case 15:
        start_alter(t, ALTER_DROP, "ctx `odd` name", 0, NULL);
        break;
    case 16:
        autotest_print("drop");
        printf("[drop] browsing: %s\n", t->browsing ? t->browsing : "-");
        start_alter(t, ALTER_DROP, "ctx `odd` name", 0, NULL); /* again: server error */
        break;
    case 17:
        autotest_print("drop again");
        /* views: Truncate is disabled */
        uiCheckboxSetChecked(t->hide_views, 0);
        on_hide_views(t->hide_views, t);
        break;
    case 18: {
        char menu[512];
        sqlc_table_menu_describe(t->side_table, autotest_row("customer_totals"), menu, sizeof menu);
        printf("[menu] view: %s\n", menu);
        /* "Tables only, load once": no views on connect, no refresh after queries */
        uiCheckboxSetChecked(t->tables_once, 1);
        on_tables_once(t->tables_once, t);
        printf("[tables once] hide views: %d, -A: %d\n",
               uiCheckboxChecked(t->hide_views), uiCheckboxChecked(t->no_rehash));
        break; /* turning on Hide views reloads the list */
    }
    case 19:
        on_connect(t->connect_btn, t);
        break;
    case 20:
        autotest_print("connect tables once");
        run_sql(t, "DROP TABLE IF EXISTS once_tmp; CREATE TABLE once_tmp (id INT);");
        break;
    case 21:
        autotest_print("query, tables once");
        printf("[tables once] once_tmp in sidebar: %s\n", autotest_row("once_tmp") >= 0 ? "yes" : "no");
        load_tables_now(t);
        break;
    case 22:
        printf("[tables once] after refresh, once_tmp in sidebar: %s\n",
               autotest_row("once_tmp") >= 0 ? "yes" : "no");
        uiCheckboxSetChecked(t->tables_once, 0);
        on_tables_once(t->tables_once, t);
        run_sql(t, "DROP TABLE once_tmp;");
        break;
    case 23: {
        /* the Normal / AI / Structure switch: Normal and AI swap the query
         * pane in place above the same results; Structure replaces both */
        show_mode(t, MODE_AI);
        printf("[view] AI: ai view %d, editor view %d, results %d, structure %d, switch %d\n",
               uiControlVisible(t->ai_view), uiControlVisible(t->normal_view),
               uiControlVisible(t->query_split), uiControlVisible(t->structure_view),
               sqlc_segmented_selected(t->mode_switch));
        show_mode(t, MODE_NORMAL);
        printf("[view] Normal: ai view %d, editor view %d, results %d, structure %d, switch %d\n",
               uiControlVisible(t->ai_view), uiControlVisible(t->normal_view),
               uiControlVisible(t->query_split), uiControlVisible(t->structure_view),
               sqlc_segmented_selected(t->mode_switch));
        /* Claude's SQL (no network here: as if Ask Claude had filled the
         * editor): a destructive statement asks first; the test cancels. */
        ai_result r = { .explanation = strdup("Deletes every customer."),
                        .sql = strdup("DELETE FROM customers;") };
        char *text = ai_format("remove all customers", &r);
        uiMultilineEntrySetText(t->editor, text);
        t->ai_in_editor = 1;
        free(text);
        ai_result_free(&r);
        on_run_menu(t);
        printf("[ai] destructive SQL ran: %s\n", t->busy ? "yes" : "no");
        /* a read-only query from Claude runs without asking */
        uiMultilineEntrySetText(t->editor, "-- Request: count customers\nSELECT COUNT(*) FROM customers;\n");
        on_run_menu(t);
        printf("[ai] read-only SQL ran: %s, still marked as Claude's: %d\n",
               t->busy ? "yes" : "no", t->ai_in_editor);
        break;
    }
    case 24:
        autotest_print("ai read-only");
        /* row editing: scratch tables with and without a primary key */
        run_sql(t, "DROP TABLE IF EXISTS edit_tmp; "
                "CREATE TABLE edit_tmp (id INT AUTO_INCREMENT PRIMARY KEY, "
                "name VARCHAR(20) NOT NULL DEFAULT 'anon', qty INT NULL, born DATE NULL, "
                "seen DATETIME NULL, data BLOB NULL); "
                "INSERT INTO edit_tmp (name, qty, born) VALUES ('Ann', 1, '2000-01-01'), "
                "('Bob', 2, NULL), ('Cy', 3, NULL); "
                "DROP TABLE IF EXISTS nopk_tmp; CREATE TABLE nopk_tmp (a INT, b VARCHAR(10)); "
                "INSERT INTO nopk_tmp VALUES (1, 'x'), (1, 'x'), (2, 'y');");
        break;
    case 25:
        browse_table(t, "edit_tmp");
        break;
    case 26: {
        autotest_print("browse edit_tmp");
        grid_edit_state *e = &t->edit;
        printf("[edit] grid: %s, read-only: %s, columns: %u, key columns: %u\n",
               e->grid ? "yes" : "no", e->readonly ? e->readonly : "no", e->info.ncols, e->info.npk);
        on_cell_double_clicked(0, 5, t); /* blob: refused */
        autotest_status("blob");
        /* double-click a cell, type, Return */
        on_cell_double_clicked(0, 1, t);
        autotest_editor("double-click name");
        sqlc_grid_test_finish(e->grid, "Annie", GRID_END_RETURN);
        break;
    }
    case 27:
        autotest_row_print("after edit", 0);
        /* a date: the calendar, then Tab to the datetime column */
        begin_cell_edit(t, 1, 3, NULL);
        autotest_editor("born (NULL)");
        sqlc_grid_test_pick_date(t->edit.grid, "2024-02-29");
        autotest_editor("picked in calendar");
        sqlc_grid_test_finish(t->edit.grid, "2024-02-29", GRID_END_TAB);
        break;
    case 28:
        autotest_row_print("after date", 1);
        autotest_editor("tabbed to seen");
        sqlc_grid_test_finish(t->edit.grid, "2024-1-5 9:30", GRID_END_RETURN);
        break;
    case 29:
        autotest_row_print("after datetime", 1);
        /* not a number: refused, the editor stays open with the text */
        begin_cell_edit(t, 0, 2, NULL);
        sqlc_grid_test_finish(t->edit.grid, "abc", GRID_END_RETURN);
        uiQueueMain(autotest_resume, NULL); /* runs after the editor's callback */
        break;
    case 30:
        autotest_editor("after bad number");
        sqlc_grid_close_editor(t->edit.grid);
        {
            char menu[16];
            unsigned bits = on_cell_menu_open(0, 2, t);
            snprintf(menu, sizeof menu, "%c%c%c%c", bits & GRID_MENU_BIT(GRID_MENU_EDIT) ? 'E' : '-',
                     bits & GRID_MENU_BIT(GRID_MENU_SET_NULL) ? 'N' : '-',
                     bits & GRID_MENU_BIT(GRID_MENU_COPY) ? 'C' : '-',
                     bits & GRID_MENU_BIT(GRID_MENU_DELETE_ROWS) ? 'D' : '-');
            printf("[edit] menu on qty: %s, on name (NOT NULL): %s\n", menu,
                   on_cell_menu_open(0, 1, t) & GRID_MENU_BIT(GRID_MENU_SET_NULL) ? "Set to NULL" : "no Set to NULL");
        }
        on_cell_menu_action(GRID_MENU_SET_NULL, 0, 2, t);
        break;
    case 31:
        autotest_row_print("after set NULL", 0);
        /* + : a new row, typed in place */
        on_add_row(NULL, t);
        autotest_editor("new row");
        autotest_row_print("new row", t->edit.new_row);
        sqlc_grid_test_finish(t->edit.grid, "Dee", GRID_END_TAB);
        uiQueueMain(autotest_resume, NULL);
        break;
    case 32:
        autotest_editor("new row, tab");
        sqlc_grid_test_finish(t->edit.grid, "7", GRID_END_RETURN);
        break;
    case 33: {
        autotest_row_print("inserted", 3);
        printf("[edit] unsaved row: %d, rows: %zu\n", t->edit.new_row, t->edit.set->nrows);
        /* select rows 1 and 3 (Annie, Cy) and delete them */
        int pick[] = { 0, 2 };
        uiTableSelection sel = { .NumRows = 2, .Rows = pick };
        uiTableSetSelection(t->edit.grid, &sel);
        delete_selected_rows(t);
        break;
    }
    case 34: {
        autotest_status("delete 2");
        for (size_t r = 0; r < t->edit.set->nrows; r++) autotest_row_print("left", (int)r);
        /* Cancel in the "are you sure?" keeps the rows */
        autotest_confirm = 0;
        int pick = 0;
        uiTableSelection sel = { .NumRows = 1, .Rows = &pick };
        uiTableSetSelection(t->edit.grid, &sel);
        delete_selected_rows(t);
        autotest_confirm = 1;
        printf("[edit] after cancelled delete: %zu rows, busy: %d\n", t->edit.set->nrows, t->busy);
        /* an unsaved new row is just discarded */
        on_add_row(NULL, t);
        sqlc_grid_close_editor(t->edit.grid);
        pick = t->edit.new_row;
        uiTableSetSelection(t->edit.grid, &sel);
        delete_selected_rows(t);
        printf("[edit] after discarding the new row: %zu rows, unsaved row: %d, busy: %d\n",
               t->edit.set->nrows, t->edit.new_row, t->busy);
        on_clear_rows(NULL, t); /* truncate, then the grid reloads */
        break;
    }
    case 35:
        autotest_status("delete all");
        printf("[edit] rows: %zu, grid editable: %s\n", t->edit.set ? t->edit.set->nrows : 0,
               t->edit.grid && !t->edit.readonly ? "yes" : "no");
        /* a row with only defaults */
        on_add_row(NULL, t);
        sqlc_grid_test_finish(t->edit.grid, "", GRID_END_RETURN);
        break;
    case 36:
        autotest_row_print("defaults", 0);
        browse_table(t, "nopk_tmp");
        break;
    case 37:
        printf("[nopk] key columns: %u, read-only: %s\n", t->edit.info.npk,
               t->edit.readonly ? t->edit.readonly : "no");
        begin_cell_edit(t, 1, 1, NULL);
        sqlc_grid_test_finish(t->edit.grid, "z", GRID_END_RETURN);
        break;
    case 38:
        autotest_status("nopk edit");
        run_sql(t, "SELECT GROUP_CONCAT(b ORDER BY b) FROM nopk_tmp");
        break;
    case 39:
        printf("[nopk] values now: %s\n", db_cell(&t->shown.sets[0], 0, 0));
        browse_table(t, "customer_totals");
        break;
    case 40:
        printf("[view] grid: %s, read-only: %s\n", t->edit.grid ? "yes" : "no",
               t->edit.readonly ? t->edit.readonly : "no");
        on_cell_double_clicked(0, 0, t);
        printf("[view] editor opened: %s\n", sqlc_grid_editing(t->edit.grid, NULL, NULL, NULL) ? "yes" : "no");
        run_sql(t, "DROP TABLE edit_tmp; DROP TABLE nopk_tmp;");
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
                uiTableSetSelection(t->side_table, &sel2);
            }
            rebuild_sidebar(t);
            char **names = NULL;
            size_t n = 0;
            int rc = selected_tables(t, &names, &n);
            printf("[multi] selected: %zu (rc %d):", n, rc);
            for (size_t i = 0; i < n; i++) printf(" [%s]", names[i]);
            printf(", single: %s\n", selected_table(t, NULL) ? selected_table(t, NULL) : "-");
            free_names(names, n);
            show_mode(t, MODE_STRUCTURE);
            char *title = uiLabelText(t->st.title);
            printf("[multi] structure: %s, loading: %s\n", title,
                   t->busy && t->job_kind == JOB_STRUCTURE ? "yes" : "no");
            uiFreeText(title);
            show_mode(t, MODE_NORMAL);
        }
        /* Structure page: select a table, switch to it */
        int row = autotest_row("orders");
        uiTableSelection sel = { .NumRows = 1, .Rows = &row };
        uiTableSetSelection(t->side_table, &sel);
        show_mode(t, MODE_STRUCTURE);
        printf("[structure] switch: %d, page shown: %d, query page shown: %d, loading: %s\n",
               sqlc_segmented_selected(t->mode_switch), uiControlVisible(t->structure_view),
               uiControlVisible(t->query_split),
               t->busy && t->job_kind == JOB_STRUCTURE ? "yes" : "no");
        break;
    }
    case 42: {
        autotest_structure("orders");
        char menu[512];
        int row = autotest_row("customers");
        sqlc_table_menu_describe(t->side_table, row, menu, sizeof menu);
        printf("[structure] menu: %s\n", menu);
        on_table_menu_action(TABLE_MENU_STRUCTURE, row, t); /* loads customers */
        break;
    }
    case 43: {
        autotest_structure("menu");
        /* the same table again: nothing to load */
        structure_sync(t); /* what the queued selection callback does */
        printf("[structure] reselect same table, loading: %s\n", t->busy ? "yes" : "no");
        /* a view */
        int row = autotest_row("customer_totals");
        if (row < 0) { /* views hidden earlier: bring them back first */
            uiCheckboxSetChecked(t->hide_views, 0);
            on_hide_views(t->hide_views, t);
            autotest_step--; /* come back here once the list is reloaded */
            break;
        }
        uiTableSelection sel = { .NumRows = 1, .Rows = &row };
        uiTableSetSelection(t->side_table, &sel);
        structure_sync(t); /* what the queued selection callback does */
        break;
    }
    case 44: {
        autotest_structure("view");
        /* double-click while on Structure: the click loads the structure,
         * the double-click browses once that's done */
        int row = autotest_row("customers");
        uiTableSelection sel = { .NumRows = 1, .Rows = &row };
        uiTableSetSelection(t->side_table, &sel);
        structure_sync(t); /* what the queued selection callback does */
        on_table_double_click(t->side_table, row, t);
        printf("[structure] double-click while loading, queued: %s\n",
               t->pending_browse ? t->pending_browse : "-");
        break;
    }
    case 45:
        autotest_print("browse after structure");
        printf("[structure] mode: %s, browsing: %s, sidebar selection kept: %s\n",
               t->mode == MODE_NORMAL ? "Normal" : t->mode == MODE_AI ? "AI" : "Structure",
               t->browsing ? t->browsing : "-",
               selected_table(t, NULL) ? selected_table(t, NULL) : "-");
        /* the editor / results divider */
        sqlc_split_set_position(t->query_split, 300);
        printf("[split] editor height: %d\n", sqlc_split_position(t->query_split));
        sqlc_split_set_position(t->query_split, 5);
        printf("[split] dragged to the top: %d (min 100)\n", sqlc_split_position(t->query_split));
        sqlc_split_set_position(t->query_split, 5000);
        printf("[split] dragged to the bottom: editor %d (results keep their minimum)\n",
               sqlc_split_position(t->query_split));
        sqlc_split_set_position(t->query_split, 200);
        /* ⌘↩ on Structure runs nothing (the editor is hidden), only a hint;
         * customers' structure is already shown, so nothing loads either */
        show_mode(t, MODE_STRUCTURE);
        on_run_menu(t);
        printf("[structure] cmd-return on Structure: busy %d, results page shown %d\n",
               t->busy, uiControlVisible(t->query_split));
        show_mode(t, MODE_NORMAL);
        /* filter bar and pages on the browsed customers table */
        autotest_browse("browse customers");
        uiComboboxSetSelected(t->filter_op, BROWSE_IS_NOT_NULL);
        on_filter_op(t->filter_op, t);
        printf("[browse] IS NOT NULL picked: value field %s\n",
               uiControlEnabled(uiControl(t->filter_value)) ? "on" : "off");
        apply_filter(t);
        autotest_continue_if_idle();
        break;
    case 46:
        autotest_browse("IS NOT NULL");
        /* a quote in a LIKE value: escaped in the editor's SELECT */
        uiComboboxSetSelected(t->filter_op, BROWSE_LIKE);
        on_filter_op(t->filter_op, t);
        uiEntrySetText(t->filter_value, "O'%");
        apply_filter(t);
        autotest_continue_if_idle();
        break;
    case 47:
        autotest_browse("LIKE quote");
        on_filter_reset(t->filter_reset, t);
        autotest_continue_if_idle();
        break;
    case 48:
        autotest_browse("reset");
        on_next_page(t->edit.next_btn, t); /* only with more than 1000 customers */
        printf("[browse] next page started: %s\n", t->busy ? "yes" : "no");
        autotest_continue_if_idle();
        break;
    case 49:
        autotest_browse("after next");
        /* the user's own query: no filter bar, no page bar */
        uiMultilineEntrySetText(t->editor, "SELECT * FROM customers LIMIT 2");
        on_run_menu(t);
        autotest_continue_if_idle();
        break;
    case 50:
        autotest_browse("own query");
        /* Disconnect: back to the welcome screen, nothing of the server left */
        on_disconnect(t->disconnect_btn, t);
        printf("[welcome] after disconnect: connected %d, welcome shown %d, layout shown %d, "
               "sidebar rows %zu, result sets %zu\n",
               t->conn != NULL, uiControlVisible(t->welcome), uiControlVisible(t->workspace),
               t->side_data.nsets ? t->side_data.sets[0].nrows : 0, t->shown.nsets);
        autotest_status("disconnect");
        fflush(stdout);
        if (on_should_quit(NULL)) uiQuit();
        break;
    }
}

static void autotest_start(void *data) {
    tab *t = cur_tab(); /* the autotest drives the front (first) tab */
    (void)data;
    const char *db = getenv("SQLC_TEST_DB");
    printf("[welcome] at launch: welcome shown %d, layout shown %d\n",
           uiControlVisible(t->welcome), uiControlVisible(t->workspace));
    uiEntrySetText(t->db, db ? db : "");
    on_connect(t->connect_btn, t);
}
#endif

/* ---------- tabs, quitting ---------- */

/* Index of t in app.list (and in app.pages and the tab strip), or -1 once
 * closed. */
static int tab_index(const tab *t) {
    for (size_t i = 0; i < app.ntabs; i++)
        if (app.list[i] == t) return (int)i;
    return -1;
}

/* t's title, see tab_title in tabs.h. */
static void tab_title_of(const tab *t, char *buf, size_t size) {
    int on = t->conn || t->lost;
    tab_title(buf, size, on ? t->profile : NULL, t->conn_user,
              on ? t->conn_host : NULL, t->current_db);
}

/* The window's title follows the tab in front: "sqlc - <tab title>". */
static void update_window_title(void) {
    if (!app.ntabs) return;
    char title[TAB_TITLE_MAX + 1], buf[TAB_TITLE_MAX + 16];
    tab_title_of(cur_tab(), title, sizeof title);
    snprintf(buf, sizeof buf, "sqlc - %s", title);
    uiWindowSetTitle(app.win, buf);
}

/* t's tab label (and the window's title if it's in front), after a
 * connect, a disconnect or a change of database. */
static void update_titles(tab *t) {
    int i = tab_index(t);
    if (i < 0) return;
    char title[TAB_TITLE_MAX + 1];
    tab_title_of(t, title, sizeof title);
    sqlc_tabbar_set_title(app.tabbar, i, title);
    if (t == cur_tab()) update_window_title();
}

/* Show the selected tab's page, hide the others (a hidden control takes no
 * space in its box), and follow it in the window title. */
static void show_selected_page(void) {
    int sel = sqlc_tabbar_selected(app.tabbar);
    for (size_t i = 0; i < app.ntabs; i++)
        if ((int)i != sel) uiControlHide(app.list[i]->page);
    if (sel >= 0 && (size_t)sel < app.ntabs) uiControlShow(app.list[sel]->page);
    update_window_title();
}

static tab *new_tab(void);
static void close_tab(tab *t);

/* The tab strip's clicks: select, x or middle click, "+". */
static void on_tabbar_select(uiControl *bar, int index, void *data) {
    (void)bar; (void)index; (void)data;
    show_selected_page();
}

static void on_tabbar_close(uiControl *bar, int index, void *data) {
    (void)bar; (void)data;
    if (index >= 0 && (size_t)index < app.ntabs) close_tab(app.list[index]);
}

static void on_tabbar_new(uiControl *bar, void *data) {
    (void)bar; (void)data;
    new_tab();
}

/* What a job on a tab's connection is, for the confirmations. */
static const char *job_name(int kind) {
    switch (kind) {
    case JOB_CONNECT:   return "connecting";
    case JOB_TABLES:    return "loading the table list";
    case JOB_AI:        return "Ask Claude";
    case JOB_ROWS:      return "saving rows";
    case JOB_STRUCTURE: return "loading a structure";
    case JOB_ALTER:     return "rename, truncate or drop";
    default:            return "query";
    }
}

/* "export of <what>" (what: the table or database) into buf. */
static void export_name(const tab *t, char *buf, size_t size) {
    const job *j = t->export_job;
    snprintf(buf, size, "export of %s", j && j->table ? j->table : "?");
}

/* Cancel t's export, if one runs (it ends on its own and says so). */
static void cancel_export_of(tab *t) {
    if (!t->export_job) return;
    atomic_store(&t->export_job->cancel, 1);
    if (!t->closed) uiControlDisable(uiControl(t->cancel_export_btn));
}

/* Quit (⌘Q) or the window's close button: returns 1 to quit now, 0 not yet.
 * With jobs running in any tab it asks first and lists them. Running
 * exports are cancelled and the quit waits for them (quit_if_done, from
 * job_done), so their temporary files are removed; a job on a tab's
 * connection can't be interrupted and doesn't hold it up. */
static int may_quit(void) {
    if (app.quitting) return 0; /* already waiting for the exports */
    typedef struct { char title[TAB_TITLE_MAX + 1]; char exp[160]; } names;
    names *nm = calloc(app.ntabs ? app.ntabs : 1, sizeof *nm);
    tab_job_info *items = calloc(app.ntabs ? 2 * app.ntabs : 1, sizeof *items);
    size_t n = 0;
    int exporting = 0;
    for (size_t i = 0; i < app.ntabs; i++) {
        tab *t = app.list[i];
        if (!t->busy && !t->export_job) continue;
        exporting |= t->export_job != NULL;
        if (!nm || !items) { n++; continue; } /* out of memory: count only */
        tab_title_of(t, nm[i].title, sizeof nm[i].title);
        if (t->busy) items[n++] = (tab_job_info){ nm[i].title, job_name(t->job_kind) };
        if (t->export_job) {
            export_name(t, nm[i].exp, sizeof nm[i].exp);
            items[n++] = (tab_job_info){ nm[i].title, nm[i].exp };
        }
    }
    int yes = 1;
    if (n) {
        char list[1200] = "Jobs are running.", msg[1600];
        if (nm && items) running_jobs_text(list, sizeof list, items, n);
        snprintf(msg, sizeof msg, "Still running:\n\n%s\n\n%s", list,
                 exporting ? "Quitting cancels the exports (their partial files are deleted) "
                             "and stops everything else."
                           : "Quitting stops them.");
        yes = confirm_danger("Quit sqlc?", msg, "Quit", 0);
    }
    free(items);
    free(nm);
    if (!yes) return 0;
    /* exports may have ended while the alert was open; closed tabs' exports
     * were cancelled when they closed, but still count */
    for (size_t i = 0; i < app.ntabs; i++) cancel_export_of(app.list[i]);
    if (app.exports == 0) return 1;
    app.quitting = 1;
    for (size_t i = 0; i < app.ntabs; i++)
        if (app.list[i]->export_job) set_status(app.list[i], "Cancelling the export before quitting...");
    return 0;
}

static int on_closing(uiWindow *w, void *data) {
    (void)w; (void)data;
    if (!may_quit()) return 0;
    app.gone = 1; /* libui destroys the window when we return 1 */
    uiQuit();
    return 1;
}

static int on_should_quit(void *data) {
    (void)data;
    if (!may_quit()) return 0;
    app.gone = 1;
    uiControlDestroy(uiControl(app.win));
    return 1;
}

/* ---------- layout ---------- */

/* The libui menus (File, AI); New Tab / Close Tab, Edit and Query are added
 * by sqlc_install_menus once the window exists. Menu items act on the tab
 * in front. */
static void build_menus(void) {
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
}

/* The window: the tab strip on top, then the box holding the tabs' pages. */
static void build_window(void) {
    app.win = uiNewWindow("sqlc", 1100, 720, 1);
    uiWindowSetMargined(app.win, 1);
    uiWindowOnClosing(app.win, on_closing, NULL);
    uiBox *root = uiNewVerticalBox();
    sqlc_tabbar_callbacks cb = {
        .on_select = on_tabbar_select, .on_close = on_tabbar_close, .on_new = on_tabbar_new,
    };
    app.tabbar = sqlc_new_tabbar(&cb);
    uiBoxAppend(root, app.tabbar, 0);
    app.pages = uiNewVerticalBox();
    uiBoxAppend(root, uiControl(app.pages), 1);
    uiWindowSetChild(app.win, uiControl(root));
}

/* One tab's page (t->page): the welcome screen, the full layout and the
 * status row. Every callback gets t as its data. */
static void build_tab_page(tab *t) {
    uiBox *root = uiNewVerticalBox();
    uiBoxSetPadded(root, 1);
    t->page = uiControl(root);

    /* ----- welcome screen: saved connections | new connection form ----- */
    t->saved_box = uiNewVerticalBox(); /* fill_saved_list puts the list in it */

    t->profile_name = uiNewEntry();
    t->host = uiNewEntry();  uiEntrySetText(t->host, "localhost");
    t->port = uiNewEntry();  uiEntrySetText(t->port, "3306");
    t->user = uiNewEntry();
    t->pass = uiNewPasswordEntry();
    t->db   = uiNewEntry();
    const char *login = getenv("USER");
    if (login) uiEntrySetText(t->user, login);
    t->remember_pw = uiNewCheckbox("Remember password");
    uiCheckboxSetChecked(t->remember_pw, 1);
    /* connection options, set before connecting (saved with profiles) */
    t->hide_views = uiNewCheckbox("Hide views");
    uiCheckboxOnToggled(t->hide_views, on_hide_views, t);
    t->no_rehash = uiNewCheckbox("No auto-rehash (-A)");
    t->tables_once = uiNewCheckbox("Tables only, load once");
    uiCheckboxOnToggled(t->tables_once, on_tables_once, t);

    uiForm *form = uiNewForm();
    uiFormSetPadded(form, 1);
    uiFormAppend(form, "Name", uiControl(t->profile_name), 0);
    uiFormAppend(form, "Host", uiControl(t->host), 0);
    uiFormAppend(form, "Port", uiControl(t->port), 0);
    uiFormAppend(form, "User", uiControl(t->user), 0);
    uiFormAppend(form, "Password", uiControl(t->pass), 0);
    uiFormAppend(form, "Database", uiControl(t->db), 0);
    uiFormAppend(form, "", uiControl(t->remember_pw), 0);
    uiFormAppend(form, "", uiControl(t->hide_views), 0);
    uiFormAppend(form, "", uiControl(t->no_rehash), 0);
    uiFormAppend(form, "", uiControl(t->tables_once), 0);

    uiBox *form_buttons = uiNewHorizontalBox();
    uiBoxSetPadded(form_buttons, 1);
    uiButton *save_btn = uiNewButton("Save");
    uiButtonOnClicked(save_btn, on_save_profile, t);
    uiButton *delete_btn = uiNewButton("Delete");
    uiButtonOnClicked(delete_btn, on_delete_profile, t);
    t->connect_btn = uiNewButton("Connect");
    uiButtonOnClicked(t->connect_btn, on_connect, t);
    uiBoxAppend(form_buttons, uiControl(save_btn), 0);
    uiBoxAppend(form_buttons, uiControl(delete_btn), 0);
    uiBoxAppend(form_buttons, uiControl(uiNewHorizontalBox()), 1); /* pushes Connect right */
    uiBoxAppend(form_buttons, uiControl(t->connect_btn), 0);

    /* the form in the middle: empty stretchy boxes around it */
    uiBox *form_col = uiNewVerticalBox();
    uiBoxSetPadded(form_col, 1);
    uiBoxAppend(form_col, uiControl(uiNewVerticalBox()), 1);
    uiBoxAppend(form_col, uiControl(uiNewLabel("Connect to a MySQL or MariaDB server")), 0);
    uiBoxAppend(form_col, uiControl(form), 0);
    uiBoxAppend(form_col, uiControl(form_buttons), 0);
    /* spinner + tip while connecting; takes its space all the time, so the
     * form doesn't move when it starts */
    t->loading = sqlc_new_loading();
    uiBoxAppend(form_col, t->loading, 0);
    uiBoxAppend(form_col, uiControl(uiNewVerticalBox()), 1);
    t->form_col = uiControl(form_col);
    uiBox *center = uiNewHorizontalBox();
    uiBoxAppend(center, uiControl(uiNewHorizontalBox()), 1);
    uiBoxAppend(center, uiControl(form_col), 1);
    uiBoxAppend(center, uiControl(uiNewHorizontalBox()), 1);

    t->welcome = sqlc_new_sidebar_split(uiControl(t->saved_box), uiControl(center),
                                         240, 160, "WelcomeListWidth");
    uiBoxAppend(root, t->welcome, 1);

    /* ----- full layout, once connected ----- */
    uiBox *workspace = uiNewVerticalBox();
    uiBoxSetPadded(workspace, 1);

    /* connected bar: where we are, Hide views (it filters the live sidebar),
     * Disconnect */
    uiBox *bar = uiNewHorizontalBox();
    uiBoxSetPadded(bar, 1);
    t->conn_label = uiNewLabel("");
    t->hide_views_bar = uiNewCheckbox("Hide views");
    uiCheckboxOnToggled(t->hide_views_bar, on_hide_views_bar, t);
    t->disconnect_btn = uiNewButton("Disconnect");
    uiButtonOnClicked(t->disconnect_btn, on_disconnect, t);
    sqlc_button_set_small(t->disconnect_btn);
    uiBoxAppend(bar, uiControl(t->conn_label), 1);
    uiBoxAppend(bar, uiControl(t->hide_views_bar), 0);
    uiBoxAppend(bar, uiControl(t->disconnect_btn), 0);
    uiBoxAppend(workspace, uiControl(bar), 0);
    uiBoxAppend(workspace, uiControl(uiNewHorizontalSeparator()), 0);

    /* main area: sidebar | editor + results, with a draggable divider */
    t->side_box = uiNewVerticalBox();
    uiBoxSetPadded(t->side_box, 1);
    t->load_tables_btn = uiNewButton("Load tables");
    uiButtonOnClicked(t->load_tables_btn, on_load_tables, t);
    uiBoxAppend(t->side_box, uiControl(t->load_tables_btn), 0);
    t->export_btn = uiNewButton("Export\xE2\x80\xA6");
    uiButtonOnClicked(t->export_btn, on_export, t);
    uiBoxAppend(t->side_box, uiControl(t->export_btn), 0);
    t->side_search = uiNewSearchEntry();
    uiEntryOnChanged(t->side_search, on_side_search, t);
    uiBoxAppend(t->side_box, uiControl(t->side_search), 0);
    /* spinner + tip while the list loads; before the list, which
     * rebuild_sidebar_keeping replaces as the box's last child */
    t->side_loading = sqlc_new_loading();
    uiControlHide(t->side_loading);
    uiBoxAppend(t->side_box, t->side_loading, 0);

    /* right side: a slim bar with the Normal | AI | Structure switch at its
     * right edge, above one page at a time */
    uiBox *main_box = uiNewVerticalBox();
    uiBoxSetPadded(main_box, 1);
    uiBox *switch_row = uiNewHorizontalBox();
    uiBoxSetPadded(switch_row, 1);
    static const char *const modes[] = { "Normal", "AI", "Structure" }; /* MODE_* order */
    t->mode_switch = sqlc_new_segmented(modes, 3);
    sqlc_segmented_on_changed(t->mode_switch, on_mode_changed, t);
    uiBoxAppend(switch_row, uiControl(uiNewHorizontalBox()), 1); /* pushes it right */
    uiBoxAppend(switch_row, t->mode_switch, 0);
    uiBoxAppend(main_box, uiControl(switch_row), 0);

    t->split = sqlc_new_sidebar_split(uiControl(t->side_box), uiControl(main_box),
                                       360, 120, NULL); /* always opens at 360 */
    uiBoxAppend(workspace, t->split, 1);
    t->workspace = uiControl(workspace);
    uiBoxAppend(root, t->workspace, 1); /* shown until main() has laid it out */

    /* Normal / AI page: the query pane above, results below, with a draggable
     * divider. The query pane has two views in the same space, one shown at
     * a time: Normal (SQL editor + run row) and AI (Ask Claude). */
    uiBox *editor_pane = uiNewVerticalBox();
    uiBoxSetPadded(editor_pane, 1);

    /* Normal view: the filter bar, the SQL editor and the run row */
    uiBox *normal = uiNewVerticalBox();
    uiBoxSetPadded(normal, 1);

    /* filter bar for the browsed table (update_browse_bars enables it) */
    uiBox *filter = uiNewHorizontalBox();
    uiBoxSetPadded(filter, 1);
    uiBoxAppend(filter, uiControl(uiNewLabel("Filter")), 0);
    t->filter_col = uiNewCombobox(); /* filled by a browse, see fill_filter_columns */
    uiBoxAppend(filter, uiControl(t->filter_col), 0);
    t->filter_op = uiNewCombobox();
    for (int op = 0; op < BROWSE_NOPS; op++) uiComboboxAppend(t->filter_op, browse_op_label((browse_op)op));
    uiComboboxSetSelected(t->filter_op, BROWSE_EQ);
    uiComboboxOnSelected(t->filter_op, on_filter_op, t);
    uiBoxAppend(filter, uiControl(t->filter_op), 0);
    t->filter_value = uiNewEntry();
    sqlc_entry_on_return(t->filter_value, on_filter_return, t);
    uiBoxAppend(filter, uiControl(t->filter_value), 1); /* only the value stretches */
    t->filter_apply = uiNewButton("Apply");
    uiButtonOnClicked(t->filter_apply, on_filter_apply, t);
    uiBoxAppend(filter, uiControl(t->filter_apply), 0);
    t->filter_reset = uiNewButton("Reset");
    uiButtonOnClicked(t->filter_reset, on_filter_reset, t);
    uiBoxAppend(filter, uiControl(t->filter_reset), 0);
    uiBoxAppend(normal, uiControl(filter), 0);

    t->editor = uiNewNonWrappingMultilineEntry();
    uiMultilineEntrySetText(t->editor, "SHOW TABLES;");
    uiBoxAppend(normal, uiControl(t->editor), 1);

    uiBox *run_row = uiNewHorizontalBox();
    uiBoxSetPadded(run_row, 1);
    t->run_btn = uiNewButton("Run (\xE2\x8C\x98\xE2\x86\xA9)"); /* "Run (⌘↩)" */
    uiButtonOnClicked(t->run_btn, on_run, t);
    uiBoxAppend(run_row, uiControl(t->run_btn), 0);
    uiBoxAppend(run_row, uiControl(uiNewLabel("Double-click a table to browse it. "
                                              "Drag the line below to resize.")), 0);
    uiBoxAppend(normal, uiControl(run_row), 0);
    t->normal_view = uiControl(normal);
    uiBoxAppend(editor_pane, t->normal_view, 1);

    /* AI view: describe it, get SQL (with an explanation) in the editor */
    uiBox *ai_box = uiNewVerticalBox();
    uiBoxSetPadded(ai_box, 1);
    uiBoxAppend(ai_box, uiControl(uiNewLabel("Describe the query you need (e.g. top 10 customers "
                                             "by total order value last month)")), 0);
    t->ai_task = uiNewMultilineEntry();
    uiBoxAppend(ai_box, uiControl(t->ai_task), 1);
    uiBox *ai_row = uiNewHorizontalBox();
    uiBoxSetPadded(ai_row, 1);
    t->ai_btn = uiNewButton("Ask Claude (\xE2\x8C\x98\xE2\x86\xA9)"); /* "Ask Claude (⌘↩)" */
    uiButtonOnClicked(t->ai_btn, on_ask_claude, t);
    uiBoxAppend(ai_row, uiControl(t->ai_btn), 0);
    uiBoxAppend(ai_row, uiControl(uiNewLabel("The SQL appears in the editor (Normal view); "
                                             "nothing runs until you press Run.")), 0);
    uiBoxAppend(ai_box, uiControl(ai_row), 0);
    t->ai_view = uiControl(ai_box);
    uiControlHide(t->ai_view);
    uiBoxAppend(editor_pane, t->ai_view, 1);

    /* Half and half to start (0 = half); 100 points each at least, so a
     * big result grid can't squeeze the editor away, or the other way round. */
    t->results_box = uiNewVerticalBox(); /* the pane stays, empty, until a query runs */
    t->query_split = sqlc_new_stacked_split(uiControl(editor_pane), uiControl(t->results_box),
                                             0, 100, 100, "QueryEditorHeight");
    uiBoxAppend(main_box, t->query_split, 1);

    /* Structure page */
    uiBox *structure = uiNewVerticalBox();
    uiBoxSetPadded(structure, 1);
    uiBox *st_row = uiNewHorizontalBox();
    uiBoxSetPadded(st_row, 1);
    t->st.title = uiNewLabel("");
    t->st.refresh_btn = uiNewButton("Refresh");
    uiButtonOnClicked(t->st.refresh_btn, on_structure_refresh, t);
    uiBoxAppend(st_row, uiControl(t->st.title), 1);
    uiBoxAppend(st_row, uiControl(t->st.refresh_btn), 0);
    uiBoxAppend(structure, uiControl(st_row), 0);

    uiBox *cols_pane = uiNewVerticalBox();
    uiBoxSetPadded(cols_pane, 1);
    uiBoxAppend(cols_pane, uiControl(uiNewLabel("Columns")), 0);
    t->st.cols_box = uiNewVerticalBox();
    uiBoxAppend(cols_pane, uiControl(t->st.cols_box), 1);
    uiBox *idx_pane = uiNewVerticalBox();
    uiBoxSetPadded(idx_pane, 1);
    uiBoxAppend(idx_pane, uiControl(uiNewLabel("Indexes")), 0);
    t->st.idx_box = uiNewVerticalBox();
    uiBoxAppend(idx_pane, uiControl(t->st.idx_box), 1);
    uiBoxAppend(structure, sqlc_new_stacked_split(uiControl(cols_pane), uiControl(idx_pane),
                                                  280, 80, 80, "StructureColumnsHeight"), 1);
    t->structure_view = uiControl(structure);
    uiControlHide(t->structure_view);
    uiBoxAppend(main_box, t->structure_view, 1);
    t->mode = MODE_NORMAL;

    /* status line; while an export runs, a progress bar and Cancel next to it */
    uiBox *status_row = uiNewHorizontalBox();
    uiBoxSetPadded(status_row, 1);
    t->status = uiNewLabel("Not connected.");
    uiBoxAppend(status_row, uiControl(t->status), 1);
    t->export_bar = uiNewProgressBar();
    sqlc_control_set_width(uiControl(t->export_bar), 300);
    uiControlHide(uiControl(t->export_bar));
    uiBoxAppend(status_row, uiControl(t->export_bar), 0);
    t->cancel_export_btn = uiNewButton("Cancel");
    uiButtonOnClicked(t->cancel_export_btn, on_cancel_export, t);
    sqlc_button_set_small(t->cancel_export_btn);
    uiControlHide(uiControl(t->cancel_export_btn));
    uiBoxAppend(status_row, uiControl(t->cancel_export_btn), 0);
    /* only after the server closed the connection, see connection_lost */
    t->reconnect_btn = uiNewButton("Reconnect");
    uiButtonOnClicked(t->reconnect_btn, on_reconnect, t);
    sqlc_button_set_small(t->reconnect_btn);
    uiControlHide(uiControl(t->reconnect_btn));
    uiBoxAppend(status_row, uiControl(t->reconnect_btn), 0);
    uiBoxAppend(root, uiControl(status_row), 0);

    t->edit.new_row = -1;
    update_browse_bars(t); /* disabled: nothing is browsed yet */
    rebuild_sidebar(t);
}

/* A new tab with the welcome screen, appended and brought to front.
 * Returns it, or NULL if out of memory. */
static tab *new_tab(void) {
    tab *t = calloc(1, sizeof *t);
    tab **list = realloc(app.list, (app.ntabs + 1) * sizeof *list);
    if (list) app.list = list;
    if (!t || !list) { free(t); return NULL; }
    build_tab_page(t);
    app.list[app.ntabs++] = t;
    int i = (int)app.ntabs - 1;
    uiBoxAppend(app.pages, t->page, 1);
    sqlc_tabbar_append(app.tabbar, "New connection");
    sqlc_tabbar_set_selected(app.tabbar, i); /* doesn't call on_tabbar_select */
    show_selected_page(); /* the new page only */
    fill_saved_list(t, app.profiles.last);
    apply_selected_profile(t);
    /* Lay out the full layout once before it is drawn, so its splits place
     * their dividers (restoring the saved sizes), then switch to the
     * welcome screen. Its page must be the one shown for that. */
    uiControlHide(t->welcome);
    sqlc_window_layout_now(app.win, 0);
    show_screen(t); /* also sets the titles */
    return t;
}

/* Close tab t: asks first if a job runs in it or a new row isn't saved.
 * Its export is cancelled; a job on its connection finishes on its own,
 * and t is freed once the last one is back (tab_release). The last tab is
 * replaced by a fresh welcome tab, never an empty window. */
static void close_tab(tab *t) {
    if (!t || tab_index(t) < 0) return;
    if (t->busy || t->export_job || t->edit.new_row >= 0) {
        char what[300] = "", exp[160], msg[700];
        if (t->busy) snprintf(what, sizeof what, "%s", job_name(t->job_kind));
        if (t->export_job) {
            size_t l = strlen(what);
            export_name(t, exp, sizeof exp);
            snprintf(what + l, sizeof what - l, "%s%s (cancelled, its partial file is deleted)",
                     l ? ", " : "", exp);
        }
        snprintf(msg, sizeof msg, "%s%s%s%s", *what ? "Still running in this tab: " : "", what,
                 *what ? ". " : "",
                 t->edit.new_row >= 0 ? "The new row isn't saved and will be discarded." : "");
        if (!confirm_danger("Close this tab?", msg, "Close Tab", 0)) return;
    }
    int i = tab_index(t);
    if (i < 0) return;
    cancel_export_of(t);
    if (app.ntabs == 1 && !new_tab()) return; /* keep the only tab if out of memory */

    clear_results(t); /* detaches row editing before its grid goes */
    uiBoxDelete(app.pages, i); /* pages are in list order */
    uiControlDestroy(t->page);
    sqlc_tabbar_remove(app.tabbar, i);
    t->page = NULL;
    memmove(app.list + i, app.list + i + 1, (app.ntabs - (size_t)i - 1) * sizeof *app.list);
    app.ntabs--;
    t->closed = 1;
    if (t->jobs == 0) tab_free(t);
    else app.closed_tabs++; /* freed by tab_release */
    show_selected_page(); /* the strip selected the tab at its place */
}

/* Query > Run, File > New Tab / Close Tab (macos_menu.m): the front tab. */
static void menu_run(void) { on_run_menu(cur_tab()); }
static void menu_new_tab(void) { new_tab(); }
static void menu_close_tab(void) { close_tab(cur_tab()); }

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

    app.last_tip = SIZE_MAX; /* no tip shown yet */
    int perr = load_profiles();
    build_menus();
    build_window();
    tab *first = new_tab();
    if (!first) {
        fprintf(stderr, "sqlc-gui: out of memory\n");
        return 1;
    }
    if (perr)
        set_status(first, "Could not read saved connections from %s: %s",
                   app.profiles_file, strerror(perr));
    sqlc_menu_callbacks cb = { .run = menu_run, .new_tab = menu_new_tab, .close_tab = menu_close_tab };
    sqlc_install_menus(&cb);
    uiControlShow(uiControl(app.win));
#ifdef SQLC_AUTOTEST
    uiQueueMain(autotest_start, NULL);
#else
    uiTimer(KEEPALIVE_MS, keepalive_tick, NULL);
#endif
    uiMain();

    /* The window (and its grids) is gone. Kept passwords go in any case (a
     * running job has its own copy); a tab is freed only if no job could
     * still be using it (quit doesn't wait for a query), and the rest only
     * if nothing runs at all. */
    int running = app.exports > 0 || app.closed_tabs > 0;
    for (size_t i = 0; i < app.ntabs; i++) {
        tab *t = app.list[i];
        forget_login(t);
        if (t->jobs == 0) tab_free(t);
        else running = 1;
    }
    free(app.list);
    app.list = NULL;
    app.ntabs = 0;
    if (!running) {
        profiles_free(&app.profiles);
        free(app.profiles_file);
        uiUninit();
    }
    mysql_library_end();
    curl_global_cleanup();
    return 0;
}
