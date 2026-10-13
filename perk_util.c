#include "perk_internal.h"
#include <limits.h>
#include <stdio.h>

int perk_is_ident_start(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

int perk_is_ident_char(char c)
{
    return perk_is_ident_start(c) || (c >= '0' && c <= '9');
}

int validate_identifier(const char *name)
{
    if (name == NULL || !perk_is_ident_start(name[0]))
        return 0;

    for (size_t len = 1; name[len] != '\0'; len++)
    {
        if (len >= PERK_MAX_IDENT || !perk_is_ident_char(name[len]))
            return 0;
    }

    // The sqlite_ prefix is reserved for SQLite's internal tables
    if (sqlite3_strnicmp(name, "sqlite_", 7) == 0)
        return 0;

    return 1;
}

void perk_set_error(PerkDB *db, const char *msg)
{
    snprintf(db->errmsg, sizeof(db->errmsg), "%s", msg ? msg : "");
}

PerkErr perk_sqlite_error(PerkDB *db, int rc)
{
    perk_set_error(db, sqlite3_errmsg(db->conn));
    switch (rc & 0xff)
    {
    case SQLITE_BUSY:
    case SQLITE_LOCKED:
        return PERK_ERR_BUSY;
    case SQLITE_NOMEM:
        return PERK_ERR_NOMEM;
    default:
        return PERK_ERR_SQL;
    }
}

PerkErr perk_finish_sql(PerkDB *db, sqlite3_str *s, char **out)
{
    int rc = sqlite3_str_errcode(s);
    *out = sqlite3_str_finish(s);
    if (rc == SQLITE_OK && *out != NULL)
        return PERK_OK;

    sqlite3_free(*out);
    *out = NULL;
    if (rc == SQLITE_TOOBIG)
    {
        perk_set_error(db, "generated SQL statement is too long");
        return PERK_ERR_SQL;
    }
    perk_set_error(db, "out of memory");
    return PERK_ERR_NOMEM;
}

void perk_discard_sql(sqlite3_str *s)
{
    sqlite3_free(sqlite3_str_finish(s));
}

PerkErr perk_bind_values(PerkDB *db, sqlite3_stmt *st, const PerkValue *vals, size_t n, int first_index)
{
    if (first_index < 1 || n > (size_t)(INT_MAX - first_index))
    {
        perk_set_error(db, "too many values to bind");
        return PERK_ERR_MISUSE;
    }
    for (size_t i = 0; i < n; i++)
    {
        const PerkValue *v = &vals[i];
        int idx = first_index + (int)i;
        int rc;

        switch (v->type)
        {
        case PERK_NULL:
            rc = sqlite3_bind_null(st, idx);
            break;
        case PERK_INT:
            rc = sqlite3_bind_int64(st, idx, v->as.i);
            break;
        case PERK_REAL:
            rc = sqlite3_bind_double(st, idx, v->as.r);
            break;
        case PERK_TEXT:
            if (v->as.text.ptr == NULL)
            {
                perk_set_error(db, "PERK_TEXT value has a NULL pointer");
                return PERK_ERR_MISUSE;
            }
            rc = sqlite3_bind_text64(st, idx, v->as.text.ptr, v->as.text.len,
                                     SQLITE_TRANSIENT, SQLITE_UTF8);
            break;
        case PERK_BLOB:
            if (v->as.blob.ptr == NULL && v->as.blob.len > 0)
            {
                perk_set_error(db, "PERK_BLOB value has a NULL pointer");
                return PERK_ERR_MISUSE;
            }
            if (v->as.blob.ptr == NULL)
                rc = sqlite3_bind_zeroblob64(st, idx, 0);
            else
                rc = sqlite3_bind_blob64(st, idx, v->as.blob.ptr, v->as.blob.len,
                                         SQLITE_TRANSIENT);
            break;
        default:
            perk_set_error(db, "PerkValue has an unknown type");
            return PERK_ERR_MISUSE;
        }

        if (rc != SQLITE_OK)
            return perk_sqlite_error(db, rc);
    }
    return PERK_OK;
}
