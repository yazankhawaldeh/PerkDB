# Coding Guidelines

<!-- Replace everything below with your own rules. Every agent reads this file. -->

## Goal
A thin C wrapper that hides SQLite's prepare/bind/step/finalize boilerplate.
Every common operation should be ONE call. Target feel:

    db_t *db = db_open("app.db");
    db_create_table(db, "users", "id INTEGER PRIMARY KEY, name TEXT, age INTEGER");
    db_insert(db, "users", "name, age", "si", "Yazan", 21);   // "si" = string, int
    db_rows_t *rows = db_select(db, "users", "age > ?", "i", 18);
    db_update(db, "users", "age = ?", "id = ?", "ii", 22, 1);
    db_delete(db, "users", "id = ?", "i", 1);
    db_rows_free(rows);
    db_close(db);

Rules:
- The caller never touches sqlite3_stmt.
- Type strings (s=text, i=int, d=double, b=blob, n=null) map to sqlite3_bind_*,
  so values are always bound. This is the SQL injection protection.
- Every function returns the project's error enum; results come back through
  out-params or a rows struct with an obvious free function.





## Stack
- Language / framework: C, no framework, just utilzing the SQLite single file amalgamation along with its api calls 
- Package manager: homebrew, target platforn is Unix-based systems. Your main target is Linux but all of our testing is done on Darwin (macOS)
- Test framework and command: no test framework, you will build your own test suite that checks write and reads using each api and injections to check security  
- Lint / format command: Linting and formatting with clang-format, use the Google style for formatting

## Style
- Follow the existing style of the codebase. 
- Small, single-purpose functions. Descriptive names. Use @param and return to describe each function and what its purpose is 
- Handle errors explicitly; no silent catches. Use the premade enums and error codes for error handling and embed logic to handle said errors

## Testing
- Every new function or endpoint gets tests: happy path, edge cases, errors.
- Tests live in:  root file -> tests/

## Never
- Commit secrets or `.env` files.
- Add a new dependency without a clear reason noted in the summary.

## Baseline
Recorded when the agent pipeline was set up (2026-10-12).

- Tests: no automated test suite exists yet. There are no known-failing tests,
  so ANY test failure from here on is new and counts against the current story.
- Build: `make`. If there is no Makefile yet, the "Build test harness" story
  creates one with `make` (library) and `make test` (build + run tests/) targets.
- Compilers: Apple clang on macOS (where testing happens); gcc or clang on Linux (main target).
  Flags: `-std=c11 -Wall -Wextra -Werror`. Test builds add `-fsanitize=address,undefined -g`.
- Formatting: run `clang-format -i --style=Google` on every changed .c/.h file before commit.
- Until "Build test harness" is done, stories before it (the audit) are checked by
  review and QA only; the tester should report `VERDICT: PASS, CAUSE: none` with
  "no test suite yet" in RESULTS.
