# PerkDB

## Description
This is a rudimentary CRUD API to write to a single file SQLite amalgamation database. This offers exceptional performance for small projects, and the API is written in C for super fast writes. This can be an alternative to PostgreSQL or other databases.
## Building
- `make` builds `build/libperkdb.a`, which contains PerkDB and SQLite
- `make shell` builds the `build/sqlite3` command-line shell for inspecting database files
- `make test` builds and runs the test suite with AddressSanitizer and UndefinedBehaviorSanitizer
- `make clean` removes `build/`

Link your program with: `cc app.c build/libperkdb.a -lpthread -lm` (add `-ldl` on Linux)

## Node addon
PerkDB can also be built as a Node native addon (N-API).

Requirements: Node >= 18.17, a C/C++ toolchain (Xcode Command Line Tools on macOS; `gcc`/`g++` and `make` on Linux) and Python 3 for node-gyp.

- `npm install` installs the dependencies and builds `build/Release/perkdb.node` (it runs `node-gyp rebuild`)
- `npm run build` rebuilds the addon
- `make test-addon` (or `npm test`) runs the Node smoke test `tests/test_addon.js`; build the addon first. `make test` does not need Node
- Load it with `require('./')` (or `require('perkdb')` once installed)

### Usage
```js
const { Database, PerkError } = require('perkdb');

const db = new Database('app.db');            // or Database.open(path); ':memory:' works too
db.createTable('users', [
  { name: 'id', type: 'INTEGER', primaryKey: true },
  { name: 'name', type: 'TEXT', notNull: true },
  { name: 'age', type: 'INTEGER' },
]);
const id = db.insert('users', { name: 'Yazan', age: 21 });   // returns the rowid
db.insert('users', ['name', 'age'], ['Sam', 30]);            // column/value arrays also work
db.select('users', { where: 'age > ?', args: [18] });        // [{ id: 1, name: 'Yazan', age: 21 }, ...]
db.selectById('users', 'id', id, { columns: ['name'] });     // { name: 'Yazan' }
db.update('users', { age: 22 }, 'id = ?', [id]);             // returns the number of rows changed
db.delete('users', 'id = ?', [id]);                          // returns the number of rows deleted
db.begin(); /* ... */ db.commit();                           // or db.rollback()
try {
  db.selectById('users', 'id', 999);
} catch (e) {
  if (e instanceof PerkError && e.code === 'PERK_ERR_NOT_FOUND') { /* no such row */ }
}
db.close();                                                  // idempotent
```

API (each method calls the C function of the same name):
- `new Database(path)` / `Database.open(path)`: `perk_open`. `db.close()`: `perk_close`; calling it again does nothing. If you never call it, the connection is closed when the object is garbage-collected. `db.isOpen` tells you whether it is still open
- `createTable(name, columns)`: each column is `{ name, type?, primaryKey?, notNull?, unique?, flags? }` (`flags` is ORed with `constants.PERK_COL_*`)
- `insert(table, { col: value })` or `insert(table, cols, values)`: returns the rowid
- `select(table, { columns?, where?, args? })`: returns an array of row objects (empty if nothing matches). A missing `where` lists every row
- `selectById(table, idColumn, id, { columns? })`: returns the first matching row and throws `PERK_ERR_NOT_FOUND` if there is none
- `update(table, { col: value }, where?, args?)` and `delete(table, where?, args?)`: return the number of rows changed. They throw `PERK_ERR_NOT_FOUND` when no row matched. A missing `where` affects every row
- `begin()`, `commit()`, `rollback()`; `db.errmsg()` is `perk_errmsg(db)` and the module's `openErrmsg()` is `perk_errmsg(NULL)`
- `constants` holds the numeric `PERK_ERR_*`, `PERK_COL_*` and `PerkType` values

Values are always bound, never pasted into SQL. JS to SQLite: integer-valued number becomes INTEGER (other numbers become REAL), `bigint` becomes INTEGER (a RangeError if it is outside int64), boolean becomes INTEGER 0/1, string becomes TEXT (UTF-8, embedded NUL characters kept), `Buffer`/`Uint8Array` becomes BLOB (copied), and `null`/`undefined` becomes NULL. Anything else throws a TypeError with `code: 'ERR_INVALID_ARG_TYPE'`. SQLite to JS: INTEGER becomes number, or `BigInt` beyond ±(2^53 - 1); REAL becomes number; TEXT becomes string; BLOB becomes `Buffer`; NULL becomes `null`. The same number/BigInt rule applies to returned rowids and counts. Table, column, WHERE and path strings must not contain NUL characters (TypeError, `code: 'ERR_INVALID_ARG_VALUE'`).

Errors: a failing C call throws a `PerkError` with `code` (the enum name), `errno` (the numeric value) and `message` (from `perk_errmsg`):

| `code` | When |
|---|---|
| `PERK_ERR_OPEN` | The database file could not be opened |
| `PERK_ERR_SQL` | SQLite error, including constraint violations and missing tables |
| `PERK_ERR_BUSY` | The database is locked by another connection |
| `PERK_ERR_BADNAME` | Invalid table/column name or column type |
| `PERK_ERR_NOMEM` | Out of memory |
| `PERK_ERR_MISUSE` | Bad WHERE syntax, `?`/args count mismatch, empty column list, transaction misuse |
| `PERK_ERR_EXISTS` | `createTable`: the name is already taken |
| `PERK_ERR_NOT_FOUND` | `selectById` found no row; `update`/`delete` matched no rows |
| `PERK_ERR_CLOSED` | A method other than `close()` was called after `close()` (`errno` is `PERK_ERR_MISUSE`) |

`binding.gyp` compiles the same C sources and SQLite amalgamation as the Makefile, with the same `-DSQLITE_DQS=0`. Warnings do not fail the addon build (`-Werror` is only used by `make`).

Dependencies: `node-addon-api` (C++ wrapper over N-API) and `node-gyp` (builds the addon) are needed to compile the N-API binding. Neither is used by the C library.

Shared `build/` directory: node-gyp and the Makefile both write to `build/`, and their file names do not overlap (node-gyp writes `build/Makefile`, `build/*.mk`, `build/config.gypi` and `build/Release/`; the Makefile writes `build/*.o`, `build/*.d`, `build/libperkdb.a` and test binaries). Both builds can share the directory, but:
- `make clean` deletes all of `build/`, including the addon. Run `npm run build` to get it back
- `npm install` / `npm run build` (`node-gyp rebuild`) also deletes all of `build/` first, so `make` rebuilds everything the next time it runs
## What can this be used for?
This can be used for pretty much anything that requires a database and needs an easy way to interact with it. This project allows for super fast database handling, especially for embedded targets since the size is small and its a compiled binary. This is also good for small to medium scale web services and cuts out a need for a more complex databasde management system.
## What is the current state of the project?
Very bare bones, and there is only one person working on this project, but progress is coming slowly.
## What are the future plans / what needs to be done?
- Finish API functinality (Will be CRUD)
- Ensure TypeScript/JavaScript functinoality for web servers
- Structure the writing and reading of the API
## Why even make this when PostgreSQL and others exist?
The simplicity of it is what the point is. No external server handling traffic and writes, no special account or API keys needed. Just pure compiled C and SQLite database management. Single folder drop in and you're done. 
## What does the name mean
My nickname from my friends is Perky. 

