# sqlc

A small MySQL / MariaDB client written in C, built on
[MariaDB Connector/C](https://mariadb.com/kb/en/mariadb-connector-c/).
It comes as a command-line client (`sqlc`) and a simple native GUI
(`sqlc-gui`, built with [libui-ng](https://github.com/libui-ng/libui-ng)) for
macOS and, as an early version, Windows (see [Windows](#windows)).
It works with both MariaDB and MySQL servers and runs natively on Apple Silicon.

## Features

### GUI (`sqlc-gui`)

- **Welcome screen** before connecting: your saved connections on the left,
  a new-connection form (host, port, user, password, database) in the middle,
  with options you set before connecting: **Hide views**, **No auto-rehash (-A)**
  and **Tables only, load once**. Once connected, the window switches to the
  full layout, with a slim bar on top showing the connection and a **Disconnect** button
- **Tabs** in a strip along the top: **+** (or **File › New Tab**, ⌘T) opens
  another welcome screen in a new tab;
  each tab is a full workspace with its own connection, so tabs can point at
  different servers (or the same one) and a long query in one never blocks
  another. Each tab's **x**, a middle click on it, or **Close Tab** (⌘W)
  disconnects and closes it
- While connecting, the form and the list are disabled and a spinner turns
  under the form, with a random SQL tip (one of 50, never the same one twice in a row);
  **Load tables** / **Refresh tables** show the same spinner and a tip in the sidebar
- **Saved connections**: save the form under a name and pick it from the list
  next time, or double-click it to connect (remembered passwords are saved with them)
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
  **Refresh tables** button and a **search field** that narrows the list to the tables whose
  name contains what you type; drag its right edge to resize it (every launch and new tab starts it at 360 points).
  Shift-click selects a range of tables, ⌘-click adds or removes one (on a Mac,
  Ctrl-click is a right-click)
- Double-click a table to browse it, 1000 rows a page (`SELECT * ... ORDER BY <key>
  LIMIT 1000`) with **<** / **>** to page through it and a **filter bar** above the
  editor (column, operator, value, **Apply**, **Reset**), then **edit its
  rows in place**: double-click a cell to change it (with a calendar for dates),
  **+** adds a row, **−** deletes the selected rows, **Reload**, **Delete All Rows…**;
  your own `SELECT`'s rows from one table can be edited the same way
- Right-click a table for **Show Structure**, **Copy Table Name**, **Rename**, **Truncate**, **Delete**
  and **Export** (Truncate and Delete always ask first; Cancel is the default)
- **Export…** in the sidebar (or **File › Export…**) exports what is selected:
  one table as SQL, CSV or XML, several tables as one SQL file, or, with
  nothing selected, the whole database
- **Export the whole database** as one SQL file, every table's structure and
  data and then the views (also **File › Export Database…**, whatever is selected)
- A **progress bar** with **Cancel** next to the status line while an export runs,
  and a sound and an alert with the time it took when it ends
- Exports run on a **connection of their own**, so you can keep browsing,
  querying and editing while one runs
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

## Windows

`sqlc-gui.exe` is the same GUI on Windows 10 and 11 (64-bit), with everything
in [GUI](#gui). The CLI isn't built for Windows. This is an early version,
not yet tried as widely as the macOS one.

**Download:** each push builds it on GitHub Actions (workflow **Windows GUI**);
the run's `sqlc-gui-windows-x64` artifact is a zip with `sqlc-gui.exe`, the
DLLs it needs and MariaDB's authentication plugins (in `plugin`). Unzip it
anywhere and run `sqlc-gui.exe`; keep the files together.

**Build** in an [MSYS2](https://www.msys2.org) UCRT64 shell:

```sh
pacman -S make mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-pkgconf \
          mingw-w64-ucrt-x86_64-libmariadbclient mingw-w64-ucrt-x86_64-curl
git clone --recursive https://github.com/nikoladubica/sql-client-c.git
cd sql-client-c
make windows    # builds ./sqlc-gui.exe
make win-test   # the unit tests that run on Windows (no database needed)
```

On macOS or Linux, `make win-check` compiles everything for Windows with
MinGW-w64 (`brew install mingw-w64`) without linking, as a quick check that a
change still builds there; `make windows` cross-compiles too, given Windows
builds of MariaDB Connector/C and libcurl (set `WIN_DEPS_CFLAGS` and
`WIN_DEPS_LIBS`).

**What's different from macOS:**

| macOS | Windows |
| --- | --- |
| ⌘↩ runs the editor (or asks Claude) | Ctrl+Enter; also **Query › Run** |
| ⌘T / ⌘W, **File › New Tab / Close Tab** | Ctrl+T / Ctrl+W, the same menu items |
| ⌘-click adds a row or table to the selection | Ctrl-click (Shift-click selects a range on both) |
| Edit menu (Undo, Cut, Copy, Paste, Select All) | the text fields' own Ctrl+Z / X / C / V / A and right-click menu |
| Settings in `~/Library/Application Support/sqlc` | Settings in `%APPDATA%\sqlc` (the same files: `connections`, `claude-api-key`) |
| Divider positions in the user defaults | Divider positions in the registry, `HKEY_CURRENT_USER\Software\sqlc` |
| Spinner while connecting / loading tables | A moving progress bar |
| Export saved message shows `~/...` | It shows the full path |
| Export options in an alert, then the save panel | An **Export** dialog with the same options, then **Save As** (it asks before replacing a file) |
| Calendar popover under a date cell's editor | A month calendar under it; for date and time columns a time field (up / down arrows) below the calendar |

When a long job ends in the background, the taskbar button flashes instead
of the Dock icon bouncing, with the Windows "asterisk" (or "exclamation" when
it failed) sound.

The suggested export file name is the table's (or database's) name with the
characters Windows doesn't allow in file names (`\ / : * ? " < > |`) turned
into `-`; a name Windows reserves, like `con` or `nul`, gets a leading `_`.

## GUI

```sh
./sqlc-gui
```

The window opens on a welcome screen with only two parts: the **Saved
connections** list on the left and a connection form in the middle (Name,
Host, Port, User, Password, Database, the options below and **Save**,
**Delete** and **Connect**). Pick a saved connection or fill in the form,
press **Connect**, then type SQL and press **Run** (⌘↩). While the connection
opens (and the table list loads), the form and the list are disabled, and a
spinner turns under the form with a SQL tip: one of 50 in `src/sql_tips.c`,
picked at random and never the same as last time. On a fast local server it
may only flash by. If the connection fails, an error appears and you stay on
the welcome screen with the form enabled again.

Once you're connected, the window switches to the full layout described
below. A slim bar above it shows the connection
(`Connected to user@host:port - database: db`, updated after `USE`),
the **Hide views** option and **Disconnect**. **Disconnect** (disabled while a
query is running) closes the connection and goes back to the welcome screen.
It clears the results, the table list and the structure, but the editor keeps
its text. An unsaved new row in a browsed table is discarded. If an export is
running, it asks first ("An export is running. Disconnecting cancels it and
deletes the partial file."; Cancel is the default) and cancels the export.
To change **No auto-rehash (-A)** or **Tables only, load once**, disconnect,
change them and connect again.

While a tab is connected and idle, sqlc-gui pings its server once a minute,
so the server doesn't close the connection for inactivity (`wait_timeout`).
If the connection is lost anyway (server restart, killed, network, the Mac
slept), the next ping or query says so at once: the status line reads
`Lost the connection to the server: ERROR 2006 (HY000): Server has gone away
- press Reconnect.`, the bar shows `Connection lost: user@host...` and a
**Reconnect** button appears at the right end of the status line. Nothing
on screen is cleared: the editor's text, the results, the table list and the
mode stay, and running SQL just repeats the hint until you reconnect.
**Reconnect** opens a new connection with the same login, to the database
that was in use (also after `USE`), keeps the sidebar selection and, if a
table was being browsed, reads it again (the same page, with its filter),
leaving the editor's text as it was.
The query that hit the lost connection isn't run again, press **Run** for
that. Session state (variables, temporary tables, an open transaction) is
gone with the old connection. If reconnecting fails, an error is shown and
**Reconnect** stays; **Disconnect** goes back to the welcome screen.

sqlc-gui keeps the login (including the password) in memory while you're
connected, so an export can open its own connection (see [Export](#export)).
The password is zeroed and freed on **Disconnect**, when you close the tab
and when you quit.

A **Normal | AI | Structure** switch at the right end of a slim bar above the
main area (right of the sidebar) picks what the area shows. In **Normal** and
**AI**, the query pane takes the top half and the results the bottom half; the results pane is there, empty, before the first query. Drag
the divider between them to give either side more room. Each side keeps at
least 100 points, so a large result can't push the editor out of view. The
editor's height is saved and restored on the next launch, clamped so that
both panes still fit; when the window is resized, the editor keeps its height
and the results take the difference.

**Normal** shows the SQL editor with the **Run** button in the query pane
(and the [filter bar](#browsing-a-table) above the editor), and
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

While the table list loads (**Load tables**, **Refresh tables**, or turning
**Hide views** on or off), the list is dimmed and a spinner with a SQL tip shows
above it in the sidebar. The tip wraps to as many lines as the sidebar's width
needs. The list reloaded after each query doesn't show it.

The **search field** above the table list filters it as you type: only the
tables (and views) whose name contains the text are listed, upper or lower
case alike (`ord` finds `Orders` and `order_items`); spaces before and after
the text are ignored, and `%` and `_` are plain characters. Clear the field
(or press its **x**) to see every table again. The filter stays on when the
list reloads (after a query, **Refresh tables**, **Hide views**). A selected
table the filter hides is no longer selected, so with none left **Export…**
exports the whole database. **Disconnect** clears the field.

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
appear in the welcome screen's **Saved connections** list, under
**New connection** (which leaves the fields as they are and clears the Name).
Clicking one fills in the form, and the last one you connected with is
preselected on launch. Double-clicking one connects with it, if its password
is remembered; otherwise, type the password and press **Connect**.
**Delete** removes the selected one.

Connections are stored in `~/Library/Application Support/sqlc/connections`
(mode `0600`; override the folder with `SQLC_CONFIG_DIR`). With
**Remember password** ticked, the password is saved in that file too, in plain
text: only your user account can read it, but anyone with access to your
account (or a backup of that folder) can. Leave it unticked for connections
whose password you'd rather type each time.

### Tabs

The window holds one or more tabs, in a strip along its top: left-aligned,
the selected one highlighted, each with its title and an **x**, and a **+**
right after the last one. Click a tab to bring it to front. **+** (or
**File › New Tab**, ⌘T) always opens a new tab on the welcome screen;
connecting fills it with the full layout. Tabs get narrower as you add them,
down to a minimum width; a long title is cut with an ellipsis and shown in
full when you hover over the tab.

Each tab is a workspace of its own: its connection, current database, table
list, editor, results, structure, row editing, status line, running job and
export. Tabs can be connected to different servers, or several to the same
one, and a query or export in one tab never blocks or changes another. Menus
and shortcuts (Run, Export, Ask Claude, ...) act on the tab in front, and a
job finishing in a tab behind it updates that tab.

A tab's title is the saved connection it was opened with, else
`user@host / database` (`user@host` with no database selected, updated after
`USE`), or "New connection" on the welcome screen; the window's title follows
the tab in front. Saved connections are shared: saving or deleting one
updates the list in every tab.

A tab's **x**, a middle click anywhere on it, or **File › Close Tab** (⌘W,
the tab in front) disconnects and closes it. If a query or an export is
running in it, or a browsed table has an unsaved new row, it asks first (Cancel is the default); the export is cancelled and its
partial file deleted, and a running query finishes on its own before the
connection is closed. Closing the last tab leaves a fresh welcome tab in its
place, never an empty window.

Quitting (⌘Q or closing the window) while anything is running in any tab
asks first and lists it, one line per job (for example
`root@localhost / shop - export of shop` and `staging - query`). Running
exports are cancelled and sqlc-gui quits once they have stopped and removed
their temporary files ("Cancelling the export before quitting..." in their
tabs' status lines meanwhile); a running query is simply dropped.

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

### Browsing a table

Double-clicking a table in the sidebar browses it: its first 1000 rows,
ordered by its primary key (every key column, for a composite key), into an
editable grid (see [Editing rows](#editing-rows)). The `SELECT` is written
into the editor, e.g. ``SELECT * FROM `orders` ORDER BY `id` LIMIT 1000;``
(the server is asked for 1001 rows, to know whether there is a next page).

Under the grid, a page bar shows **<**, the rows on screen (`Rows 1–1,000`,
`Rows 1,001–2,000`, `No rows`) and **>**. **<** is disabled on the first page
and **>** when no rows come after this page; each page adds `OFFSET 1000`,
`OFFSET 2000`, ... to the editor's `SELECT`. There is no total row count
(no `COUNT(*)` on a large table). A table without a primary key isn't
ordered, so the server may return the rows in a different order on each
page; the label says so: `Rows 1,001–2,000 (no primary key: order not guaranteed)`.

The **filter bar** above the editor (Normal view) filters the browsed table
by one column: a column dropdown, an operator (`=`, `!=`, `<`, `>`, `<=`,
`>=`, `LIKE`, `NOT LIKE`, `IS NULL`, `IS NOT NULL`), a value, **Apply** and
**Reset**. The column starts on the first primary key column (else the
table's first column) and is refilled when another table is browsed.
**Apply**, or Return in the value field, shows the first page of the
matching rows, e.g. ``SELECT * FROM `orders` WHERE `status` = 'paid' ORDER BY `id`
LIMIT 1000;``, and **<** / **>** then page through those. The value is
escaped for the server and the column name quoted, so any text is safe to
type. `LIKE` and `NOT LIKE` search for the value anywhere in the column
when it has no wildcard (`john` runs `LIKE '%john%'`); a value with `%` or
`_` is used as typed (`john%` = starts with john). The value field is disabled for
`IS NULL` and `IS NOT NULL`, and an empty value means no filter (so `= ''`
can't be asked for here; type that query in the editor). **Reset** clears
the value, puts the column back to its default and the operator to `=`, and
shows the first page with no filter; it is disabled while no filter is
applied and the first page is shown.

Both bars only work for a browsed table: running your own SQL (or Claude's)
disables the filter bar and shows no page bar, since that SQL may have its
own `LIMIT`. Both are disabled while a query runs, and the filter bar while
nothing is browsed.

### Editing rows

Double-click a table in the sidebar to browse it; its grid can then be edited,
with a toolbar below it.

The rows of your own query (or one Claude wrote) can be edited the same way
when every column is a plain column of one table in the current database:
`SELECT * FROM orders WHERE total > 100`, `SELECT id, name AS n FROM
customers ORDER BY n` and `SELECT o.* FROM orders o JOIN ...` all qualify. The server says
where each column comes from, so aliases and any column order work. The
toolbar names the table ("Rows of `orders`. ..."), and edits work as below
with two differences: **Reload** runs the query again (the one that made the grid, not the editor's current text), and
**Delete All Rows…** isn't offered, since the query may show only some of
the rows. A grid stays read-only, with the reason in the toolbar, when:

- a column is an expression (`COUNT(*)`, `price * qty`, a literal) or appears
  twice, or the columns come from more than one table (`SELECT o.id, c.name ...`);
- the query leaves out the primary key (or, without a key, leaves out some
  columns), so a row can't be told apart from others: add the key to the
  query;
- the table is a view, or in another database (`SELECT * FROM other.t`);
- the editor ran several statements (only a single result set is editable).

Inserting a row into a grid that shows only some columns gives the others
their defaults. After a change, the row is read back with the grid's
columns.

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
- **Reload** reads the table again: for a browse, the same page with the same
  filter (after deleting rows a page may come back with fewer rows, or none:
  `No rows`, with **<** still there). **Delete All Rows…** truncates it (asks
  first) and shows the first page.
- Editing works the same on any page and with a filter applied: rows are
  saved by their key, whatever page they are on.
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

Every export opens a **separate, hidden connection** with the same login
(host, port, user, password, `utf8mb4`) to the current database, and closes
it when the export ends, fails or is cancelled. The main connection stays
free: while an export runs you can browse tables, run queries, edit rows and
look at structures. One export runs at a time in each tab (tabs export
independently); starting another in the same tab says "An export is already
running - wait for it or cancel it." Its progress bar, **Cancel** and finish
alert belong to the tab it was started from. If the export's
connection can't be opened (the password changed, `max_connections`, the
network), the export fails with that error and nothing else changes.

On its connection the export runs inside `START TRANSACTION WITH CONSISTENT
SNAPSHOT` (repeatable read), so it sees committed data only, as of the moment
it started, and a whole database is a consistent snapshot across its tables
(InnoDB; MyISAM and other non-transactional tables are read as they are when
reached). It doesn't see the main connection's session state: `SET`
variables, temporary tables, or changes in a transaction you haven't committed.

While any export runs (one table, selected tables or the whole database), a
progress bar and a **Cancel** button sit next to the status line, which keeps
saying what is being written:

- **Progress**: one table moves by rows written against its row estimate
  (`information_schema.TABLES.TABLE_ROWS`, not a `COUNT(*)`); several tables
  are weighted by their estimates, so one huge table moves the bar for as long
  as it takes. Estimates can be off by tens of percent on InnoDB, so the bar
  is approximate and stays below 100% until the export has really finished.
  With no estimate (a view, or a single table the server has none for) the bar
  is indeterminate; with no estimates at all for several tables it steps
  table by table.
- **Cancel** asks first ("Cancel the export? The partial file will be
  deleted."); **Keep exporting** is the default, so Return doesn't cancel and
  Escape does nothing. The export keeps running while you decide, and if it
  finishes in the meantime, confirming does nothing. Once confirmed it stops
  at the next table or within a few thousand rows, the temporary file is
  deleted (the target file is never created, and one that already exists is
  left as it was) and the status line reads "Export cancelled.". The rest of
  the table being read is skipped first, which can take a moment on a big
  table. **Disconnect** and quitting also cancel a running export, after
  asking.
- **When it ends**, the bar is hidden. A finished export plays a sound and
  shows an alert with how long it took and what was written ("Export
  finished in 2m 14s - 12 tables, 1,204,331 rows, saved to ~/dump.sql."); a
  failed one shows the error and the time until it failed, with the system
  alert sound; a cancelled one shows neither. If sqlc-gui is in the
  background, its Dock icon bounces and the alert is waiting when you come
  back. The status line keeps its usual final message.

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
| `src/gui.c` | GUI (libui-ng), shared by macOS and Windows |
| `src/platform.c` | The file calls that differ between macOS and Windows: settings folder, UTF-8 paths, replacing a file in one step, temporary files, `getline`, random numbers (no UI) |
| `src/win/` | The Windows versions of the `*.m` shims in this table, one `*_win.c` per header (Win32 / common controls on libui-ng's Windows API), `win_util.c` (UTF-8 / UTF-16, registry, fonts), `dlg_win.c` (dialogs built in memory: prompts, export options), and `sqlc.rc` / `sqlc.manifest` (common controls 6, DPI awareness, UTF-8 code page) |
| `src/macos_menu.m` | Native Edit menu and Query › Run (⌘↩), File › New Tab (⌘T) and Close Tab (⌘W), which libui-ng lacks; small control sizes and widths; Return in a text field (the filter bar's value); the end-of-export sound, Dock bounce and alert |
| `src/splitview.m` | Resizable panes (sidebar, editor / results, columns / indexes): an `NSSplitView` wrapped as a libui control |
| `src/loading.m` | The loading spinner with a tip under it (connecting, refreshing tables): an `NSProgressIndicator` and a wrapping label wrapped as a libui control |
| `src/sql_tips.c` | The 50 SQL tips shown while connecting or loading tables, and the no-repeat random pick |
| `src/tabs.c` | Tab titles and the quit / close-tab list of running jobs (no UI) |
| `src/sidebar.c` | The sidebar search field's name match (no UI) |
| `src/tabbar.m` | The window's tab strip: left-aligned tabs with a close button, "+", middle click to close; a custom `NSView` wrapped as a libui control |
| `src/segmented.m` | The Normal / AI / Structure switch: an `NSSegmentedControl` wrapped as a libui control |
| `src/export.c` | Streaming SQL / CSV / XML writers, the progress bar's value, time and count formatting, and the temporary file an export is written to (no database code) |
| `src/export_db.c` | Export a table, the whole current database or selected tables as SQL, from a connection through those writers |
| `src/export_dialog.m` | Native export options dialogs (table, database or selected tables) and save panel |
| `src/table_menu.m` | Sidebar right-click menu, rename and API-key prompts, confirmations (Cancel / Keep exporting as the default), clipboard |
| `src/row_edit.c` | Row editing without UI: column kinds, value checks, `UPDATE` / `INSERT` / `DELETE` SQL |
| `src/browse.c` | Browsing a table without UI: the page's `SELECT` (filter, `ORDER BY` the key, `LIMIT` / `OFFSET`), its key columns, the page bar's label |
| `src/grid_edit.m` | In-place cell editor for the results grid (text field, calendar popover, cell menu) |
| `src/ai.c` | Ask Claude: request body, Claude API call (libcurl), reply parsing, editor text, destructive-statement check |
| `src/ai_db.c` | Current database's schema as text for the prompt |
| `src/json.c` | Small JSON builder and parser used by `ai.c` |
| `tests/` | Unit tests (`make test`), e.g. `tests/browse_test.c` for `src/browse.c` |
| `.github/workflows/windows.yml` | Builds `sqlc-gui.exe` with MSYS2, runs `make win-test`, uploads the zip |
| `third_party/libui-ng` | libui-ng submodule |

## Development

`make test` runs the unit tests for the saved-connections file, the export
writers' escaping/quoting, the database dump's header, comments, view
ordering and table selection, Ask Claude's request/response handling, comment formatting and
destructive-statement check, the row editor's value checks and generated
SQL, the browse page's `SELECT` (filter operators, escaping, `ORDER BY` the
key, `OFFSET`), key columns and page labels, the SQL tips' length, format and no-repeat pick, the tab titles and
running-jobs list (truncation, UTF-8), the sidebar search's name match, which errors mean a lost
connection, and the platform file calls (replacing a file, temporary files,
`getline`, UTF-8 file names), the export file name made safe for Windows, and
the date a cell editor's calendar shows for typed text (no database or network needed).

`make win-check` compiles the GUI for Windows without linking, and
`make win-test` builds and runs the unit tests that work on Windows (platform,
saved connections, row editing, browse, SQL tips, tabs, sidebar search); see
[Windows](#windows).

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
a primary key and a read-only view, the filter bar (`IS NOT NULL`, `LIKE` with
a quote, **Reset**), the page bar, and that the user's own query disables both,
and the welcome screen at launch and after **Disconnect**). It expects a database (`SQLC_TEST_DB`, default `test_sqlc_demo`) with tables
`customers` and `orders` plus a view.

## Limitations

- The `--no-views` filter applies only when `SHOW TABLES` is the sole statement
  sent (not when combined with others, e.g. `SHOW TABLES; SELECT 1;`).
- A `;` at the end of a line inside a quoted string ends the statement early.
- Column widths are counted in bytes, so non-ASCII text may misalign.
- No `\G` vertical output, TLS options or prepared statements yet.
- GUI: only rows from a single table can be edited (a browsed table, or a
  query's rows from one table, see [Editing rows](#editing-rows)). A browse
  shows 1000 rows a page (not configurable) with no total row count; without
  a primary key the order between pages isn't guaranteed. The filter bar
  filters by one column, and can't ask for an empty value (`= ''`).
  There's no syntax highlighting, and it always runs the whole editor (not just
  the selection). A running query can't be cancelled yet.
- Database and multi-table export are SQL only and GUI only. The consistent
  snapshot covers transactional (InnoDB) tables only. Views keep their
  `DEFINER`, as `SHOW CREATE VIEW` writes it; loading them as another user
  may need the privilege to set that definer. MariaDB system-versioned tables
  export their current rows only, without history.
- GUI: one export at a time per tab, and a tab's status line is shared by its
  jobs: an export's progress messages replace a query's result message in that
  tab while it runs. With many tabs open, once they reach their minimum width
  the strip is cut off at the window's right edge (no scrolling or overflow
  menu yet): tabs past it, and **+**, can't be clicked until some are closed
  or the window is widened; ⌘T still opens a new tab.
- Windows: `make gui-test` and `make export-test` are macOS only, so the
  Windows GUI is checked by hand.
- Ask Claude is GUI only (no `sqlc` CLI command yet), and the window is busy
  (Run disabled) until Claude answers; a request can't be cancelled.

## License

[MIT](LICENSE)
