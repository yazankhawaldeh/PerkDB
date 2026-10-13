#ifndef PERK_INTERNAL_H
#define PERK_INTERNAL_H

// Shared between the PerkDB source files, not part of the public API

#include "perkdb.h"
#include "sqlite3-src/sqlite3.h"

struct PerkDB
{
    sqlite3 *conn;
    char errmsg[256];
};

// cells is row-major: the value at (row, col) is cells[row * ncols + col].
// Every text/blob pointer in cells and every col_names entry is owned by the result.
struct PerkResult
{
    size_t nrows, ncols;
    char **col_names;
    PerkValue *cells;
};

// Longest table/column name accepted by validate_identifier()
#define PERK_MAX_IDENT 64

int perk_is_ident_start(char c);
int perk_is_ident_char(char c);

/*
 * Checks a table/column name against ^[A-Za-z_][A-Za-z0-9_]{0,63}$ (ASCII only, no
 * locale-dependent ctype calls) and rejects the reserved "sqlite_" prefix (any case).
 * @param name NUL-terminated name, may be NULL
 * @return 1 if the name is valid, 0 otherwise (NULL, "", too long, bad byte, reserved)
 */
int validate_identifier(const char *name);

void perk_set_error(PerkDB *db, const char *msg);

// Copies SQLite's error message into db->errmsg and maps rc to a PerkErr
PerkErr perk_sqlite_error(PerkDB *db, int rc);

// Frees s and hands back the finished SQL in *out (release with sqlite3_free)
PerkErr perk_finish_sql(PerkDB *db, sqlite3_str *s, char **out);

// Frees s without using its contents, for error paths
void perk_discard_sql(sqlite3_str *s);

// Binds vals to parameters first_index .. first_index + n - 1
PerkErr perk_bind_values(PerkDB *db, sqlite3_stmt *st, const PerkValue *vals, size_t n, int first_index);

// Validates a user WHERE clause and appends it to s (perk_where.c)
PerkErr perk_append_where(PerkDB *db, sqlite3_str *s, const char *where, size_t nargs);

// Steps st to completion, copying every row into a new PerkResult (perk_result.c)
PerkErr perk_result_from_stmt(PerkDB *db, sqlite3_stmt *st, PerkResult **out);

#endif
