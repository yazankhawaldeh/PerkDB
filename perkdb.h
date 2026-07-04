#ifndef PERKDB_H
#define PERKDB_H



#include <stdint.h>
#include <stdlib.h>


// This is defined in perkdb.c
typedef struct PerkDB PerkDB;


typedef enum {
    PERK_OK = 0,
    PERK_ERR_OPEN, PERK_ERR_SQL, PERK_ERR_BUSY,
    PERK_ERR_BADNAME, PERK_ERR_NOMEM
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

static inline PerkValue perk_int(int64_t v);
static inline PerkValue perk_real(double v);
static inline PerkValue perk_text(const char *v);
static inline PerkValue perk_blob(const void *p, size_t n);
static inline PerkValue perk_null(void);


typedef struct PerkResult PerkResult;

PerkErr perk_open(const char *path, PerkDB **out);

void perk_close(PerkDB *db);

const char *perk_errmsg(PerkDB *db);

PerkErr perk_create_table(PerkDB*, const char *table, const PerkColumn *cols, size_t ncols);

PerkErr perk_insert(PerkDB*, const char *table, const char **cols, const PerkValue *vals, size_t n, int64_t *rowid_out);

PerkErr perk_select (PerkDB*, const char *table, const char **cols, size_t ncols, const char *where, const PerkValue *args, size_t nargs, PerkResult **out);

PerkErr perk_update (PerkDB*, const char *table, const char **set_cols, const PerkValue *set_vals, size_t nset,
                     const char *where, const PerkValue *args, size_t nargs, int64_t *changed);

PerkErr perk_delete (PerkDB*, const char *table, const char *where, const PerkValue *args, size_t nargs, int64_t *changed);

void    perk_result_free(PerkResult*);

PerkErr perk_begin(PerkDB*); PerkErr perk_commit(PerkDB*); PerkErr perk_rollback(PerkDB*);

#endif
