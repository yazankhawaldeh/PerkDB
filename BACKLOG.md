# Backlog

<!--
One story per checkbox. The orchestrator works top to bottom.
- [ ] open    - [x] done    - [!] blocked (needs you)
Indent acceptance criteria under each story so the testers and QA can check them.
-->

- [x] Audit existing API and plan remaining work
  - Write docs/AUDIT.md listing every public function: what it does, which CRUD op it covers, known gaps
  - Append one new story per missing CRUD feature to the end of BACKLOG.md, with acceptance criteria
  - No production code changes in this story
- [x] Build test harness
  - `make test` compiles everything in tests/ and runs it; exits non-zero on any failure
  - Tests build with -fsanitize=address,undefined -g
  - Small assert macros print file, line, and expected vs actual
- [x] Complete create-table
  - Creates a table from a caller-supplied schema; returns the error enum if it already exists or the name is invalid
  - Tests: happy path, duplicate table, invalid name, NULL args
- [x] Complete insert / read / update / delete
  - Read supports by-id and list-all; update/delete return a NOT_FOUND error for missing rows
  - Every op has happy-path, missing-row, and NULL/empty-input tests
- [x] SQL injection hardening
  - Every value goes through sqlite3_bind_*; no user data is ever formatted into SQL text
  - Table/column names are validated against ^[A-Za-z_][A-Za-z0-9_]{0,63}$ and rejected otherwise
  - Tests feed payloads like `'; DROP TABLE x;--` and `" OR 1=1` to every function and confirm the data is unchanged
- [x] Memory safety pass
  - Full test suite runs clean under ASan/UBSan (zero reports)
  - Tests cover oversized strings, empty strings, max-length identifiers, and NULL pointers for every function
- [ ] Node addon: build and load
  - node-addon-api + node-gyp; `npm install && node -e "require('./')"` works on macOS and Linux
- [ ] Node addon: expose CRUD functions
  - Every C function is callable from JS; C error enums become thrown Errors with a `code` property
  - DB handle is closed when the JS object is garbage-collected
- [ ] Node addon: TypeScript types and example
  - index.d.ts covers every function; examples/server.ts runs a tiny HTTP CRUD server using the addon
- [ ] Drop table
  - New `perk_drop_table(PerkDB*, const char *table)` deletes a table; returns PERK_ERR_BADNAME for an invalid name and PERK_ERR_NOT_FOUND (or a documented error) if the table does not exist
  - Table name is validated with validate_identifier() and quoted; nothing user-supplied is formatted into SQL unchecked
  - Tests: happy path (table gone, later select fails), missing table, invalid/injection name, NULL args
- [ ] Bulk insert
  - New function inserts many rows of the same columns in one call inside a single transaction; all-or-nothing on failure
  - Every value is bound with sqlite3_bind_*; optional out-param reports rows inserted
  - Tests: happy path (N rows readable afterwards), zero rows, one bad row rolls back the whole batch, NULL args, injection payloads stored verbatim
- [ ] Upsert (insert or update on conflict)
  - New function inserts a row or, on a conflict with a caller-named key column(s), updates the given columns instead
  - Conflict and update columns are validated identifiers; values are bound, never formatted into SQL
  - Tests: insert when absent, update when present, invalid conflict column, NULL args, injection payloads
- [ ] Constraint violation error code
  - Add PERK_ERR_CONSTRAINT to PerkErr; perk_sqlite_error maps SQLITE_CONSTRAINT (UNIQUE, NOT NULL, PRIMARY KEY, FOREIGN KEY) to it instead of PERK_ERR_SQL
  - perk_errmsg() still describes which constraint failed
  - Tests: duplicate UNIQUE/PK insert, NULL into NOT NULL column, FK violation, update that breaks a constraint
- [ ] Select ordering and pagination
  - perk_select (or a new variant) accepts ORDER BY columns with ASC/DESC plus LIMIT and OFFSET
  - ORDER BY columns are validated identifiers; LIMIT/OFFSET are bound as integers; negative values are rejected with PERK_ERR_MISUSE
  - Tests: ascending/descending order, limit, offset past end returns zero rows, invalid/injection column name, negative limit
- [ ] Count rows
  - New `perk_count(PerkDB*, const char *table, const char *where, const PerkValue *args, size_t nargs, int64_t *out)` returns the number of matching rows without materialising them
  - Uses the same WHERE whitelist and binding as perk_select
  - Tests: empty table, filtered count, NULL where counts all rows, invalid table, injection payloads in args, NULL out
