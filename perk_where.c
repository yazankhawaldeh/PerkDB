#include "perk_internal.h"
#include <stdio.h>
#include <string.h>

// WHERE clauses are never passed through to SQLite as written. They are tokenized
// against a whitelist and rebuilt, so the SQL that reaches SQLite only ever contains
// tokens this file emitted itself.
//
// Accepted grammar (tokens may be separated by space, tab, CR or LF only):
//   word      [A-Za-z_][A-Za-z0-9_]*, at most 64 characters (ASCII only)
//               - AND OR NOT IS NULL LIKE GLOB IN BETWEEN (any case) are emitted as keywords
//               - any other word must pass validate_identifier() (so no sqlite_ prefix)
//                 and is emitted as a double-quoted column name; it may not be
//                 followed by '(' (that would be a function call)
//   ?         positional parameter; the number of ?s must equal nargs
//   operator  = == != <> < <= > >=
//   ( ) ,     parentheses must balance
// Extra rules that stop the whitelist from being combined into something unsafe:
//   - IN must be followed by '(' : "x IN tbl" would read another table
//   - LIKE / GLOB must follow an operand (or NOT after an operand): at the start of
//     an expression SQLite parses "like(...)" / "glob(...)" as function calls
// Everything else is rejected: string/number/blob literals, quotes of any kind,
// ;, -- and /* */ comments, backslashes, ., ||, ?NNN, :name, @name, $name, other
// whitespace (\v, \f, non-ASCII), and any byte >= 0x80. Because SELECT, PRAGMA,
// ATTACH, etc. are not keywords here they can only ever become quoted column
// names, so subqueries, extra statements and other schema objects are unreachable.

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

// Kind of the previously emitted token, used to enforce the IN / LIKE / GLOB rules
typedef enum
{
    TOK_START,             // nothing yet, or '(' ',' an operator or a keyword
    TOK_OPERAND,           // column name, ?, ')' or NULL: an expression just ended
    TOK_NOT_AFTER_OPERAND, // NOT directly after an operand, as in "x NOT LIKE ?"
    TOK_IN                 // IN: the next token must be '('
} PrevTok;

/*
 * Skips the whitespace accepted by the WHERE grammar.
 * @param p position in the WHERE string
 * @return first non-space position at or after p
 */
static const char *skip_space(const char *p)
{
    while (is_space(*p))
        p++;
    return p;
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

/*
 * Validates a caller-supplied WHERE clause against the grammar above and appends
 * the rebuilt clause (" WHERE ...") to s. Nothing is appended for a NULL/blank where.
 * @param db open handle (receives the error message)
 * @param s SQL builder to append to; the caller frees it on failure
 * @param where filter text with ? placeholders, or NULL
 * @param nargs number of values the caller will bind; must equal the number of ?s
 * @return PERK_OK, PERK_ERR_BADNAME for an invalid or too-long column name, or
 *         PERK_ERR_MISUSE for anything outside the grammar or a ? count mismatch
 */
PerkErr perk_append_where(PerkDB *db, sqlite3_str *s, const char *where, size_t nargs)
{
    const char *p = where == NULL ? NULL : skip_space(where);

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
    PrevTok prev = TOK_START;
    sqlite3_str_appendall(s, " WHERE");

    while (*p != '\0')
    {
        if (is_space(*p))
        {
            p++;
            continue;
        }

        if (prev == TOK_IN && *p != '(')
            return where_error(db, PERK_ERR_MISUSE, "IN must be followed by '('", where, p);

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
            {
                // LIKE / GLOB in operand position would be parsed as a function call
                if ((strcmp(kw, "LIKE") == 0 || strcmp(kw, "GLOB") == 0) &&
                    prev != TOK_OPERAND && prev != TOK_NOT_AFTER_OPERAND)
                    return where_error(db, PERK_ERR_MISUSE,
                                       "LIKE/GLOB must follow a column or ? (function calls are not allowed)",
                                       where, start);
                sqlite3_str_appendf(s, " %s", kw);
                if (strcmp(kw, "IN") == 0)
                    prev = TOK_IN;
                else if (strcmp(kw, "NULL") == 0)
                    prev = TOK_OPERAND;
                else if (strcmp(kw, "NOT") == 0 && prev == TOK_OPERAND)
                    prev = TOK_NOT_AFTER_OPERAND;
                else
                    prev = TOK_START;
            }
            else if (!validate_identifier(word))
                return where_error(db, PERK_ERR_BADNAME, "invalid column name", where, start);
            else
            {
                // SQLite treats "name"(...) as a function call, so a column name may not precede '('
                if (*skip_space(p) == '(')
                    return where_error(db, PERK_ERR_MISUSE, "function calls are not allowed", where, start);
                sqlite3_str_appendf(s, " \"%w\"", word);
                prev = TOK_OPERAND;
            }
            continue;
        }

        switch (*p)
        {
        case '?':
            nparams++;
            sqlite3_str_appendall(s, " ?");
            p++;
            prev = TOK_OPERAND;
            continue;
        case '(':
            depth++;
            sqlite3_str_appendall(s, " (");
            p++;
            prev = TOK_START;
            continue;
        case ')':
            if (depth == 0)
                return where_error(db, PERK_ERR_MISUSE, "unbalanced ')'", where, p);
            depth--;
            sqlite3_str_appendall(s, " )");
            p++;
            prev = TOK_OPERAND;
            continue;
        case ',':
            sqlite3_str_appendall(s, " ,");
            p++;
            prev = TOK_START;
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
        prev = TOK_START;
    }

    if (prev == TOK_IN)
    {
        perk_set_error(db, "where clause: IN must be followed by '('");
        return PERK_ERR_MISUSE;
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
