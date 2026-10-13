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
- Load it with `node -e "require('./')"`. For now it only exports `loaded`, `version()` and `sqliteVersion()`; the CRUD functions come later

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

