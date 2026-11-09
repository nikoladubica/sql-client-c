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
  from a dropdown next time (passwords go in the macOS Keychain)
- SQL editor with **Run** (button or ⌘↩); several statements at once
- Editor above, results below, split in half; drag the divider between them
  to resize (neither side can be dragged away, and the position is remembered)
- **Ask Claude**: describe the query you need in plain language and Claude
  writes the SQL into the editor, with an explanation as `--` comments, for you
  to review and run (needs an Anthropic API key)
- Results grid, with one tab per result set, plus "Query OK" and error pages
- Sidebar listing the tables in the current database, with a **Load tables** /
  **Refresh tables** button; drag its right edge to resize it (the width is remembered)
- Double-click a table to browse it (`SELECT * ... LIMIT 1000`)
- Right-click a table for **Copy Table Name**, **Rename**, **Truncate**, **Delete**
  and **Export** (Truncate and Delete always ask first; Cancel is the default)
- **Export** a table as SQL, CSV or XML (select it, then **Export…** in the
  sidebar or **File › Export Table…**)
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

The query editor (with the **Run** button) takes the top half of the main
area and the results the bottom half; the results pane is there, empty, before
the first query. Drag the divider between them to give either side more room.
Each side keeps at least 100 points, so a large result can't push the editor
out of view, and the position is saved (as a share of the height) and restored
on the next launch.

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
**Remember password** ticked, the password is stored in your login Keychain
(service `sqlc`), never in that file. The first time an item is read, macOS may
ask whether `sqlc-gui` may use it (and again after a rebuild, since the binary
changes).

### Ask Claude (AI query assistant)

Type what you need in the **Ask Claude** field above the editor (for example
*top 10 customers by total order value last month*) and press **Ask Claude**.
Claude's answer replaces the editor's text: your request and Claude's
explanation as `--` comments, then the SQL. **Nothing runs until you press Run**,
so read it first and edit it if needed.

- **API key**: the first time, sqlc asks for an
  [Anthropic API key](https://console.anthropic.com/) and stores it in your login
  Keychain (service `sqlc-ai`). Change or remove it under **AI › Set Claude API
  Key…** / **AI › Remove Claude API Key**. If no key is in the Keychain,
  `ANTHROPIC_API_KEY` from the environment is used.
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

### Export

Select a table in the sidebar and press **Export…** (or **File › Export Table…**),
pick a format and options, then where to save (defaults to `<table>.<ext>`).
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
| `src/splitview.m` | Resizable sidebar and editor/results divider: `NSSplitView`s wrapped as libui controls |
| `src/export.c` | Streaming SQL / CSV / XML writers (no database code) |
| `src/export_db.c` | Export a table from a connection through those writers |
| `src/export_dialog.m` | Native export options dialog and save panel |
| `src/table_menu.m` | Sidebar right-click menu, rename and API-key prompts, confirmations, clipboard |
| `src/ai.c` | Ask Claude: request body, Claude API call (libcurl), reply parsing, editor text, destructive-statement check |
| `src/ai_db.c` | Current database's schema as text for the prompt |
| `src/json.c` | Small JSON builder and parser used by `ai.c` |
| `third_party/libui-ng` | libui-ng submodule |

## Development

`make test` runs the unit tests for the saved-connections file, the export
writers' escaping/quoting, and Ask Claude's request/response handling,
comment formatting and destructive-statement check (no database or network needed).

`make export-test` exports an edge-case table in every format and checks the
result: the SQL is re-imported into an empty database and compared with
`CHECKSUM TABLE`, the XML is checked with `xmllint`, and the CSV is parsed back
with Python. It creates and drops `test_sqlc_export_src` and `test_sqlc_export_dst`.

`make gui-test` builds the GUI with sanitizers and runs a scripted smoke test
(connect, multi-statement results, views filter, double-click, row cap,
the editor/results divider's minimum heights, export, `-A`, the context-menu actions on a scratch table `ctx_tmp`, and
"Tables only, load once" with a scratch table `once_tmp`, and the Run warning
for destructive SQL from Claude, without calling the API). It
expects a database (`SQLC_TEST_DB`, default `test_sqlc_demo`) with tables
`customers` and `orders` plus a view.

## Limitations

- The `--no-views` filter applies only when `SHOW TABLES` is the sole statement
  sent (not when combined with others, e.g. `SHOW TABLES; SELECT 1;`).
- A `;` at the end of a line inside a quoted string ends the statement early.
- Column widths are counted in bytes, so non-ASCII text may misalign.
- No `\G` vertical output, TLS options or prepared statements yet.
- GUI: the grid is read-only, there's no syntax highlighting, and it always
  runs the whole editor (not just the selection). A running query can't be
  cancelled yet.
- Ask Claude is GUI only (no `sqlc` CLI command yet), and the window is busy
  (Run disabled) until Claude answers; a request can't be cancelled.

## License

[MIT](LICENSE)
