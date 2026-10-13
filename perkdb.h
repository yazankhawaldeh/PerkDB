#ifndef PERKDB_H
#define PERKDB_H



#include <stdint.h>
#include <stdlib.h>
#include <string.h>


// This is defined in perk_internal.h
typedef struct PerkDB PerkDB;


typedef enum {
    PERK_OK = 0,
    PERK_ERR_OPEN, PERK_ERR_SQL, PERK_ERR_BUSY,
    PERK_ERR_BADNAME, PERK_ERR_NOMEM,
    PERK_ERR_MISUSE, // NULL handle, missing arguments, or a malformed PerkValue
    PERK_ERR_EXISTS, // perk_create_table: a table, view or index with that name already exists
    PERK_ERR_NOT_FOUND // perk_select_by_id found no row; perk_update/perk_delete matched no rows
} PerkErr;

// Column inputs to perk_create_table() and flags for Primary Key, Not Null, and Unique
typedef enum {
    PERK_COL_PK = 1 << 0,
    PERK_COL_NOTNULL = 1 << 1,
    PERK_COL_UNIQUE = 1 << 2,
} PerkColFlags;

typedef struct {
    const char *name;
    const char *type;
    int flags;
} PerkColumn;

// Definitions for PerkValue's, generics that are inputted for data insertion into the db

typedef enum {
    PERK_NULL,
    PERK_INT,
    PERK_REAL,
    PERK_TEXT,
    PERK_BLOB
} PerkType;


typedef struct {
    PerkType type;
    union {
        int64_t i;
        double r;
        struct { const char *ptr; size_t len; } text;
        struct { const void *ptr; size_t len; } blob;
    } as;
} PerkValue;

static inline PerkValue perk_int(int64_t v)
{
    PerkValue pv = { .type = PERK_INT, .as.i = v };
    return pv;
}

static inline PerkValue perk_real(double v)
{
    PerkValue pv = { .type = PERK_REAL, .as.r = v };
    return pv;
}

static inline PerkValue perk_null(void)
{
    PerkValue pv = { .type = PERK_NULL };
    return pv;
}

// A NULL string is stored as SQL NULL
static inline PerkValue perk_text(const char *v)
{
    if (v == NULL)
        return perk_null();
    PerkValue pv = { .type = PERK_TEXT, .as.text = { v, strlen(v) } };
    return pv;
}

static inline PerkValue perk_blob(const void *p, size_t n)
{
    PerkValue pv = { .type = PERK_BLOB, .as.blob = { p, n } };
    return pv;
}


typedef struct PerkResult PerkResult;

/*
 * Input rules shared by every function:
 *  - Table and column names must match [A-Za-z_][A-Za-z0-9_]*, be at most 64
 *    characters, and not start with "sqlite_". Anything else is PERK_ERR_BADNAME.
 *  - Values are always passed as PerkValues and bound as parameters, never
 *    pasted into SQL.
 *  - where is a restricted filter, e.g. "age >= ? AND (name = ? OR name IS NULL)".
 *    It may contain column names, ? placeholders, = == != <> < <= > >=, ( ) ,
 *    and the keywords AND OR NOT IS NULL LIKE GLOB IN BETWEEN. Literals, quotes,
 *    ;, comments and functions are rejected: pass every value through args.
 *    The number of ?s must equal nargs. NULL or "" matches every row.
 *  - On failure, perk_errmsg() describes what went wrong.
 */

// On failure *out is NULL and perk_errmsg(NULL) holds the reason (per thread)
PerkErr perk_open(const char *path, PerkDB **out);

void perk_close(PerkDB *db);

// Message for the last error on db, or for the last failed perk_open() if db is NULL
const char *perk_errmsg(PerkDB *db);

// Creates a new table. Returns PERK_ERR_EXISTS (and leaves the existing schema untouched)
// if a table, view or index with that name already exists; names are compared
// case-insensitively, as SQLite does. Column type may be NULL or one of
// INTEGER, INT, REAL, TEXT, BLOB, NUMERIC. More than one PERK_COL_PK column makes a composite key.
PerkErr perk_create_table(PerkDB*, const char *table, const PerkColumn *cols, size_t ncols);

/*
 * Inserts one row. cols[i] receives vals[i].
 * @param rowid_out may be NULL; receives the new row's rowid
 * @return PERK_OK, PERK_ERR_MISUSE (NULL db/table/cols/vals or n == 0),
 *         PERK_ERR_BADNAME (invalid or "" table/column name), or PERK_ERR_SQL
 *         (e.g. missing table, constraint violation)
 */
PerkErr perk_insert(PerkDB*, const char *table, const char **cols, const PerkValue *vals, size_t n, int64_t *rowid_out);

/*
 * Reads rows. A NULL or "" where lists every row in the table (list-all); an empty
 * table or a filter that matches nothing is not an error and gives a 0-row result.
 * @param table table to read from
 * @param cols columns to return; NULL / ncols 0 selects every column
 * @param where optional filter (see the input rules above)
 * @param args values for the ?s in where (NULL when nargs is 0)
 * @param out receives the result; free it with perk_result_free(). NULL on failure.
 * @return PERK_OK, PERK_ERR_MISUSE (NULL db/table/out, bad where), PERK_ERR_BADNAME,
 *         or PERK_ERR_SQL (e.g. no such table)
 */
PerkErr perk_select (PerkDB*, const char *table, const char **cols, size_t ncols, const char *where, const PerkValue *args, size_t nargs, PerkResult **out);

/*
 * Reads the row(s) whose id_col equals *id. The id is bound, never put in the SQL.
 * If id_col is not unique, every matching row is returned.
 * @param table table to read from
 * @param cols columns to return; NULL / ncols 0 selects every column
 * @param id_col name of the key column, e.g. "id"
 * @param id key value; a PERK_NULL id matches no row
 * @param out receives the result (at least one row); NULL on failure
 * @return PERK_OK, PERK_ERR_NOT_FOUND if no row matches, PERK_ERR_MISUSE for NULL
 *         db/table/id_col/id/out, PERK_ERR_BADNAME, or PERK_ERR_SQL
 */
PerkErr perk_select_by_id(PerkDB*, const char *table, const char **cols, size_t ncols,
                          const char *id_col, const PerkValue *id, PerkResult **out);

/*
 * Sets set_cols = set_vals on every row matching where. A NULL or "" where updates every row.
 * @param changed may be NULL; receives the number of rows changed (0 on PERK_ERR_NOT_FOUND)
 * @return PERK_OK if at least one row changed, PERK_ERR_NOT_FOUND if no row matched
 *         (including an empty table), PERK_ERR_MISUSE, PERK_ERR_BADNAME, or PERK_ERR_SQL
 */
PerkErr perk_update (PerkDB*, const char *table, const char **set_cols, const PerkValue *set_vals, size_t nset,
                     const char *where, const PerkValue *args, size_t nargs, int64_t *changed);

/*
 * Deletes every row matching where. A NULL or "" where deletes every row.
 * @param changed may be NULL; receives the number of rows deleted (0 on PERK_ERR_NOT_FOUND)
 * @return PERK_OK if at least one row was deleted, PERK_ERR_NOT_FOUND if no row matched
 *         (including an empty table), PERK_ERR_MISUSE, PERK_ERR_BADNAME, or PERK_ERR_SQL
 */
PerkErr perk_delete (PerkDB*, const char *table, const char *where, const PerkValue *args, size_t nargs, int64_t *changed);

// Result accessors. Out-of-range indexes return NULL. Returned pointers stay valid
// until perk_result_free(); text values are NUL-terminated.
size_t           perk_result_rows(const PerkResult*);
size_t           perk_result_cols(const PerkResult*);
const char      *perk_result_colname(const PerkResult*, size_t col);
const PerkValue *perk_result_get(const PerkResult*, size_t row, size_t col);

void    perk_result_free(PerkResult*);

// perk_begin starts a write (IMMEDIATE) transaction. Nesting is PERK_ERR_MISUSE.
PerkErr perk_begin(PerkDB*); PerkErr perk_commit(PerkDB*); PerkErr perk_rollback(PerkDB*);

#endif
