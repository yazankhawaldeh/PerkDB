#include "perk_internal.h"
#include <stdio.h>
#include <string.h>

// WHERE clauses are never passed through to SQLite as written. They are tokenized
// against a whitelist and rebuilt:
//   - words in where_keywords are emitted as SQL keywords
//   - any other word must be a valid identifier and is emitted as a quoted column name
//   - ? is a positional parameter; the number of ?s must equal nargs
//   - comparison operators, parentheses and commas are passed through
// Everything else (literals, quotes, ;, comments, ?NNN, :name, functions) is rejected.

static const char *const where_keywords[] = {
    "AND", "OR", "NOT", "IS", "NULL", "LIKE", "GLOB", "IN", "BETWEEN"
};

// Longest first so "<=" is matched before "<"
static const char *const where_operators[] = {
    "<=", ">=", "<>", "!=", "==", "=", "<", ">"
};

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static const char *match_keyword(const char *word)
{
    for (size_t i = 0; i < sizeof(where_keywords) / sizeof(where_keywords[0]); i++)
        if (sqlite3_stricmp(word, where_keywords[i]) == 0)
            return where_keywords[i];
    return NULL;
}

static PerkErr where_error(PerkDB *db, PerkErr err, const char *what, const char *where, const char *at)
{
    snprintf(db->errmsg, sizeof(db->errmsg), "where clause: %s at offset %zu",
             what, (size_t)(at - where));
    return err;
}

PerkErr perk_append_where(PerkDB *db, sqlite3_str *s, const char *where, size_t nargs)
{
    const char *p = where;
    if (p != NULL)
        while (is_space(*p))
            p++;

    if (p == NULL || *p == '\0')
    {
        if (nargs != 0)
        {
            perk_set_error(db, "where clause is empty but args were given");
            return PERK_ERR_MISUSE;
        }
        return PERK_OK;
    }

    size_t nparams = 0;
    size_t depth = 0;
    sqlite3_str_appendall(s, " WHERE");

    while (*p != '\0')
    {
        if (is_space(*p))
        {
            p++;
            continue;
        }

        if (perk_is_ident_start(*p))
        {
            const char *start = p;
            while (perk_is_ident_char(*p))
                p++;

            size_t len = (size_t)(p - start);
            if (len > PERK_MAX_IDENT)
                return where_error(db, PERK_ERR_BADNAME, "name too long", where, start);

            char word[PERK_MAX_IDENT + 1];
            memcpy(word, start, len);
            word[len] = '\0';

            const char *kw = match_keyword(word);
            if (kw != NULL)
                sqlite3_str_appendf(s, " %s", kw);
            else if (!validate_identifier(word))
                return where_error(db, PERK_ERR_BADNAME, "invalid column name", where, start);
            else
            {
                // SQLite treats "name"(...) as a function call, so a column name may not precede '('
                const char *next = p;
                while (is_space(*next))
                    next++;
                if (*next == '(')
                    return where_error(db, PERK_ERR_MISUSE, "function calls are not allowed", where, start);
                sqlite3_str_appendf(s, " \"%w\"", word);
            }
            continue;
        }

        switch (*p)
        {
        case '?':
            nparams++;
            sqlite3_str_appendall(s, " ?");
            p++;
            continue;
        case '(':
            depth++;
            sqlite3_str_appendall(s, " (");
            p++;
            continue;
        case ')':
            if (depth == 0)
                return where_error(db, PERK_ERR_MISUSE, "unbalanced ')'", where, p);
            depth--;
            sqlite3_str_appendall(s, " )");
            p++;
            continue;
        case ',':
            sqlite3_str_appendall(s, " ,");
            p++;
            continue;
        default:
            break;
        }

        const char *op = NULL;
        for (size_t i = 0; i < sizeof(where_operators) / sizeof(where_operators[0]); i++)
        {
            size_t oplen = strlen(where_operators[i]);
            if (strncmp(p, where_operators[i], oplen) == 0)
            {
                op = where_operators[i];
                p += oplen;
                break;
            }
        }
        if (op == NULL)
            return where_error(db, PERK_ERR_MISUSE,
                               "unsupported character (use ? for values)", where, p);
        sqlite3_str_appendf(s, " %s", op);
    }

    if (depth != 0)
    {
        perk_set_error(db, "where clause: unbalanced '('");
        return PERK_ERR_MISUSE;
    }
    if (nparams != nargs)
    {
        snprintf(db->errmsg, sizeof(db->errmsg),
                 "where clause has %zu ? placeholders but %zu args were given", nparams, nargs);
        return PERK_ERR_MISUSE;
    }
    return PERK_OK;
}
