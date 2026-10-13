#include "perk_internal.h"
#include <stdint.h>
#include <string.h>

static void free_value(PerkValue *v)
{
    if (v->type == PERK_TEXT)
        free((void *)v->as.text.ptr);
    else if (v->type == PERK_BLOB)
        free((void *)v->as.blob.ptr);
}

void perk_result_free(PerkResult *res)
{
    if (res == NULL)
        return;

    if (res->cells != NULL)
        for (size_t i = 0; i < res->nrows * res->ncols; i++)
            free_value(&res->cells[i]);
    free(res->cells);

    if (res->col_names != NULL)
        for (size_t i = 0; i < res->ncols; i++)
            free(res->col_names[i]);
    free(res->col_names);

    free(res);
}

// Copies len bytes plus a NUL terminator
static char *copy_text(const void *src, size_t len)
{
    char *copy = malloc(len + 1);
    if (copy == NULL)
        return NULL;
    memcpy(copy, src, len);
    copy[len] = '\0';
    return copy;
}

// Copies column c of the current row into v. v must be zeroed (PERK_NULL) on entry
// so that it stays safe to free if this fails.
static int copy_column(sqlite3_stmt *st, int c, PerkValue *v)
{
    switch (sqlite3_column_type(st, c))
    {
    case SQLITE_INTEGER:
        *v = perk_int(sqlite3_column_int64(st, c));
        return 1;
    case SQLITE_FLOAT:
        *v = perk_real(sqlite3_column_double(st, c));
        return 1;
    case SQLITE_TEXT:
    {
        // column_text must be called before column_bytes
        const unsigned char *text = sqlite3_column_text(st, c);
        int len = sqlite3_column_bytes(st, c);
        if (text == NULL || len < 0)
            return 0;
        char *copy = copy_text(text, (size_t)len);
        if (copy == NULL)
            return 0;
        v->type = PERK_TEXT;
        v->as.text.ptr = copy;
        v->as.text.len = (size_t)len;
        return 1;
    }
    case SQLITE_BLOB:
    {
        const void *blob = sqlite3_column_blob(st, c);
        int len = sqlite3_column_bytes(st, c);
        if (len < 0 || (blob == NULL && len > 0))
            return 0;
        if (len == 0)
        {
            *v = perk_blob(NULL, 0);
            return 1;
        }
        void *copy = malloc((size_t)len);
        if (copy == NULL)
            return 0;
        memcpy(copy, blob, (size_t)len);
        *v = perk_blob(copy, (size_t)len);
        return 1;
    }
    default:
        *v = perk_null();
        return 1;
    }
}

PerkErr perk_result_from_stmt(PerkDB *db, sqlite3_stmt *st, PerkResult **out)
{
    *out = NULL;

    int ncols = sqlite3_column_count(st);
    if (ncols <= 0)
    {
        perk_set_error(db, "statement returns no columns");
        return PERK_ERR_MISUSE;
    }

    PerkResult *res = calloc(1, sizeof(*res));
    if (res == NULL)
        goto nomem;
    res->ncols = (size_t)ncols;

    res->col_names = calloc(res->ncols, sizeof(*res->col_names));
    if (res->col_names == NULL)
        goto nomem;
    for (int c = 0; c < ncols; c++)
    {
        const char *name = sqlite3_column_name(st, c);
        if (name == NULL)
            goto nomem;
        res->col_names[c] = copy_text(name, strlen(name));
        if (res->col_names[c] == NULL)
            goto nomem;
    }

    size_t cap = 0;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW)
    {
        if (res->nrows == cap)
        {
            size_t new_cap = cap ? cap * 2 : 16;
            if (new_cap < cap || new_cap > SIZE_MAX / sizeof(PerkValue) / res->ncols)
                goto nomem;
            PerkValue *cells = realloc(res->cells, new_cap * res->ncols * sizeof(PerkValue));
            if (cells == NULL)
                goto nomem;
            res->cells = cells;
            cap = new_cap;
        }

        // Zero the row and count it before filling, so a partial row frees cleanly
        PerkValue *row = &res->cells[res->nrows * res->ncols];
        memset(row, 0, res->ncols * sizeof(PerkValue));
        res->nrows++;

        for (int c = 0; c < ncols; c++)
            if (!copy_column(st, c, &row[c]))
                goto nomem;
    }

    if (rc != SQLITE_DONE)
    {
        PerkErr err = perk_sqlite_error(db, rc);
        perk_result_free(res);
        return err;
    }

    *out = res;
    return PERK_OK;

nomem:
    perk_result_free(res);
    perk_set_error(db, "out of memory");
    return PERK_ERR_NOMEM;
}

size_t perk_result_rows(const PerkResult *res)
{
    return res ? res->nrows : 0;
}

size_t perk_result_cols(const PerkResult *res)
{
    return res ? res->ncols : 0;
}

const char *perk_result_colname(const PerkResult *res, size_t col)
{
    if (res == NULL || col >= res->ncols)
        return NULL;
    return res->col_names[col];
}

const PerkValue *perk_result_get(const PerkResult *res, size_t row, size_t col)
{
    if (res == NULL || row >= res->nrows || col >= res->ncols)
        return NULL;
    return &res->cells[row * res->ncols + col];
}
