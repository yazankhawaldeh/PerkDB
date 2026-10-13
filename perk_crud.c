#include "perk_internal.h"
#include <stdio.h>

// Injection safety: table/column names must pass validate_identifier() and are
// then double-quoted; column types come from a fixed whitelist; WHERE clauses are
// rebuilt by perk_append_where(); values are never put in the SQL text, only bound
// as parameters.

static PerkErr check_names(PerkDB *db, const char *fn, const char *table, const char **cols, size_t n)
{
    if (!validate_identifier(table))
    {
        snprintf(db->errmsg, sizeof(db->errmsg), "%s: invalid table name", fn);
        return PERK_ERR_BADNAME;
    }
    for (size_t i = 0; i < n; i++)
    {
        if (!validate_identifier(cols[i]))
        {
            snprintf(db->errmsg, sizeof(db->errmsg), "%s: invalid column name at index %zu", fn, i);
            return PERK_ERR_BADNAME;
        }
    }
    return PERK_OK;
}

// Finishes s (always freeing it) and prepares the result into *st
static PerkErr prepare_sql(PerkDB *db, sqlite3_str *s, sqlite3_stmt **st)
{
    char *sql = NULL;
    PerkErr err = perk_finish_sql(db, s, &sql);
    if (err != PERK_OK)
        return err;

    int rc = sqlite3_prepare_v2(db->conn, sql, -1, st, NULL);
    sqlite3_free(sql);
    if (rc != SQLITE_OK)
        return perk_sqlite_error(db, rc);
    return PERK_OK;
}

// Steps a statement that returns no rows, then finalizes it
static PerkErr run_write(PerkDB *db, sqlite3_stmt *st, int64_t *changed)
{
    PerkErr err = PERK_OK;
    int rc = sqlite3_step(st);
    if (rc != SQLITE_DONE)
        err = perk_sqlite_error(db, rc);
    else if (changed != NULL)
        *changed = sqlite3_changes64(db->conn);
    sqlite3_finalize(st);
    return err;
}

static const char *const column_types[] = {
    "INTEGER", "INT", "REAL", "TEXT", "BLOB", "NUMERIC"
};

static const char *match_column_type(const char *type)
{
    for (size_t i = 0; i < sizeof(column_types) / sizeof(column_types[0]); i++)
        if (sqlite3_stricmp(type, column_types[i]) == 0)
            return column_types[i];
    return NULL;
}

/*
 * Looks up a schema object (table, view, index or trigger) in the main database
 * by name, compared case-insensitively like SQLite does. The name is bound, never
 * formatted into the SQL.
 * @param db open handle
 * @param name name to look for
 * @param type_out receives the object type ("table", "index", ...) when found
 * @param type_cap size of type_out
 * @param found set to 1 if an object with that name exists, 0 otherwise
 * @return PERK_OK, or the mapped SQLite error if the lookup itself failed
 */
static PerkErr find_schema_object(PerkDB *db, const char *name, char *type_out, size_t type_cap, int *found)
{
    *found = 0;
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db->conn,
                                "SELECT type FROM main.sqlite_master WHERE name = ?1 COLLATE NOCASE LIMIT 1",
                                -1, &st, NULL);
    if (rc != SQLITE_OK)
        return perk_sqlite_error(db, rc);
    rc = sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK)
        rc = sqlite3_step(st);

    PerkErr err = PERK_OK;
    if (rc == SQLITE_ROW)
    {
        const unsigned char *type = sqlite3_column_text(st, 0);
        snprintf(type_out, type_cap, "%s", type ? (const char *)type : "object");
        *found = 1;
    }
    else if (rc != SQLITE_DONE)
        err = perk_sqlite_error(db, rc);
    sqlite3_finalize(st);
    return err;
}

/*
 * Checks whether table's name is already taken and, if so, sets the error message.
 * @param db open handle
 * @param table validated table name
 * @return PERK_ERR_EXISTS if the name is taken, PERK_OK if it is free, or the
 *         lookup's error
 */
static PerkErr check_table_absent(PerkDB *db, const char *table)
{
    char type[16];
    int found = 0;
    PerkErr err = find_schema_object(db, table, type, sizeof(type), &found);
    if (err != PERK_OK)
        return err;
    if (!found)
        return PERK_OK;
    snprintf(db->errmsg, sizeof(db->errmsg),
             "perk_create_table: a %s named \"%s\" already exists", type, table);
    return PERK_ERR_EXISTS;
}

PerkErr perk_create_table(PerkDB *db, const char *table, const PerkColumn *cols, size_t ncols)
{
    if (db == NULL)
        return PERK_ERR_MISUSE;
    if (table == NULL || cols == NULL || ncols == 0)
    {
        perk_set_error(db, "perk_create_table: table and cols are required and ncols must be > 0");
        return PERK_ERR_MISUSE;
    }
    if (!validate_identifier(table))
    {
        perk_set_error(db, "perk_create_table: invalid table name");
        return PERK_ERR_BADNAME;
    }

    const int known_flags = PERK_COL_PK | PERK_COL_NOTNULL | PERK_COL_UNIQUE;
    size_t npk = 0;
    for (size_t i = 0; i < ncols; i++)
    {
        if (!validate_identifier(cols[i].name))
        {
            snprintf(db->errmsg, sizeof(db->errmsg),
                     "perk_create_table: invalid column name at index %zu", i);
            return PERK_ERR_BADNAME;
        }
        if (cols[i].type != NULL && match_column_type(cols[i].type) == NULL)
        {
            snprintf(db->errmsg, sizeof(db->errmsg),
                     "perk_create_table: unsupported type at index %zu "
                     "(use INTEGER, INT, REAL, TEXT, BLOB, NUMERIC or NULL)", i);
            return PERK_ERR_BADNAME;
        }
        if (cols[i].flags & ~known_flags)
        {
            snprintf(db->errmsg, sizeof(db->errmsg),
                     "perk_create_table: unknown flags at index %zu", i);
            return PERK_ERR_MISUSE;
        }
        if (cols[i].flags & PERK_COL_PK)
            npk++;
    }

    PerkErr err = check_table_absent(db, table);
    if (err != PERK_OK)
        return err;

    sqlite3_str *s = sqlite3_str_new(db->conn);
    sqlite3_str_appendf(s, "CREATE TABLE \"%w\" (", table);
    for (size_t i = 0; i < ncols; i++)
    {
        sqlite3_str_appendf(s, "%s\"%w\"", i ? ", " : "", cols[i].name);
        if (cols[i].type != NULL)
            sqlite3_str_appendf(s, " %s", match_column_type(cols[i].type));
        // A single PK stays a column constraint so INTEGER PRIMARY KEY aliases the rowid
        if (npk == 1 && (cols[i].flags & PERK_COL_PK))
            sqlite3_str_appendall(s, " PRIMARY KEY");
        if (cols[i].flags & PERK_COL_NOTNULL)
            sqlite3_str_appendall(s, " NOT NULL");
        if (cols[i].flags & PERK_COL_UNIQUE)
            sqlite3_str_appendall(s, " UNIQUE");
    }
    if (npk > 1)
    {
        sqlite3_str_appendall(s, ", PRIMARY KEY (");
        size_t written = 0;
        for (size_t i = 0; i < ncols; i++)
            if (cols[i].flags & PERK_COL_PK)
                sqlite3_str_appendf(s, "%s\"%w\"", written++ ? ", " : "", cols[i].name);
        sqlite3_str_appendchar(s, 1, ')');
    }
    sqlite3_str_appendchar(s, 1, ')');

    sqlite3_stmt *st = NULL;
    err = prepare_sql(db, s, &st);
    if (err == PERK_OK)
        err = run_write(db, st, NULL);
    // Another connection may have created the name after the check above
    if (err == PERK_ERR_SQL)
    {
        char create_msg[sizeof(db->errmsg)];
        memcpy(create_msg, db->errmsg, sizeof(create_msg));
        if (check_table_absent(db, table) == PERK_ERR_EXISTS)
            return PERK_ERR_EXISTS;
        memcpy(db->errmsg, create_msg, sizeof(create_msg));  // keep the CREATE failure's message
    }
    return err;
}

PerkErr perk_insert(PerkDB *db, const char *table, const char **cols, const PerkValue *vals, size_t n, int64_t *rowid_out)
{
    if (db == NULL)
        return PERK_ERR_MISUSE;
    if (table == NULL || cols == NULL || vals == NULL || n == 0)
    {
        perk_set_error(db, "perk_insert: table, cols and vals are required and n must be > 0");
        return PERK_ERR_MISUSE;
    }
    PerkErr err = check_names(db, "perk_insert", table, cols, n);
    if (err != PERK_OK)
        return err;

    sqlite3_str *s = sqlite3_str_new(db->conn);
    sqlite3_str_appendf(s, "INSERT INTO \"%w\" (", table);
    for (size_t i = 0; i < n; i++)
        sqlite3_str_appendf(s, "%s\"%w\"", i ? ", " : "", cols[i]);
    sqlite3_str_appendall(s, ") VALUES (");
    for (size_t i = 0; i < n; i++)
        sqlite3_str_appendall(s, i ? ", ?" : "?");
    sqlite3_str_appendchar(s, 1, ')');

    sqlite3_stmt *st = NULL;
    err = prepare_sql(db, s, &st);
    if (err != PERK_OK)
        return err;

    err = perk_bind_values(db, st, vals, n, 1);
    if (err != PERK_OK)
    {
        sqlite3_finalize(st);
        return err;
    }
    err = run_write(db, st, NULL);
    if (err == PERK_OK && rowid_out != NULL)
        *rowid_out = sqlite3_last_insert_rowid(db->conn);
    return err;
}

PerkErr perk_select(PerkDB *db, const char *table, const char **cols, size_t ncols, const char *where, const PerkValue *args, size_t nargs, PerkResult **out)
{
    if (out != NULL)
        *out = NULL;
    if (db == NULL)
        return PERK_ERR_MISUSE;
    if (table == NULL || out == NULL || (ncols > 0 && cols == NULL) || (nargs > 0 && args == NULL))
    {
        perk_set_error(db, "perk_select: table and out are required, and cols/args must be set when their counts are > 0");
        return PERK_ERR_MISUSE;
    }
    PerkErr err = check_names(db, "perk_select", table, cols, ncols);
    if (err != PERK_OK)
        return err;

    sqlite3_str *s = sqlite3_str_new(db->conn);
    sqlite3_str_appendall(s, "SELECT ");
    if (ncols == 0)
        sqlite3_str_appendchar(s, 1, '*');
    for (size_t i = 0; i < ncols; i++)
        sqlite3_str_appendf(s, "%s\"%w\"", i ? ", " : "", cols[i]);
    sqlite3_str_appendf(s, " FROM \"%w\"", table);

    err = perk_append_where(db, s, where, nargs);
    if (err != PERK_OK)
    {
        perk_discard_sql(s);
        return err;
    }

    sqlite3_stmt *st = NULL;
    err = prepare_sql(db, s, &st);
    if (err != PERK_OK)
        return err;

    err = perk_bind_values(db, st, args, nargs, 1);
    if (err == PERK_OK)
        err = perk_result_from_stmt(db, st, out);
    sqlite3_finalize(st);
    return err;
}

PerkErr perk_update(PerkDB *db, const char *table, const char **set_cols, const PerkValue *set_vals, size_t nset,
                    const char *where, const PerkValue *args, size_t nargs, int64_t *changed)
{
    if (db == NULL)
        return PERK_ERR_MISUSE;
    if (table == NULL || set_cols == NULL || set_vals == NULL || nset == 0 || (nargs > 0 && args == NULL))
    {
        perk_set_error(db, "perk_update: table, set_cols and set_vals are required, nset must be > 0, and args must be set when nargs > 0");
        return PERK_ERR_MISUSE;
    }
    PerkErr err = check_names(db, "perk_update", table, set_cols, nset);
    if (err != PERK_OK)
        return err;

    sqlite3_str *s = sqlite3_str_new(db->conn);
    sqlite3_str_appendf(s, "UPDATE \"%w\" SET ", table);
    for (size_t i = 0; i < nset; i++)
        sqlite3_str_appendf(s, "%s\"%w\" = ?", i ? ", " : "", set_cols[i]);

    err = perk_append_where(db, s, where, nargs);
    if (err != PERK_OK)
    {
        perk_discard_sql(s);
        return err;
    }

    sqlite3_stmt *st = NULL;
    err = prepare_sql(db, s, &st);
    if (err != PERK_OK)
        return err;

    // SET values take parameters 1..nset, WHERE args follow. prepare_sql has already
    // rejected statements with more parameters than SQLite allows, so nset fits in an int.
    err = perk_bind_values(db, st, set_vals, nset, 1);
    if (err == PERK_OK)
        err = perk_bind_values(db, st, args, nargs, (int)nset + 1);
    if (err != PERK_OK)
    {
        sqlite3_finalize(st);
        return err;
    }
    return run_write(db, st, changed);
}

PerkErr perk_delete(PerkDB *db, const char *table, const char *where, const PerkValue *args, size_t nargs, int64_t *changed)
{
    if (db == NULL)
        return PERK_ERR_MISUSE;
    if (table == NULL || (nargs > 0 && args == NULL))
    {
        perk_set_error(db, "perk_delete: table is required and args must be set when nargs > 0");
        return PERK_ERR_MISUSE;
    }
    PerkErr err = check_names(db, "perk_delete", table, NULL, 0);
    if (err != PERK_OK)
        return err;

    sqlite3_str *s = sqlite3_str_new(db->conn);
    sqlite3_str_appendf(s, "DELETE FROM \"%w\"", table);

    err = perk_append_where(db, s, where, nargs);
    if (err != PERK_OK)
    {
        perk_discard_sql(s);
        return err;
    }

    sqlite3_stmt *st = NULL;
    err = prepare_sql(db, s, &st);
    if (err != PERK_OK)
        return err;

    err = perk_bind_values(db, st, args, nargs, 1);
    if (err != PERK_OK)
    {
        sqlite3_finalize(st);
        return err;
    }
    return run_write(db, st, changed);
}
