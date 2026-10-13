# Backlog

<!--
One story per checkbox. The orchestrator works top to bottom.
- [ ] open    - [x] done    - [!] blocked (needs you)
Indent acceptance criteria under each story so the testers and QA can check them.
-->

- [ ] Audit existing API and plan remaining work
  - Write docs/AUDIT.md listing every public function: what it does, which CRUD op it covers, known gaps
  - Append one new story per missing CRUD feature to the end of BACKLOG.md, with acceptance criteria
  - No production code changes in this story
- [ ] Build test harness
  - `make test` compiles everything in tests/ and runs it; exits non-zero on any failure
  - Tests build with -fsanitize=address,undefined -g
  - Small assert macros print file, line, and expected vs actual
- [ ] Complete create-table
  - Creates a table from a caller-supplied schema; returns the error enum if it already exists or the name is invalid
  - Tests: happy path, duplicate table, invalid name, NULL args
- [ ] Complete insert / read / update / delete
  - Read supports by-id and list-all; update/delete return a NOT_FOUND error for missing rows
  - Every op has happy-path, missing-row, and NULL/empty-input tests
- [ ] SQL injection hardening
  - Every value goes through sqlite3_bind_*; no user data is ever formatted into SQL text
  - Table/column names are validated against ^[A-Za-z_][A-Za-z0-9_]{0,63}$ and rejected otherwise
  - Tests feed payloads like `'; DROP TABLE x;--` and `" OR 1=1` to every function and confirm the data is unchanged
- [ ] Memory safety pass
  - Full test suite runs clean under ASan/UBSan (zero reports)
  - Tests cover oversized strings, empty strings, max-length identifiers, and NULL pointers for every function
- [ ] Node addon: build and load
  - node-addon-api + node-gyp; `npm install && node -e "require('./')"` works on macOS and Linux
- [ ] Node addon: expose CRUD functions
  - Every C function is callable from JS; C error enums become thrown Errors with a `code` property
  - DB handle is closed when the JS object is garbage-collected
- [ ] Node addon: TypeScript types and example
  - index.d.ts covers every function; examples/server.ts runs a tiny HTTP CRUD server using the addon
