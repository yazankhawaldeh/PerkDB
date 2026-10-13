#include "perk_internal.h"
#include <stdio.h>
#include <stdlib.h>

// perk_open() has no handle to store its error in when it fails, so the message for
// the last failed open on this thread is kept here and read with perk_errmsg(NULL).
static _Thread_local char open_errmsg[256];

static void set_open_error(const char *msg)
{
    snprintf(open_errmsg, sizeof(open_errmsg), "%s", msg ? msg : "");
}

PerkErr perk_open(const char *path, PerkDB **out)
{
    set_open_error("");
    if (out == NULL || path == NULL)
    {
        set_open_error("perk_open: path and out are required");
        return PERK_ERR_MISUSE;
    }
    *out = NULL;

    PerkDB *db = calloc(1, sizeof(*db));
    if (db == NULL)
    {
        set_open_error("out of memory");
        return PERK_ERR_NOMEM;
    }

    if (sqlite3_open(path, &db->conn) != SQLITE_OK)
    {
        set_open_error(db->conn ? sqlite3_errmsg(db->conn) : "out of memory");
        sqlite3_close(db->conn);
        free(db);
        return PERK_ERR_OPEN;
    }

    const char *pragmas =
        "PRAGMA journal_mode = WAL;"
        "PRAGMA busy_timeout = 5000;"
        "PRAGMA foreign_keys = ON;"
        "PRAGMA synchronous = NORMAL;";
    if (sqlite3_exec(db->conn, pragmas, NULL, NULL, NULL) != SQLITE_OK)
    {
        set_open_error(sqlite3_errmsg(db->conn));
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

    // Every function finalizes its statements, so this always succeeds;
    // an open transaction is rolled back.
    sqlite3_close(db->conn);
    free(db);
}

const char *perk_errmsg(PerkDB *db)
{
    if (db == NULL)
        return open_errmsg;
    return db->errmsg;
}
