# sqlc

A small MySQL / MariaDB client written in C, built on
[MariaDB Connector/C](https://mariadb.com/kb/en/mariadb-connector-c/).
It comes as a command-line client (`sqlc`) and a simple native macOS GUI
(`sqlc-gui`, built with [libui-ng](https://github.com/libui-ng/libui-ng)).
It works with both MariaDB and MySQL servers and runs natively on Apple Silicon.

## Features

### GUI (`sqlc-gui`)

- Connection bar (host, port, user, password, database) with options you can
  set before connecting: **Hide views** and **No auto-rehash (-A)**
- **Saved connections**: save the connection fields under a name and pick it
  from a dropdown next time (passwords go in the macOS Keychain)
- SQL editor with **Run** (button or ⌘↩); several statements at once
- Results grid, with one tab per result set, plus "Query OK" and error pages
- Sidebar listing the tables in the current database, with a **Load tables** /
  **Refresh tables** button; drag its right edge to resize it (the width is remembered)
- Double-click a table to browse it (`SELECT * ... LIMIT 1000`)
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

Connection options (both are saved with a connection):

- **Hide views**: views are left out of the sidebar (they're filtered in the
  query to the server) and out of `SHOW TABLES` results, like the CLI's `--no-views`.
- **No auto-rehash (-A)**: like the CLI's `-A`, the table list isn't loaded
  on connect or after each query, which speeds up connecting to large schemas.
  Press **Load tables** in the sidebar to load it when you need it.

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
| `src/splitview.m` | Resizable sidebar: an `NSSplitView` wrapped as a libui control |
| `third_party/libui-ng` | libui-ng submodule |

## Development

`make test` runs the unit tests for the saved-connections file (no database needed).

`make gui-test` builds the GUI with sanitizers and runs a scripted smoke test
(connect, multi-statement results, views filter, double-click, row cap). It
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

## License

[MIT](LICENSE)
