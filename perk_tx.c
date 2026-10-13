#include "perk_internal.h"

// SQLite's autocommit flag is the source of truth for whether a transaction is open,
// since SQLite can roll a transaction back on its own after some errors.

static PerkErr exec_tx(PerkDB *db, const char *sql)
{
    int rc = sqlite3_exec(db->conn, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK)
        return perk_sqlite_error(db, rc);
    return PERK_OK;
}

PerkErr perk_begin(PerkDB *db)
{
    if (db == NULL)
        return PERK_ERR_MISUSE;
    if (!sqlite3_get_autocommit(db->conn))
    {
        perk_set_error(db, "perk_begin: a transaction is already open");
        return PERK_ERR_MISUSE;
    }
    // IMMEDIATE takes the write lock up front, so a later write can't fail with BUSY mid-transaction
    return exec_tx(db, "BEGIN IMMEDIATE");
}

PerkErr perk_commit(PerkDB *db)
{
    if (db == NULL)
        return PERK_ERR_MISUSE;
    if (sqlite3_get_autocommit(db->conn))
    {
        perk_set_error(db, "perk_commit: no transaction is open");
        return PERK_ERR_MISUSE;
    }
    return exec_tx(db, "COMMIT");
}

PerkErr perk_rollback(PerkDB *db)
{
    if (db == NULL)
        return PERK_ERR_MISUSE;
    if (sqlite3_get_autocommit(db->conn))
    {
        perk_set_error(db, "perk_rollback: no transaction is open");
        return PERK_ERR_MISUSE;
    }
    return exec_tx(db, "ROLLBACK");
}
