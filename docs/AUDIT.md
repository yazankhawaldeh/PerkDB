# PerkDB API Audit

Audit date: 2026-10-12. Sources reviewed: `perkdb.h`, `perk_internal.h`, `perkdb.c`,
`perk_crud.c`, `perk_result.c`, `perk_tx.c`, `perk_util.c`, `perk_where.c`, `Makefile`,
and the existing files in `tests/`.

No production code was changed for this audit.

## Summary

The library already covers the basic CRUD verbs in one call each: `perk_create_table`,
`perk_insert`, `perk_select`, `perk_update` and `perk_delete`. Every value goes
through `sqlite3_bind_*`, table and column names are checked by `validate_identifier()`
and then double-quoted, and WHERE clauses are tokenized against a whitelist and rebuilt
before they reach SQLite.

The main gaps are:

- Constraint violations can't be told apart from other SQL errors.
- `perk_select` has no ordering or pagination.
- There is no upsert, no bulk insert, no count, and no drop table.

## Error codes (`PerkErr`)

| Code | Meaning today |
|---|---|
| `PERK_OK` | Success |
| `PERK_ERR_OPEN` | `sqlite3_open` or the startup PRAGMAs failed |
| `PERK_ERR_SQL` | Any other SQLite failure, including constraint violations (UNIQUE, NOT NULL, PK, FK) and over-long SQL |
| `PERK_ERR_BUSY` | `SQLITE_BUSY` / `SQLITE_LOCKED` |
| `PERK_ERR_BADNAME` | Invalid table/column name, unsupported column type, or invalid name in a WHERE clause |
| `PERK_ERR_NOMEM` | Allocation failure |
| `PERK_ERR_MISUSE` | NULL handle, missing arguments, malformed `PerkValue`, bad WHERE syntax, `?` count mismatch, transaction misuse |
| `PERK_ERR_EXISTS` | `perk_create_table`: the name is already taken |
| `PERK_ERR_NOT_FOUND` | `perk_select_by_id` found no row; `perk_update` / `perk_delete` matched no rows |

Gaps: there is no `PERK_ERR_CONSTRAINT`.

## Public functions (`perkdb.h`)

### Value constructors (static inline, `perkdb.h`)

| Function | What it does | CRUD op | Known gaps |
|---|---|---|---|
| `perk_int(int64_t)` | Builds a `PERK_INT` value | Support (C/R/U/D args) | none |
| `perk_real(double)` | Builds a `PERK_REAL` value | Support | none |
| `perk_null(void)` | Builds a `PERK_NULL` value | Support | none |
| `perk_text(const char*)` | Builds a `PERK_TEXT` value using `strlen`. A NULL pointer becomes `PERK_NULL` | Support | Text with embedded NULs needs a hand-built `PerkValue`. There is no `perk_text_n(ptr, len)` |
| `perk_blob(const void*, size_t)` | Builds a `PERK_BLOB` value | Support | A NULL pointer with len 0 is stored as a zero-length blob, not as NULL. This is intended but not documented in the header |

### Connection (`perkdb.c`)

| Function | What it does | CRUD op | Known gaps |
|---|---|---|---|
| `perk_open(path, &out)` | Allocates a `PerkDB`, opens the file, hardens the connection (defensive mode, load_extension off, TRUSTED_SCHEMA off, DQS off, ATTACH limit 0) and sets WAL, `busy_timeout=5000`, `foreign_keys=ON` and `synchronous=NORMAL`. On failure `*out` is NULL and the message goes to a thread-local buffer | Lifecycle | No open flags (read-only, no-create) and no way to pick PRAGMAs. WAL can't be used with `:memory:`, but the PRAGMA result is not checked for that, so this is harmless |
| `perk_close(db)` | Closes the connection and frees the handle. NULL is ignored. An open transaction is rolled back | Lifecycle | Returns `void`, so a failed `sqlite3_close` can't be reported (it can't currently fail because every statement is finalized) |
| `perk_errmsg(db)` | Returns the last error message for `db`, or for the last failed `perk_open` on this thread if `db` is NULL | Support | The message is not cleared when a call succeeds, so it can be stale. Calls that return `PERK_ERR_MISUSE` because `db == NULL` set no message |

### Create (`perk_crud.c`)

| Function | What it does | CRUD op | Known gaps |
|---|---|---|---|
| `perk_create_table(db, table, cols, ncols)` | Builds `CREATE TABLE` from `PerkColumn[]`, with a type whitelist (INTEGER, INT, REAL, TEXT, BLOB, NUMERIC or NULL) and PK/NOTNULL/UNIQUE flags. Several PK columns make a composite key | Create (schema) | Returns `PERK_ERR_EXISTS` if any schema object already holds the name (case-insensitive). No DEFAULT, CHECK, FOREIGN KEY/REFERENCES, AUTOINCREMENT, or WITHOUT ROWID support. No way to check whether a table exists. No drop table (see Delete) |
| `perk_insert(db, table, cols, vals, n, &rowid)` | Inserts one row with bound values. Can return the new rowid | Create (row) | Only one row per call, so there is no bulk insert. No upsert or conflict handling (`INSERT OR REPLACE`, `ON CONFLICT ... DO UPDATE`). Constraint violations come back as the generic `PERK_ERR_SQL`. `rowid_out` means nothing for WITHOUT ROWID tables (not creatable today anyway) |

### Read (`perk_crud.c`, `perk_result.c`)

| Function | What it does | CRUD op | Known gaps |
|---|---|---|---|
| `perk_select(db, table, cols, ncols, where, args, nargs, &out)` | `SELECT` of the given columns (or `*`) with an optional whitelisted WHERE. Copies every row into a `PerkResult` | Read | No ORDER BY, LIMIT or OFFSET, so there is no pagination and everything is loaded into memory. A NULL or empty WHERE lists every row; a filter matching nothing is a 0-row `PERK_OK` result. No COUNT/aggregates. No DISTINCT. No streaming or cursor API |
| `perk_select_by_id(db, table, cols, ncols, id_col, &id, &out)` | Reads the row(s) whose `id_col` equals the bound `id`. Returns `PERK_ERR_NOT_FOUND` (and `*out == NULL`) if none match | Read (by id) | A non-unique `id_col` returns every matching row. A `PERK_NULL` id never matches |
| `perk_result_rows(res)` | Number of rows (0 if `res` is NULL) | Read | none |
| `perk_result_cols(res)` | Number of columns (0 if `res` is NULL) | Read | none |
| `perk_result_colname(res, col)` | Column name, or NULL if out of range | Read | No lookup of a column index by name |
| `perk_result_get(res, row, col)` | Pointer to the `PerkValue` cell, or NULL if out of range | Read | No typed accessors (for example get-int or get-text with a type check). The caller has to switch on `type` |
| `perk_result_free(res)` | Frees the result and every copied text, blob and name. NULL is ignored | Read | none |

### Update (`perk_crud.c`)

| Function | What it does | CRUD op | Known gaps |
|---|---|---|---|
| `perk_update(db, table, set_cols, set_vals, nset, where, args, nargs, &changed)` | `UPDATE ... SET col = ?` with an optional WHERE. Can report the number of changed rows | Update | When no row matches (including an empty table) it returns `PERK_ERR_NOT_FOUND` with `changed == 0`. The WHERE is optional, so a NULL or empty WHERE updates every row with no explicit opt-in. Only `col = value` is possible: no expressions such as `count = count + ?`. Constraint violations are `PERK_ERR_SQL` |

### Delete (`perk_crud.c`)

| Function | What it does | CRUD op | Known gaps |
|---|---|---|---|
| `perk_delete(db, table, where, args, nargs, &changed)` | `DELETE FROM` with an optional WHERE. Can report the number of deleted rows | Delete (row) | Returns `PERK_ERR_NOT_FOUND` with `changed == 0` when no row matches (including an empty table). A NULL WHERE deletes every row with no explicit opt-in. There is no `perk_drop_table`, so a table can't be deleted |

### Transactions (`perk_tx.c`)

| Function | What it does | CRUD op | Known gaps |
|---|---|---|---|
| `perk_begin(db)` | `BEGIN IMMEDIATE`. Returns `PERK_ERR_MISUSE` if a transaction is already open | Support (multi-op writes) | No nesting or savepoints |
| `perk_commit(db)` | `COMMIT`. Returns `PERK_ERR_MISUSE` if no transaction is open | Support | none |
| `perk_rollback(db)` | `ROLLBACK`. Returns `PERK_ERR_MISUSE` if no transaction is open | Support | none |

## Internal functions (`perk_internal.h`, not public)

Listed for completeness. Callers can't use these.

| Function | File | What it does |
|---|---|---|
| `perk_is_ident_start`, `perk_is_ident_char` | `perk_util.c` | Character classes for identifiers |
| `validate_identifier` | `perk_util.c` | Checks `[A-Za-z_][A-Za-z0-9_]*`, at most 64 characters, and no `sqlite_` prefix |
| `perk_set_error` | `perk_util.c` | Writes `db->errmsg` |
| `perk_sqlite_error` | `perk_util.c` | Copies SQLite's message and maps the rc to a `PerkErr` (BUSY/LOCKED to BUSY, NOMEM to NOMEM, everything else to SQL) |
| `perk_finish_sql`, `perk_discard_sql` | `perk_util.c` | Finish or free a `sqlite3_str` builder |
| `perk_bind_values` | `perk_util.c` | Binds `PerkValue[]` with `sqlite3_bind_*` |
| `perk_append_where` | `perk_where.c` | Tokenizes and rebuilds a WHERE clause against a whitelist; `IN` must be followed by `(`, `LIKE`/`GLOB` must follow an operand |
| `perk_result_from_stmt` | `perk_result.c` | Steps a statement and copies all rows into a `PerkResult` |
| `harden_connection` (static) | `perkdb.c` | Applies the `sqlite3_db_config` / `sqlite3_limit` safety settings at open |
| `check_names`, `prepare_sql`, `run_write`, `match_column_type`, `begin_select`, `finish_select`, `not_found_if_unchanged` (static) | `perk_crud.c` | Name checks, prepare, step-to-done, the column type whitelist, shared SELECT building/running, and mapping 0 changed rows to `PERK_ERR_NOT_FOUND` |

Note: `validate_identifier` has no `perk_` prefix even though it is an external symbol in
the static library. It could collide with a symbol in a user's program.

## CRUD coverage matrix

| Feature | Status |
|---|---|
| Create table | Done; returns `PERK_ERR_EXISTS` if the name is taken |
| Drop table | **Missing** |
| Insert one row | Done |
| Insert many rows in one call | **Missing** |
| Upsert / insert-or-replace | **Missing** |
| Distinguish constraint violations | **Missing** (all are `PERK_ERR_SQL`) |
| Read list-all / filtered | Done (`perk_select` with a NULL WHERE) |
| Read by id with NOT_FOUND | Done (`perk_select_by_id`) |
| Read with ORDER BY / LIMIT / OFFSET | **Missing** |
| Count rows matching a filter | **Missing** |
| Update with NOT_FOUND | Done (`perk_update` returns `PERK_ERR_NOT_FOUND` when 0 rows change) |
| Delete with NOT_FOUND | Done (`perk_delete` returns `PERK_ERR_NOT_FOUND` when 0 rows are deleted) |

Items in **bold** have no backlog story yet. A new story was added for each one at the
end of `BACKLOG.md`.

## Other observations (not CRUD features, no story added)

- **API shape vs. GUIDELINES.md:** The "Target feel" in GUIDELINES.md uses `db_open`,
  `db_insert(..., "si", ...)` with type strings, and `db_rows_t`. The real API uses the
  `perk_` prefix, `PerkValue` arrays and `PerkResult`. Both bind every value, so the
  injection protection is the same, but the names and calling style are different.
- **Tests already exist:** `tests/test_perkdb.c`, `tests/test_injection.c` and
  `tests/test_rw.c` exist, and the Makefile already has `test` and `bench` targets with
  ASan/UBSan. The "Baseline" section of GUIDELINES.md says no test suite exists yet. These
  tests were not run as part of this audit.
- **Compiler flags:** The Makefile uses `-std=c11 -Wall -Wextra -Wpedantic` without
  `-Werror`. GUIDELINES.md asks for `-Werror`.
- **Formatting:** The source uses 4-space Allman-style braces, not the Google
  clang-format style named in GUIDELINES.md.

## SQL injection hardening (story "SQL injection hardening", 2026-10-12)

Every place SQL text is built was traced. Verdicts:

| Site | Verdict |
|---|---|
| `perk_create_table` (`perk_crud.c`): table/column names | Validated by `validate_identifier()` before use, emitted as `"%w"` |
| `perk_create_table`: column types | Emitted only from the fixed `column_types[]` whitelist, never the caller's string |
| `perk_create_table`: flags | Unknown bits rejected; only constant `PRIMARY KEY` / `NOT NULL` / `UNIQUE` text is emitted |
| `find_schema_object` (`perk_crud.c`) | Constant SQL; name is bound (`?1`) |
| `perk_insert`, `perk_update`, `perk_delete`, `perk_select`, `perk_select_by_id` | Names validated then `"%w"`-quoted; every value goes through `perk_bind_values()` (`sqlite3_bind_*`) |
| `perk_append_where` (`perk_where.c`) | Rebuilt from whitelisted tokens. **Two bypasses fixed** (below) |
| `perk_open` PRAGMAs (`perkdb.c`) | Constant string, no caller input |
| `BEGIN IMMEDIATE` / `COMMIT` / `ROLLBACK` (`perk_tx.c`) | Constant strings, no caller input |
| `validate_identifier` (`perk_util.c`) | Implements `^[A-Za-z_][A-Za-z0-9_]{0,63}$` with explicit ASCII ranges (no `ctype`/locale); bytes >= 0x80 are negative or out of range and rejected; NULL, "", 65+ chars rejected; `sqlite_` prefix (any case) rejected |
| `prepare_sql` (`perk_crud.c`) | Previously ignored the prepare tail. Now refuses SQL with more than one statement (defense in depth; generated SQL can never contain `;`) |

Fixed WHERE bypasses (both reached data outside the named table or the "no functions" rule):

- `id IN secret` was valid SQLite (`expr IN table-name`) and read another table. `IN`
  must now be followed by `(`; otherwise `PERK_ERR_MISUSE`.
- `like(?, ?)` / `glob(?, ?)` in operand position were parsed by SQLite as function
  calls. `LIKE` / `GLOB` are now accepted only after a column, `?`, `)` or `NULL`
  (optionally with `NOT` between); otherwise `PERK_ERR_MISUSE`.

Connection hardening added in `perk_open`: `SQLITE_DBCONFIG_DEFENSIVE` on,
`SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION` off, `SQLITE_DBCONFIG_TRUSTED_SCHEMA` off,
`SQLITE_DBCONFIG_DQS_DML/DDL` off (also compiled in with `-DSQLITE_DQS=0`), and
`SQLITE_LIMIT_ATTACHED` = 0. If any setting fails, `perk_open` returns `PERK_ERR_OPEN`.

Behavior changes: the two WHERE forms above are now rejected; opened databases can't
use writable_schema, load_extension or ATTACH (none were reachable through the API).
