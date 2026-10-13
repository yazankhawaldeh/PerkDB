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
    PERK_ERR_MISUSE  // NULL handle, missing arguments, or a malformed PerkValue
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

// Creates the table if it doesn't already exist. Column type may be NULL or one of
// INTEGER, INT, REAL, TEXT, BLOB, NUMERIC. More than one PERK_COL_PK column makes a composite key.
PerkErr perk_create_table(PerkDB*, const char *table, const PerkColumn *cols, size_t ncols);

// rowid_out may be NULL
PerkErr perk_insert(PerkDB*, const char *table, const char **cols, const PerkValue *vals, size_t n, int64_t *rowid_out);

// cols NULL / ncols 0 selects every column. Free *out with perk_result_free().
PerkErr perk_select (PerkDB*, const char *table, const char **cols, size_t ncols, const char *where, const PerkValue *args, size_t nargs, PerkResult **out);

// changed may be NULL. A NULL where updates every row.
PerkErr perk_update (PerkDB*, const char *table, const char **set_cols, const PerkValue *set_vals, size_t nset,
                     const char *where, const PerkValue *args, size_t nargs, int64_t *changed);

// changed may be NULL. A NULL where deletes every row.
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
