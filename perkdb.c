#include "perkdb.h"
#include "sqlite3-src/sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
struct PerkDB
{
    sqlite3 *conn;
    char errmsg[256];
    int in_transaction;
};

struct PerkResult
{
    size_t nrows, ncols;
    char **col_names;
    PerkValue *cells;
};

PerkErr perk_open(const char *path, PerkDB **out)
{
    if (out == NULL || path == NULL)
    {
        return PERK_ERR_OPEN;
    }
    *out = NULL;

    PerkDB *db = calloc(1, sizeof(*db));
    if (db == NULL)
        return PERK_ERR_NOMEM;

    if (sqlite3_open(path, &db->conn) != SQLITE_OK)
    {
        sqlite3_close(db->conn);
        free(db);
        return PERK_ERR_OPEN;
    }

    const char *pragmas = 
        "PRAGMA journal_mode = WAL;"
        "PRAGMA busy_timeout = 5000;"
        "PRAGMA foreign_keys = ON"
        "PRAGMA synchronous = NORMAL";
    if (sqlite3_exec(db->conn, pragmas, NULL, NULL, NULL) != SQLITE_OK)
    {
        snprintf(db->errmsg, sizeof(db->errmsg), "%s", sqlite3_errmsg(db->conn));
        sqlite3_close(db->conn);
        free(db);
        return PERK_ERR_OPEN;
    }
    *out = db;
    return PERK_OK;
}


void perk_close(PerkDB *db)
{
    if (db == NULL)
    {
        return;
    }

    sqlite3_close(db->conn);
    free(db);
}

PerkErr perk_insert(PerkDB *db, const char *table, const char **cols, const PerkValue *vals, size_t n, int64_t *rowid_out)
{
    if (db == NULL || table == NULL || cols == NULL || vals == NULL || n == 0)
    {
        return PERK_ERR_SQL;
    }
    if (!validate_identifier(table))
        return PERK_ERR_BADNAME;
    for (size_t i = 0; i < n; i++)
        if(!validate_identifier(cols[i]))
            return PERK_ERR_BADNAME;
    char sql[1024];
    size_t off = (size_t)snprintf(sql, sizeof(sql), "INSERT INTO %s (", table);
    for (size_t i = 0; i < n; i++)
        off += (size_t)snprintf(sql + off, sizeof(sql) - off, "%s%s",
                i ? ", " : "", cols[i]);
    off += (size_t)snprintf(sql + off, sizeof(sql) - off, ") VALUES (");
    for (size_t i = 0; i < n; i++)
        off += (size_t)snprintf(sql + off, sizeof(sql) - off, "%s?",
                i ? ", " : "");
    off += (size_t)snprintf(sql + off, sizeof sql - off, ")");
    if (off >= sizeof sql)
        return PERK_ERR_SQL;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare16_v2(sqlite3 *db, const void *zSql, int nByte, sqlite3_stmt **ppStmt, const void **pzTail)


                
}
