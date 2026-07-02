#ifndef PERKDB_H
#define PERKDB_H



#include <stdint.h>
#include <stdlib.h>

typedef struct PerkDB PerkDB;
typedef enum {
    PERK_OK = 0,
    PERK_ERR_OPEN, PERK_ERR_SQL, PERK_ERR_BUSY,
    PERK_ERR_BADNAME, PERK_ERR_NOMEM
} PerkErr;

typedef struct { const char *name; const char *type; int flags; } PerkColumn;
typedef struct { } PerkValue;
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
