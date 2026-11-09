# sqlc

A small MySQL / MariaDB client written in C, built on
[MariaDB Connector/C](https://mariadb.com/kb/en/mariadb-connector-c/).
It comes as a command-line client (`sqlc`) and a simple native macOS GUI
(`sqlc-gui`, built with [libui-ng](https://github.com/libui-ng/libui-ng)).
It works with both MariaDB and MySQL servers and runs natively on Apple Silicon.

## Features

### GUI (`sqlc-gui`)

- Connection bar (host, port, user, password, database) with options you can
  set before connecting: **Hide views**, **No auto-rehash (-A)** and
  **Tables only, load once**
- **Saved connections**: save the connection fields under a name and pick it
  from a dropdown next time (remembered passwords are saved with them)
- SQL editor with **Run** (button or ⌘↩); several statements at once
- Editor above, results below, split in half; drag the divider between them
  to resize (neither side can be dragged away, and the position is remembered)
- **Ask Claude**: describe the query you need in plain language and Claude
  writes the SQL into the editor, with an explanation as `--` comments, for you
  to review and run (needs an Anthropic API key)
- Results grid, with one tab per result set, plus "Query OK" and error pages
- **Normal** / **AI** / **Structure** switch at the top right of the main
  area: Normal is the SQL editor above the results, AI is Ask Claude above the
  same results, and Structure shows the columns (type, null, key, default,
  extra, collation, comment) above the indexes of the table selected in the
  sidebar, with its engine, row estimate and collation; switching keeps the
  query, its results and the selection
- Sidebar listing the tables in the current database, with a **Load tables** /
  **Refresh tables** button; drag its right edge to resize it (the width is remembered).
  Shift-click selects a range of tables, ⌘-click adds or removes one (on a Mac,
  Ctrl-click is a right-click)
- Double-click a table to browse it (`SELECT * ... LIMIT 1000`), then **edit its
  rows in place**: double-click a cell to change it (with a calendar for dates),
  **+** adds a row, **−** deletes the selected rows, **Reload**, **Delete All Rows…**
- Right-click a table for **Show Structure**, **Copy Table Name**, **Rename**, **Truncate**, **Delete**
  and **Export** (Truncate and Delete always ask first; Cancel is the default)
- **Export…** in the sidebar (or **File › Export…**) exports what is selected:
  one table as SQL, CSV or XML, several tables as one SQL file, or, with
  nothing selected, the whole database
- **Export the whole database** as one SQL file, every table's structure and
  data and then the views (also **File › Export Database…**, whatever is selected)
- Queries run on a background thread, so the window never freezes
- Shows the first 10,000 rows per result set

### CLI (`sqlc`)

- Interactive prompt with history and multi-line statements (end with `;`)
- `mysql`-style table output (numbers right-aligned, `NULL` shown)
- Several statements and result sets in one go
- Tab completion of SQL keywords, databases, tables and columns, **with views excluded**
- `--no-views`: hide views from `SHOW TABLES` output
- Familiar `mysql` flags: `-h -P -u -p -D -S -e -A`

## Requirements

- macOS (tested on Apple Silicon) with the Xcode Command Line Tools
- MariaDB Connector/C, which provides `mariadb_config`:

  ```sh
  brew install mariadb-connector-c   # or: brew install mariadb
  ```

It uses `libedit` and AppKit, which ship with macOS. libui-ng is included as a
git submodule and compiled by the Makefile, so meson/ninja aren't needed.

## Build

```sh
git clone --recursive https://github.com/nikoladubica/sql-client-c.git
cd sql-client-c
make            # builds ./sqlc and ./sqlc-gui
make sqlc       # CLI only
make gui        # GUI only
make debug      # both, with AddressSanitizer + UBSan
make compile_flags.txt   # optional: include paths for clangd / your editor
```

If you cloned without `--recursive`, the Makefile fetches the submodule for you
(or run `git submodule update --init`).

## GUI

```sh
./sqlc-gui
```

Fill in the connection fields, press **Connect**, type SQL and press **Run** (⌘↩).

A **Normal | AI | Structure** switch at the right end of a slim bar above the
main area (right of the sidebar) picks what the area shows. In **Normal** and
**AI**, the query pane takes the top half and the results the bottom half; the results pane is there, empty, before the first query. Drag
the divider between them to give either side more room. Each side keeps at
least 100 points, so a large result can't push the editor out of view. The
editor's height is saved and restored on the next launch, clamped so that
both panes still fit; when the window is resized, the editor keeps its height
and the results take the difference.

**Normal** shows the SQL editor with the **Run** button in the query pane, and
**AI** shows [Ask Claude](#ask-claude-ai-query-assistant) in its place; the
results pane below doesn't change when you switch between the two.
Double-clicking a table in the sidebar switches to **Normal**, since it
replaces the editor's text, and running a query from **Structure** (a table
browse, a reload) switches to **Normal** to show its results.

**Structure** replaces the query pane and the results with the structure of
the table selected in the sidebar: its columns above, its indexes below, with
a draggable divider (its position is remembered) and a **Refresh** button.
The structure is loaded when you switch to it and whenever the sidebar
selection changes while it's shown; with several tables selected it asks you
to select one. **Show Structure** in a table's right-click menu switches to it.

The sidebar selects several tables with Shift-click (a range) and ⌘-click
(add or remove one); Ctrl-click is a right-click on a Mac. The selection is
kept when the list is refreshed. Right-clicking a table selects just that
table and its menu acts on it, and double-clicking browses the table clicked. ⌘↩ runs nothing while Structure is shown
(the editor is hidden); switch to **Normal** to run SQL.

Connection options (all are saved with a connection):

- **Hide views**: views are left out of the sidebar (they're filtered in the
  query to the server) and out of `SHOW TABLES` results, like the CLI's `--no-views`.
- **No auto-rehash (-A)**: like the CLI's `-A`, the table list isn't loaded
  on connect or after each query, which speeds up connecting to large schemas.
  Press **Load tables** in the sidebar to load it when you need it.
- **Tables only, load once**: the sidebar lists base tables (no views) when you
  connect, and after that behaves like `-A`: it isn't reloaded after each
  query, so press **Refresh tables** after creating, dropping or `USE`. It
  turns on **Hide views** and takes the place of **-A** while it's checked.

To keep a connection, type a **Name** and press **Save**. Saved connections
appear in the **Saved connections** dropdown; choosing one fills in the fields,
and the last one you connected with is preselected on launch. **Delete** removes
the selected one.

Connections are stored in `~/Library/Application Support/sqlc/connections`
(mode `0600`; override the folder with `SQLC_CONFIG_DIR`). With
**Remember password** ticked, the password is saved in that file too, in plain
text: only your user account can read it, but anyone with access to your
account (or a backup of that folder) can. Leave it unticked for connections
whose password you'd rather type each time.

### Ask Claude (AI query assistant)

Switch the main area to **AI** (the **Normal | AI | Structure** switch, or
**AI › Ask Claude**), describe what you need (for example *top 10 customers by
total order value last month*) and press **Ask Claude** (or ⌘↩, which asks
Claude instead of running while AI is shown). Claude's answer replaces
the editor's text, and the switch goes back to **Normal** to show
it: your request and Claude's explanation as `--` comments, then the SQL.
**Nothing runs until you press Run**, so read it first and edit it if needed.
Your request stays in the AI pane, so you can refine it and ask again. If the
request fails, the switch stays on **AI**.

- **API key**: the first time, sqlc asks for an
  [Anthropic API key](https://console.anthropic.com/) and saves it in
  `~/Library/Application Support/sqlc/claude-api-key` (mode `0600`). Change or
  remove it under **AI › Set Claude API Key…** / **AI › Remove Claude API Key**.
  If no key is saved, `ANTHROPIC_API_KEY` from the environment is used.
- **What is sent**: your request, the server version, the current database's
  name, and its schema: table and view names, column names and types,
  NULL-ability, keys and foreign keys. **Row data is never sent.** Very large
  schemas are shortened to about 200 KB.
- **Safety**: if the SQL Claude wrote contains `DROP`, `TRUNCATE`, `DELETE`,
  `ALTER … DROP` or an `UPDATE` without `WHERE`, **Run** asks first (Cancel is
  the default). Claude is also told to write read-only queries unless you ask
  for a change.
- It uses `claude-opus-5-5` through the Messages API, with structured output
  (`{explanation, sql}`). If a safety classifier declines a request, the API
  retries it on a recommended fallback model (`fallbacks: "default"`); the status
  bar shows which model answered.
- Errors (no or invalid key, network, rate limits, overload) are shown in a
  dialog and the status bar.

### Editing rows

Double-click a table in the sidebar to browse it; its grid can then be edited,
with a toolbar below it.

- **Edit a cell**: double-click it, type, and press **Return** to save
  (`UPDATE`), **Tab** / **Shift-Tab** to save and move to the next / previous
  column, or **Escape** to cancel. Clicking elsewhere also saves.
- The editor follows the column type: numbers are checked before saving,
  dates (`DATE`) and date-times (`DATETIME`, `TIMESTAMP`) show a calendar under
  the field (the clock too for date-times), and you can still type the value
  (`2024-1-5` becomes `2024-01-05`, `2024-01-05 9:30` becomes
  `2024-01-05 09:30:00`). For non-text columns, an empty value means `NULL`.
- Right-click a cell for **Set to NULL**, **Copy Value** and **Delete Row…**.
- **+** adds a row under the last one: type into its cells (empty ones show
  `(default)` and get the column's default) and press **Return** to `INSERT` it.
  The row is then read back, so auto-increment ids and defaults show up.
- **−** deletes the selected rows (⌘-click or ⇧-click selects several) after
  asking; they're deleted in one transaction.
- **Reload** reads the table again; **Delete All Rows…** truncates it (asks
  first).
- If the server refuses a change (wrong type, constraint, duplicate key), the
  error is shown and the editor reopens with what you typed, so you can fix it.
- Rows are found by their primary key. Without one, they're matched on all
  their values (except float and binary columns), one row at a time.
  Binary and generated columns can't be edited, and views are read-only.

### Export

**Export…** in the sidebar (or **File › Export…**) exports the sidebar selection:

- **Nothing selected**: the whole database, as **File › Export Database…**
  (see [Whole database](#whole-database)).
- **One table or view**: a format dialog (SQL, CSV or XML, below).
- **Several** (Shift-click, ⌘-click): one SQL file with just those, see
  [Selected tables](#selected-tables). There is no CSV or XML form for several
  tables; export them one at a time for that.

For one table, pick a format and options, then where to save (defaults to `<table>.<ext>`).
Rows are streamed to the file on a background thread, so big tables don't
freeze the window; the file only appears once the export has finished.

- **SQL**: `DROP TABLE IF EXISTS` + `SHOW CREATE TABLE`, then multi-row `INSERT`s
  (structure only, data only, or both). Binary columns are written as hex,
  `NULL` as `NULL`, and generated columns are left out. Import it with
  `mysql db < table.sql`. A view exports as its `CREATE VIEW`.
- **CSV**: RFC 4180 (comma separated, CRLF, fields with `,` `"` or line breaks
  quoted), header row, UTF-8 with a BOM so Excel detects it (optional).
  `NULL` is an empty field (or `\N`), an empty string is `""`; binary as `0x…` hex.
- **XML**: UTF-8, `<table name="…"><row><field name="…">…</field></row></table>`;
  `NULL` is `<field name="…" xsi:nil="true"/>`, binary is `encoding="hex"`.

#### Whole database

**File › Export Database…** exports the current database (the one
`SELECT DATABASE()` names: the **Database** field, or the last `USE`) to one
SQL file, defaulting to `<database>.sql`. Choose structure, `DROP … IF EXISTS`
and data as for a table; there is no CSV or XML form.

- The file starts by setting `utf8mb4`, the UTC time zone and
  `FOREIGN_KEY_CHECKS=0`, and ends by restoring them, so tables load in any
  order whatever their foreign keys.
- Every table follows by name, each written exactly like a single-table SQL
  export (`DROP TABLE IF EXISTS`, `CREATE TABLE`, `INSERT`s).
- Views come after all tables (`CREATE VIEW` only, no data), and a view that
  uses another view comes after it.
- Sequences and other objects that aren't tables or views are left out, with
  a `-- Skipped` comment in the file.
- There's no `CREATE DATABASE` or `USE`: load it into any database with
  `mysql db < db.sql`.
- The status bar shows which table is being written and the row count.
  Nothing is changed on the server (`SELECT` and `SHOW` only).

#### Selected tables

With two or more tables or views selected in the sidebar, **Export…** writes
them to one SQL file the same way, defaulting to `<database>-<n>-tables.sql`,
with the same structure / `DROP … IF EXISTS` / data options:

- The same header and footer (`utf8mb4`, UTC, `FOREIGN_KEY_CHECKS=0`, then
  restored); the opening comment says it is a selection of the database.
- The selected tables by name, then the selected views, each after the
  selected views it uses. Tables or views they depend on are not added: select
  them too if the file has to load into an empty database.
- A selected table that no longer exists (dropped or renamed since the list
  was loaded) stops the export before any file is written; refresh the list
  and select again.

## CLI usage

```sh
./sqlc -u root -p                       # prompts for the password
./sqlc -u app -pSECRET -h db.local mydb # connect straight to a database
./sqlc -u root -p -e "SHOW DATABASES;"  # run SQL and exit
./sqlc -u root -p -A mydb               # skip loading names (fast start)
./sqlc -u root -p --no-views mydb       # SHOW TABLES without views
```

| Option | Description |
| --- | --- |
| `-h, --host=HOST` | Server host (default `localhost`) |
| `-P, --port=PORT` | Server port (default 3306) |
| `-u, --user=USER` | User name |
| `-p, --password[=PASS]` | Password; prompts if no value is given (`-pPASS`, no space) |
| `-D, --database=DB` | Default database (also accepted as the last argument) |
| `-S, --socket=PATH` | Unix socket path |
| `-e, --execute=SQL` | Execute statement(s) and exit |
| `-A, --no-auto-rehash` | Don't load names for tab completion |
| `--no-views` | Hide views from `SHOW TABLES` output |
| `-?, --help` | Show help |

### Interactive commands

| Command | Description |
| --- | --- |
| `quit`, `exit`, `\q` | Leave (Ctrl-D works too) |
| `rehash`, `\#` | Reload completion names |
| `views off` / `views on` | Hide / show views in `SHOW TABLES` |

### How views are handled

- **Tab completion** loads names from `information_schema` and skips anything
  whose `TABLE_TYPE` is `VIEW` or `SYSTEM VIEW`, including the views' columns.
  Names reload automatically after `USE db;`.
- **`--no-views`** rewrites a `SHOW [FULL] TABLES ...` statement to
  `SHOW FULL TABLES ...` and drops the view rows. The `Table_type` column is
  only shown if you asked for `FULL` yourself. `LIKE` and `WHERE` clauses keep
  working.

## Project layout

| Path | What |
| --- | --- |
| `src/db.c`, `src/db.h` | Shared core: run SQL, collect results, views filter |
| `src/main.c`, `src/complete.c` | CLI and its tab completion |
| `src/gui.c` | GUI (libui-ng) |
| `src/macos_menu.m` | Native Edit menu and Query › Run (⌘↩), which libui-ng lacks |
| `src/splitview.m` | Resizable panes (sidebar, editor / results, columns / indexes): an `NSSplitView` wrapped as a libui control |
| `src/segmented.m` | The Normal / AI / Structure switch: an `NSSegmentedControl` wrapped as a libui control |
| `src/export.c` | Streaming SQL / CSV / XML writers (no database code) |
| `src/export_db.c` | Export a table, the whole current database or selected tables as SQL, from a connection through those writers |
| `src/export_dialog.m` | Native export options dialogs (table, database or selected tables) and save panel |
| `src/table_menu.m` | Sidebar right-click menu, rename and API-key prompts, confirmations, clipboard |
| `src/row_edit.c` | Row editing without UI: column kinds, value checks, `UPDATE` / `INSERT` / `DELETE` SQL |
| `src/grid_edit.m` | In-place cell editor for the results grid (text field, calendar popover, cell menu) |
| `src/ai.c` | Ask Claude: request body, Claude API call (libcurl), reply parsing, editor text, destructive-statement check |
| `src/ai_db.c` | Current database's schema as text for the prompt |
| `src/json.c` | Small JSON builder and parser used by `ai.c` |
| `third_party/libui-ng` | libui-ng submodule |

## Development

`make test` runs the unit tests for the saved-connections file, the export
writers' escaping/quoting, the database dump's header, comments, view
ordering and table selection, Ask Claude's request/response handling, comment formatting and
destructive-statement check, and the row editor's value checks and generated
SQL (no database or network needed).

`make export-test` exports an edge-case table in every format and checks the
result: the SQL is re-imported into an empty database and compared with
`CHECKSUM TABLE`, the XML is checked with `xmllint`, and the CSV is parsed back
with Python. It creates and drops `test_sqlc_export_src` and `test_sqlc_export_dst`.

`make gui-test` builds the GUI with sanitizers and runs a scripted smoke test
(connect, multi-statement results, views filter, double-click, row cap,
the editor/results divider's minimum heights, the Normal / AI / Structure switch,
table and database export, sidebar multi-select, `-A`, the context-menu actions on a scratch table `ctx_tmp`, and
"Tables only, load once" with a scratch table `once_tmp`, and the Run warning
for destructive SQL from Claude, without calling the API, and row editing on
scratch tables `edit_tmp` and `nopk_tmp`: cell edits, the calendar, invalid
values, Set to NULL, adding, deleting several, Delete All Rows, a table without
a primary key and a read-only view). It
expects a database (`SQLC_TEST_DB`, default `test_sqlc_demo`) with tables
`customers` and `orders` plus a view.

## Limitations

- The `--no-views` filter applies only when `SHOW TABLES` is the sole statement
  sent (not when combined with others, e.g. `SHOW TABLES; SELECT 1;`).
- A `;` at the end of a line inside a quoted string ends the statement early.
- Column widths are counted in bytes, so non-ASCII text may misalign.
- No `\G` vertical output, TLS options or prepared statements yet.
- GUI: only a browsed table's grid (double-click in the sidebar) can be
  edited, not the results of your own queries; it shows the first 1000 rows.
  There's no syntax highlighting, and it always runs the whole editor (not just
  the selection). A running query can't be cancelled yet.
- Database and multi-table export are SQL only and GUI only. Tables are read one after
  another, not in one transaction, so a database that is being written to
  during the export isn't captured as a consistent snapshot. Views keep their
  `DEFINER`, as `SHOW CREATE VIEW` writes it; loading them as another user
  may need the privilege to set that definer. MariaDB system-versioned tables
  export their current rows only, without history.
- Ask Claude is GUI only (no `sqlc` CLI command yet), and the window is busy
  (Run disabled) until Claude answers; a request can't be cancelled.

## License

[MIT](LICENSE)
