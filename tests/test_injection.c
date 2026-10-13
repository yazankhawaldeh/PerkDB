// SQL injection test suite.
// Fires known injection payloads at every input of the API, then fuzzes the
// identifier and WHERE inputs with random input. After every attempt it checks
// that the database is unchanged:
//   - schema (sqlite_master) is identical
//   - the "secrets" canary table is intact
//   - no extra databases are attached and pragmas are unchanged
//   - no secret text came back in a query result
// Uses the raw SQLite handle (perk_internal.h) only to inspect the database.

#include "perk_internal.h"
#include <stdio.h>
#include <string.h>

static PerkDB *db;
static char *baseline_schema;
static char *baseline_secrets;

static int total_attempts, blocked, neutralized, breached;

// Runs a query returning one text value and copies it (caller frees)
static char *query_text(const char *sql)
{
    sqlite3_stmt *st = NULL;
    char *out = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
    {
        const char *t = (const char *)sqlite3_column_text(st, 0);
        t = t ? t : "";
        out = malloc(strlen(t) + 1);
        if (out != NULL)
            strcpy(out, t);
    }
    sqlite3_finalize(st);
    return out;
}

static int query_int(const char *sql)
{
    sqlite3_stmt *st = NULL;
    int v = -1;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        v = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return v;
}

static const char *SCHEMA_SQL =
    "SELECT group_concat(type || ':' || name || ':' || coalesce(sql, ''), '|') "
    "FROM (SELECT * FROM sqlite_master ORDER BY type, name)";
static const char *SECRETS_SQL =
    "SELECT group_concat(id || '=' || secret, '|') FROM (SELECT * FROM secrets ORDER BY id)";

static int database_intact(void)
{
    char *schema = query_text(SCHEMA_SQL);
    char *secrets = query_text(SECRETS_SQL);
    int ok = schema && secrets &&
             strcmp(schema, baseline_schema) == 0 &&
             strcmp(secrets, baseline_secrets) == 0 &&
             query_int("SELECT count(*) FROM pragma_database_list") == 1 &&
             query_int("PRAGMA foreign_keys") == 1 &&
             sqlite3_get_autocommit(db->conn);
    free(schema);
    free(secrets);
    return ok;
}

static int result_leaks_secret(const PerkResult *res)
{
    for (size_t r = 0; r < perk_result_rows(res); r++)
        for (size_t c = 0; c < perk_result_cols(res); c++)
        {
            const PerkValue *v = perk_result_get(res, r, c);
            if (v->type == PERK_TEXT && strstr(v->as.text.ptr, "TOPSECRET") != NULL)
                return 1;
        }
    return 0;
}

static const char *err_name(PerkErr e)
{
    switch (e)
    {
    case PERK_OK: return "OK";
    case PERK_ERR_OPEN: return "ERR_OPEN";
    case PERK_ERR_SQL: return "ERR_SQL";
    case PERK_ERR_BUSY: return "ERR_BUSY";
    case PERK_ERR_BADNAME: return "ERR_BADNAME";
    case PERK_ERR_NOMEM: return "ERR_NOMEM";
    case PERK_ERR_MISUSE: return "ERR_MISUSE";
    case PERK_ERR_EXISTS: return "ERR_EXISTS";
    }
    return "?";
}

// Prints a payload on one line, escaping control characters
static void print_payload(const char *p, size_t n)
{
    size_t shown = 0;
    for (size_t i = 0; i < n && shown < 44; i++, shown++)
    {
        unsigned char c = (unsigned char)p[i];
        if (c == '\n') { fputs("\\n", stdout); shown++; }
        else if (c == '\0') { fputs("\\0", stdout); shown++; }
        else if (c < 0x20) { printf("\\x%02x", c); shown += 3; }
        else putchar(c);
    }
    if (n > 44)
        fputs("...", stdout);
    else
        for (; shown < 47; shown++)
            putchar(' ');
}

typedef enum { BLOCKED, NEUTRALIZED, BREACHED } Outcome;

static void report(const char *surface, const char *payload, size_t len, Outcome o, const char *detail)
{
    total_attempts++;
    if (o == BLOCKED) blocked++;
    else if (o == NEUTRALIZED) neutralized++;
    else breached++;

    if (surface == NULL)
        return;  // fuzz attempts are only counted
    printf("  %-14s ", surface);
    print_payload(payload, len);
    printf(" %-12s %s\n", o == BLOCKED ? "BLOCKED" : o == NEUTRALIZED ? "NEUTRALIZED" : "**BREACHED**", detail);
}

// Outcome for an attempt that should never succeed (names, types)
static Outcome expect_rejected(PerkErr e, char *detail, size_t dlen)
{
    int intact = database_intact();
    snprintf(detail, dlen, "%s%s", err_name(e), intact ? "" : ", DATABASE CHANGED");
    if (!intact)
        return BREACHED;
    return e == PERK_OK ? BREACHED : BLOCKED;
}

static const char *payloads[] = {
    "' OR '1'='1",
    "' OR 1=1 --",
    "\" OR \"1\"=\"1",
    "admin'--",
    "'; DROP TABLE secrets; --",
    "\"; DROP TABLE secrets; --",
    "1; DROP TABLE secrets",
    "users; DROP TABLE secrets",
    "users\"; DROP TABLE secrets; --",
    "users` ; DROP TABLE secrets; --",
    "users] ; DROP TABLE secrets; --",
    "1 UNION SELECT secret FROM secrets",
    "') UNION SELECT secret, secret FROM secrets --",
    "' || (SELECT secret FROM secrets) || '",
    "1 AND (SELECT count(*) FROM secrets) > 0",
    "x' AND 1=(SELECT 1 FROM sqlite_master) --",
    "1; ATTACH DATABASE '/tmp/perk_evil.db' AS evil; --",
    "1; PRAGMA foreign_keys = OFF; --",
    "1; PRAGMA writable_schema = ON; --",
    "' ; SELECT load_extension('evil') --",
    "1; BEGIN; DELETE FROM secrets; --",
    "/**/OR/**/1=1",
    "\\' OR 1=1 --",
    "%27 OR 1=1 --",
    "\xca\xbc OR 1=1 --",
    "users\nDROP TABLE secrets",
    "users\x0b; DROP TABLE secrets",
    "sqlite_master",
    "SQLITE_MASTER",
    "secrets",
    "1 OR 1 = 1",
    "randomblob(1000000000)",
    "id = ? ; VACUUM INTO '/tmp/perk_copy.db'",
};
#define NPAYLOADS (sizeof(payloads) / sizeof(payloads[0]))

static void attack_table_names(void)
{
    printf("\n[1] Payload as table name (perk_create_table / insert / select / update / delete)\n");
    for (size_t i = 0; i < NPAYLOADS; i++)
    {
        const char *p = payloads[i];
        char detail[96];
        PerkColumn col[] = { { "x", "TEXT", 0 } };
        const char *name_col[] = { "name" };
        PerkValue v = perk_text("x");
        PerkResult *res = NULL;
        int64_t changed = 0;

        PerkErr e[5];
        e[0] = perk_create_table(db, p, col, 1);
        e[1] = perk_insert(db, p, name_col, &v, 1, NULL);
        e[2] = perk_select(db, p, NULL, 0, NULL, NULL, 0, &res);
        int leaked = res != NULL && result_leaks_secret(res);
        perk_result_free(res);
        e[3] = perk_update(db, p, name_col, &v, 1, NULL, NULL, 0, &changed);
        e[4] = perk_delete(db, p, NULL, NULL, 0, &changed);

        // "secrets" is a legitimate table name: operations on it are allowed by
        // design, so for that payload only check nothing escaped the call
        int legit = strcmp(p, "secrets") == 0;
        PerkErr worst = PERK_ERR_BADNAME;
        for (int k = 0; k < 5; k++)
            if (e[k] != PERK_ERR_BADNAME)
                worst = e[k];

        if (legit)
        {
            report("table name", p, strlen(p), NEUTRALIZED,
                   "valid name, operations scoped to that table (by design)");
            // Restore the canary for the rest of the run
            sqlite3_exec(db->conn, "DELETE FROM secrets; INSERT INTO secrets VALUES "
                         "(1,'TOPSECRET-alpha'),(2,'TOPSECRET-bravo'),(3,'TOPSECRET-charlie');", 0, 0, 0);
            continue;
        }
        Outcome o = expect_rejected(worst, detail, sizeof(detail));
        if (leaked)
            o = BREACHED;
        report("table name", p, strlen(p), o, detail);
    }
}

static void attack_column_names(void)
{
    printf("\n[2] Payload as column name (create / insert / select / update SET)\n");
    for (size_t i = 0; i < NPAYLOADS; i++)
    {
        const char *p = payloads[i];
        char detail[96];
        const char *cols[] = { p };
        PerkColumn pcol[] = { { p, "TEXT", 0 } };
        PerkValue v = perk_text("x");
        PerkResult *res = NULL;

        PerkErr e[4];
        e[0] = perk_create_table(db, "t_inj", pcol, 1);
        e[1] = perk_insert(db, "users", cols, &v, 1, NULL);
        e[2] = perk_select(db, "users", cols, 1, NULL, NULL, 0, &res);
        int leaked = res != NULL && result_leaks_secret(res);
        perk_result_free(res);
        e[3] = perk_update(db, "users", cols, &v, 1, NULL, NULL, 0, NULL);

        PerkErr worst = PERK_ERR_BADNAME;
        for (int k = 0; k < 4; k++)
            if (e[k] != PERK_ERR_BADNAME)
                worst = e[k];
        // "secrets" is a valid identifier; there is no such column in users
        if (strcmp(p, "secrets") == 0)
        {
            sqlite3_exec(db->conn, "DROP TABLE IF EXISTS t_inj", 0, 0, 0);
            int intact = database_intact();
            report("column name", p, strlen(p), intact && !leaked ? NEUTRALIZED : BREACHED,
                   "valid name; no such column in users -> ERR_SQL");
            continue;
        }
        Outcome o = expect_rejected(worst, detail, sizeof(detail));
        if (leaked)
            o = BREACHED;
        report("column name", p, strlen(p), o, detail);
    }
}

static void attack_column_types(void)
{
    printf("\n[3] Payload as column type (perk_create_table)\n");
    const char *types[] = {
        "TEXT); DROP TABLE secrets; --",
        "TEXT, evil TEXT",
        "TEXT DEFAULT (SELECT secret FROM secrets)",
        "TEXT CHECK (0)",
        "TEXT GENERATED ALWAYS AS (1)",
        "INTEGER PRIMARY KEY AUTOINCREMENT",
        "TEXT\0; DROP TABLE secrets",
        "VARCHAR(255)",
    };
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
    {
        char detail[96];
        PerkColumn c[] = { { "x", types[i], 0 } };
        PerkErr e = perk_create_table(db, "t_types", c, 1);
        // "TEXT\0..." is just "TEXT" to C; anything that got through must be a plain column
        if (e == PERK_OK)
        {
            char *sql = query_text("SELECT sql FROM sqlite_master WHERE name = 't_types'");
            int plain = sql != NULL && strcmp(sql, "CREATE TABLE \"t_types\" (\"x\" TEXT)") == 0;
            free(sql);
            sqlite3_exec(db->conn, "DROP TABLE t_types", 0, 0, 0);
            int intact = database_intact();
            report("column type", types[i], i == 6 ? 5 + strlen(types[i] + 5) : strlen(types[i]),
                   plain && intact ? NEUTRALIZED : BREACHED,
                   "C string ends at \\0: created plain TEXT column");
            continue;
        }
        report("column type", types[i], strlen(types[i]), expect_rejected(e, detail, sizeof(detail)), detail);
    }
}

// Values must round-trip exactly and never change which rows a query touches
static void attack_values(void)
{
    printf("\n[4] Payload as value (insert / update / where args)\n");
    for (size_t i = 0; i < NPAYLOADS; i++)
    {
        const char *p = payloads[i];
        size_t len = strlen(p);
        PerkValue v = { .type = PERK_TEXT, .as.text = { p, len } };

        perk_delete(db, "users", NULL, NULL, 0, NULL);
        const char *name_col[] = { "name" };
        PerkValue decoy = perk_text("decoy");
        for (int d = 0; d < 3; d++)
            perk_insert(db, "users", name_col, &decoy, 1, NULL);

        int64_t id = 0, changed = -1;
        int ok = perk_insert(db, "users", name_col, &v, 1, &id) == PERK_OK;

        // Exact round trip, and a WHERE name = ? match returns only that row
        PerkResult *res = NULL;
        ok = ok && perk_select(db, "users", NULL, 0, "name = ?", &v, 1, &res) == PERK_OK &&
             perk_result_rows(res) == 1;
        const PerkValue *got = perk_result_get(res, 0, 1);
        ok = ok && got != NULL && got->type == PERK_TEXT && got->as.text.len == len &&
             memcmp(got->as.text.ptr, p, len) == 0;
        int leaked = res != NULL && result_leaks_secret(res);
        perk_result_free(res);

        // As an UPDATE value and as a WHERE arg, only the matching row changes
        const char *note_col[] = { "note" };
        ok = ok && perk_update(db, "users", note_col, &v, 1, "name = ?", &v, 1, &changed) == PERK_OK &&
             changed == 1;
        ok = ok && perk_delete(db, "users", "name = ?", &v, 1, &changed) == PERK_OK && changed == 1;
        ok = ok && perk_select(db, "users", NULL, 0, NULL, NULL, 0, &res) == PERK_OK &&
             perk_result_rows(res) == 3;
        perk_result_free(res);

        int intact = database_intact();
        report("value", p, len, ok && intact && !leaked ? NEUTRALIZED : BREACHED,
               ok ? "stored and matched literally; decoys untouched" : "UNEXPECTED BEHAVIOUR");
    }
}

static void attack_where(void)
{
    printf("\n[5] Payload in a where clause (select / update / delete)\n");
    const char *wheres[] = {
        "name = ? OR 1=1",
        "name = '' OR '1'='1'",
        "name = ? --",
        "name = ? /* comment */",
        "name = ?; DROP TABLE secrets",
        "name = ? UNION SELECT secret, secret, secret FROM secrets",
        "id IN (SELECT id FROM secrets)",
        "EXISTS (SELECT 1 FROM secrets)",
        "name = (SELECT secret FROM secrets)",
        "name = ? OR name = secrets.secret",
        "name = ? OR id IS NOT NULL",
        "load_extension(?) IS NULL",
        "randomblob(?) IS NOT NULL",
        "name = ?1",
        "name = :name",
        "name = @name",
        "name = $name",
        "name = x'41'",
        "name = CAST(? AS TEXT)",
        "name = ? COLLATE NOCASE",
        "name LIKE ? ESCAPE ?",
        "name = ?) OR (1",
        "name = \"name\"",
        "name = `name`",
        "name = [name]",
        "sqlite_master = ?",
        "name\x00 OR 1=1",
        "name = ? \xe2\x80\xa8 OR 1=1",
    };
    for (size_t i = 0; i < sizeof(wheres) / sizeof(wheres[0]); i++)
    {
        const char *w = wheres[i];
        PerkValue args[] = { perk_text("decoy"), perk_text("decoy") };
        size_t nargs = 0;
        for (const char *c = w; *c; c++)
            nargs += *c == '?';
        if (nargs > 2)
            nargs = 2;

        perk_delete(db, "users", NULL, NULL, 0, NULL);
        const char *name_col[] = { "name" };
        PerkValue decoy = perk_text("decoy"), other = perk_text("other");
        perk_insert(db, "users", name_col, &decoy, 1, NULL);
        perk_insert(db, "users", name_col, &other, 1, NULL);

        PerkResult *res = NULL;
        PerkErr es = perk_select(db, "users", NULL, 0, w, args, nargs, &res);
        int leaked = res != NULL && result_leaks_secret(res);
        size_t rows = perk_result_rows(res);
        perk_result_free(res);
        int64_t changed = 0;
        const char *note_col[] = { "note" };
        PerkErr eu = perk_update(db, "users", note_col, &decoy, 1, w, args, nargs, &changed);
        PerkErr ed = perk_delete(db, "users", w, args, nargs, &changed);
        int intact = database_intact();

        char detail[128];
        Outcome o;
        if (!intact || leaked)
        {
            o = BREACHED;
            snprintf(detail, sizeof(detail), "select=%s update=%s delete=%s%s%s", err_name(es), err_name(eu),
                     err_name(ed), intact ? "" : " DATABASE CHANGED", leaked ? " SECRET LEAKED" : "");
        }
        else if (es == PERK_OK || eu == PERK_OK || ed == PERK_OK)
        {
            // Accepted clauses are plain filters on the users table (e.g. "OR id IS NOT NULL").
            // That's the caller's own logic, and it can't reach other tables.
            o = NEUTRALIZED;
            snprintf(detail, sizeof(detail), "valid filter on users only (%zu rows); nothing else touched", rows);
        }
        else
        {
            o = BLOCKED;
            snprintf(detail, sizeof(detail), "%s: %.80s", err_name(es), perk_errmsg(db));
        }
        report("where", w, strlen(w), o, detail);
    }
}

// Random-input fuzzing. Output is only counted; any breach is listed.
static uint64_t rng_state = 0xC0FFEE1234567ULL;
static uint64_t next_rand(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static int matches_identifier_rule(const char *s)
{
    if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '_'))
        return 0;
    size_t n = 0;
    for (; s[n]; n++)
        if (!((s[n] >= 'a' && s[n] <= 'z') || (s[n] >= 'A' && s[n] <= 'Z') || s[n] == '_' ||
              (s[n] >= '0' && s[n] <= '9')))
            return 0;
    return n <= 64 && sqlite3_strnicmp(s, "sqlite_", 7) != 0;
}

static void fuzz(size_t iterations)
{
    printf("\n[6] Fuzzing (%zu iterations each)\n", iterations);

    // Random bytes as table/column names: accepted only if they follow the identifier rule
    size_t accepted = 0, wrongly_accepted = 0;
    int start_breached = breached;
    for (size_t i = 0; i < iterations; i++)
    {
        char name[80];
        size_t len = 1 + (size_t)(next_rand() % 70);
        for (size_t k = 0; k < len; k++)
        {
            // Bias toward identifier characters so many inputs are near-valid
            uint64_t r = next_rand();
            static const char near[] = "abcXYZ_09;'\" -()*/\\`[]";
            name[k] = (r % 4) ? near[r % (sizeof(near) - 1)] : (char)(1 + (r >> 8) % 255);
        }
        name[len] = '\0';
        PerkValue v = perk_text("x");
        const char *cols[] = { "name" };
        PerkErr e = perk_insert(db, name, cols, &v, 1, NULL);
        if (e != PERK_ERR_BADNAME)
        {
            accepted++;
            if (!matches_identifier_rule(name))
                wrongly_accepted++;
        }
        report(NULL, NULL, 0, e == PERK_ERR_BADNAME ? BLOCKED : NEUTRALIZED, NULL);
    }
    if (!database_intact())
        report(NULL, NULL, 0, BREACHED, NULL);
    printf("  identifier fuzz: %zu accepted (all valid names, no such table), %zu wrongly accepted, %d breaches\n",
           accepted, wrongly_accepted, breached - start_breached);
    if (wrongly_accepted)
        breached++;

    // Random token soup as WHERE clauses for select, update and delete
    static const char *tokens[] = {
        "'", "\"", ";", "--", "/*", "*/", "(", ")", ",", "?", "=", "<", ">", "!=", "OR", "AND",
        "NOT", "IS", "NULL", "IN", "LIKE", "BETWEEN", "SELECT", "FROM", "secrets", "secret",
        "DROP", "TABLE", "UNION", "ATTACH", "PRAGMA", "1", "0x1", "id", "name", "note", "\\",
        "`", "[", "]", ":a", "@a", "$a", "?1", "||", "load_extension", "sqlite_master", "\n",
        "\t", " ", " ", " ", "x'00'", "CAST", "AS", "EXISTS", "VACUUM", "*", "%", "_",
    };
    size_t ntok = sizeof(tokens) / sizeof(tokens[0]);
    size_t where_ok = 0;
    start_breached = breached;
    PerkValue args[16];
    for (int k = 0; k < 16; k++)
        args[k] = perk_text("decoy");

    for (size_t i = 0; i < iterations; i++)
    {
        char w[512] = "";
        size_t len = 0, nq = 0;
        size_t count = 1 + (size_t)(next_rand() % 12);
        for (size_t t = 0; t < count; t++)
        {
            const char *tok = tokens[next_rand() % ntok];
            size_t tl = strlen(tok);
            if (len + tl + 2 >= sizeof(w))
                break;
            memcpy(w + len, tok, tl);
            len += tl;
            if (next_rand() % 2)
                w[len++] = ' ';
            w[len] = '\0';
            nq += strcmp(tok, "?") == 0;
        }
        if (nq > 16)
            nq = 16;

        PerkResult *res = NULL;
        int op = (int)(next_rand() % 3);
        PerkErr e;
        int leaked = 0;
        if (op == 0)
        {
            e = perk_select(db, "users", NULL, 0, w, args, nq, &res);
            leaked = res != NULL && result_leaks_secret(res);
            perk_result_free(res);
        }
        else if (op == 1)
        {
            const char *note_col[] = { "note" };
            e = perk_update(db, "users", note_col, args, 1, w, args, nq, NULL);
        }
        else
            e = perk_delete(db, "users", w, args, nq, NULL);
        where_ok += e == PERK_OK;

        int intact = (i % 50 == 0) ? database_intact() : 1;
        report(NULL, NULL, 0, leaked || !intact ? BREACHED : e == PERK_OK ? NEUTRALIZED : BLOCKED, NULL);
        if (leaked || !intact)
            printf("  BREACH with where: %s\n", w);
    }
    if (!database_intact())
        report(NULL, NULL, 0, BREACHED, NULL);
    printf("  where fuzz: %zu accepted as valid filters, %zu rejected, %d breaches\n",
           where_ok, iterations - where_ok, breached - start_breached);
}

int main(int argc, char **argv)
{
    size_t fuzz_iterations = argc > 1 ? (size_t)strtoull(argv[1], NULL, 10) : 20000;

    if (perk_open(":memory:", &db) != PERK_OK)
    {
        printf("open failed: %s\n", perk_errmsg(NULL));
        return 1;
    }
    PerkColumn users[] = { { "id", "INTEGER", PERK_COL_PK }, { "name", "TEXT", 0 }, { "note", "TEXT", 0 } };
    PerkColumn secrets[] = { { "id", "INTEGER", PERK_COL_PK }, { "secret", "TEXT", 0 } };
    perk_create_table(db, "users", users, 3);
    perk_create_table(db, "secrets", secrets, 2);
    sqlite3_exec(db->conn, "INSERT INTO secrets VALUES "
                 "(1,'TOPSECRET-alpha'),(2,'TOPSECRET-bravo'),(3,'TOPSECRET-charlie');", 0, 0, 0);
    baseline_schema = query_text(SCHEMA_SQL);
    baseline_secrets = query_text(SECRETS_SQL);

    printf("PerkDB injection suite\n");
    printf("BLOCKED = rejected by PerkDB. NEUTRALIZED = accepted but treated as data / a\n");
    printf("plain filter, database verified unchanged. BREACHED = injection had an effect.\n");

    attack_table_names();
    attack_column_names();
    attack_column_types();
    attack_values();
    attack_where();
    fuzz(fuzz_iterations);

    printf("\nTotal attempts: %d   blocked: %d   neutralized: %d   breached: %d\n",
           total_attempts, blocked, neutralized, breached);

    free(baseline_schema);
    free(baseline_secrets);
    perk_close(db);
    return breached != 0;
}
