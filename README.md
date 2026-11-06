# sqlc

A small MySQL / MariaDB command-line client written in C, built on
[MariaDB Connector/C](https://mariadb.com/kb/en/mariadb-connector-c/).
It works with both MariaDB and MySQL servers and runs natively on Apple Silicon.

## Features

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

It uses `libedit`, which ships with macOS.

## Build

```sh
make            # builds ./sqlc
make debug      # build with AddressSanitizer + UBSan
make compile_flags.txt   # optional: include paths for clangd / your editor
```

## Usage

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

## Limitations

- The `--no-views` filter applies only when `SHOW TABLES` is the sole statement
  sent (not when combined with others, e.g. `SHOW TABLES; SELECT 1;`).
- A `;` at the end of a line inside a quoted string ends the statement early.
- Column widths are counted in bytes, so non-ASCII text may misalign.
- No `\G` vertical output, TLS options or prepared statements yet.

## License

[MIT](LICENSE)
