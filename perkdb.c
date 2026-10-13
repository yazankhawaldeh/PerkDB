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

/*
 * Locks the connection down. PerkDB only ever runs SQL it generated itself, so
 * nothing below takes away a feature the API exposes:
 *  - DEFENSIVE: no writable_schema or other ways to corrupt the database file
 *  - load_extension() disabled (both the C API and the SQL function)
 *  - TRUSTED_SCHEMA off: views/triggers in an opened file can't call risky functions
 *  - DQS off: "..." is always an identifier, never a string literal
 *  - ATTACH limited to 0 databases, so no other file can be reached
 * @param conn freshly opened connection
 * @return SQLITE_OK, or the first sqlite3_db_config error
 */
static int harden_connection(sqlite3 *conn)
{
    static const int on_off[][2] = {
        { SQLITE_DBCONFIG_DEFENSIVE, 1 },
        { SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0 },
        { SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0 },
        { SQLITE_DBCONFIG_DQS_DML, 0 },
        { SQLITE_DBCONFIG_DQS_DDL, 0 },
    };
    for (size_t i = 0; i < sizeof(on_off) / sizeof(on_off[0]); i++)
    {
        int rc = sqlite3_db_config(conn, on_off[i][0], on_off[i][1], (int *)NULL);
        if (rc != SQLITE_OK)
            return rc;
    }
    sqlite3_limit(conn, SQLITE_LIMIT_ATTACHED, 0);
    return SQLITE_OK;
}

PerkErr perk_open(const char *path, PerkDB **out)
{
    set_open_error("");
    if (out != NULL)
    {
        *out = NULL;
    }
    if (out == NULL || path == NULL)
    {
        set_open_error("perk_open: path and out are required");
        return PERK_ERR_MISUSE;
    }

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

    if (harden_connection(db->conn) != SQLITE_OK)
    {
        set_open_error("perk_open: could not apply connection safety settings");
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
