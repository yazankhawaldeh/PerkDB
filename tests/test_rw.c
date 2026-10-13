// Write/read test suite and benchmark.
// Usage: test_rw [records] [db_path]
// Every phase verifies what it reads back byte-for-byte and reports its timing.

#include "perkdb.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    if (failures <= 20) printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static double now(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// xorshift64, fixed seed so every run generates the same data
static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;
static uint64_t next_rand(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

typedef struct {
    int64_t id;
    char name[32];
    int64_t qty;
    double price;
    unsigned char *data;
    size_t data_len;
    char *note;
    size_t note_len;
    int deleted;
} Rec;

// Note text is built from SQL metacharacters, multi-byte UTF-8 and embedded NULs
static const struct { const char *s; size_t n; } note_pieces[] = {
    { "'", 1 }, { "\"", 1 }, { ";", 1 }, { "--", 2 }, { " OR 1=1", 7 }, { "\xe6\x97\xa5\xe6\x9c\xac", 6 },
    { "\xc3\xa9", 2 }, { "\xf0\x9f\x8e\x89", 4 }, { "\0", 1 }, { "\\", 1 }, { "%", 1 }, { "_", 1 },
    { "\n", 1 }, { "abc", 3 }, { "DROP TABLE items", 16 },
};
#define NPIECES (sizeof(note_pieces) / sizeof(note_pieces[0]))

static int gen_rec(Rec *r, size_t i)
{
    snprintf(r->name, sizeof(r->name), "item-%zu", i);
    r->qty = (int64_t)(next_rand() % 1000);
    r->price = (double)(next_rand() % 10000000) / 100.0;
    r->deleted = 0;

    r->data_len = (size_t)(next_rand() % 513);
    r->data = malloc(r->data_len ? r->data_len : 1);
    if (r->data == NULL)
        return 0;
    for (size_t b = 0; b < r->data_len; b++)
        r->data[b] = (unsigned char)next_rand();

    size_t npieces = (size_t)(next_rand() % 21);
    r->note = malloc(npieces * 16 + 1);
    if (r->note == NULL)
        return 0;
    r->note_len = 0;
    for (size_t p = 0; p < npieces; p++)
    {
        size_t k = (size_t)(next_rand() % NPIECES);
        memcpy(r->note + r->note_len, note_pieces[k].s, note_pieces[k].n);
        r->note_len += note_pieces[k].n;
    }
    return 1;
}

static PerkValue text_n(const char *s, size_t n)
{
    PerkValue v = { .type = PERK_TEXT, .as.text = { s, n } };
    return v;
}

static int text_eq(const PerkValue *v, const char *s, size_t n)
{
    return v != NULL && v->type == PERK_TEXT && v->as.text.len == n &&
           memcmp(v->as.text.ptr, s, n) == 0 && v->as.text.ptr[n] == '\0';
}

static int blob_eq(const PerkValue *v, const void *p, size_t n)
{
    return v != NULL && v->type == PERK_BLOB && v->as.blob.len == n &&
           (n == 0 || memcmp(v->as.blob.ptr, p, n) == 0);
}

static const char *item_cols[] = { "id", "name", "qty", "price", "data", "note" };

// Compares one result row (in item_cols order) against the record it came from
static int row_matches(const PerkResult *res, size_t row, const Rec *r)
{
    const PerkValue *id = perk_result_get(res, row, 0);
    const PerkValue *qty = perk_result_get(res, row, 2);
    const PerkValue *price = perk_result_get(res, row, 3);
    return id && id->type == PERK_INT && id->as.i == r->id &&
           text_eq(perk_result_get(res, row, 1), r->name, strlen(r->name)) &&
           qty && qty->type == PERK_INT && qty->as.i == r->qty &&
           price && price->type == PERK_REAL && price->as.r == r->price &&
           blob_eq(perk_result_get(res, row, 4), r->data, r->data_len) &&
           text_eq(perk_result_get(res, row, 5), r->note, r->note_len);
}

// Reads the whole table and checks it matches recs exactly (including deletions)
static int verify_all(PerkDB *db, const Rec *recs, size_t n)
{
    PerkResult *res = NULL;
    if (perk_select(db, "items", item_cols, 6, NULL, NULL, 0, &res) != PERK_OK)
        return 0;

    size_t live = 0, row = 0, ok = 1;
    for (size_t i = 0; i < n; i++)
    {
        if (recs[i].deleted)
            continue;
        live++;
        // Rows come back in rowid order, which is insertion order here
        if (row >= perk_result_rows(res) || !row_matches(res, row, &recs[i]))
            ok = 0;
        row++;
    }
    ok = ok && perk_result_rows(res) == live;
    perk_result_free(res);
    return (int)ok;
}

typedef struct {
    const char *name;
    size_t ops;
    double secs;
    int ok;
} Phase;

static Phase phases[16];
static size_t nphases = 0;

static void record_phase(const char *name, size_t ops, double secs, int ok)
{
    phases[nphases++] = (Phase){ name, ops, secs, ok };
    CHECK(ok);
}

static int insert_rec(PerkDB *db, Rec *r)
{
    const char *cols[] = { "name", "qty", "price", "data", "note" };
    PerkValue vals[] = { perk_text(r->name), perk_int(r->qty), perk_real(r->price),
                         perk_blob(r->data, r->data_len), text_n(r->note, r->note_len) };
    return perk_insert(db, "items", cols, vals, 5, &r->id) == PERK_OK;
}

// Edge-case values, stored in an untyped column so SQLite keeps them as given
static void test_edge_values(PerkDB *db)
{
    PerkColumn cols[] = { { "id", "INTEGER", PERK_COL_PK }, { "v", NULL, 0 } };
    CHECK(perk_create_table(db, "edge", cols, 2) == PERK_OK);

    size_t big = 1024 * 1024;
    char *big_text = malloc(big);
    unsigned char *big_blob = malloc(big);
    if (big_text == NULL || big_blob == NULL)
    {
        CHECK(!"out of memory");
        free(big_text);
        free(big_blob);
        return;
    }
    for (size_t i = 0; i < big; i++)
    {
        big_text[i] = (char)('a' + i % 26);
        big_blob[i] = (unsigned char)next_rand();
    }

    struct { const char *label; PerkValue v; } cases[] = {
        { "INT64_MIN", perk_int(INT64_MIN) },
        { "INT64_MAX", perk_int(INT64_MAX) },
        { "0.0", perk_real(0.0) },
        { "-0.0", perk_real(-0.0) },
        { "DBL_MAX", perk_real(DBL_MAX) },
        { "DBL_TRUE_MIN (denormal)", perk_real(DBL_TRUE_MIN) },
        { "+INFINITY", perk_real(INFINITY) },
        { "-INFINITY", perk_real(-INFINITY) },
        { "NAN", perk_real(NAN) },
        { "empty text", text_n("", 0) },
        { "empty blob", perk_blob(NULL, 0) },
        { "1 MiB text", text_n(big_text, big) },
        { "1 MiB blob", perk_blob(big_blob, big) },
        { "invalid UTF-8 bytes", text_n("\xff\xfe\x80", 3) },
        { "UTF-8 + emoji", perk_text("h\xc3\xa9llo \xe6\x97\xa5\xe6\x9c\xac \xf0\x9f\x8e\x89") },
        { "text with NULs", text_n("a\0b\0c", 5) },
        { "NULL", perk_null() },
    };

    printf("\nEdge values (stored, read back, compared):\n");
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        const char *vcol[] = { "v" };
        int64_t id = 0;
        PerkErr err = perk_insert(db, "edge", vcol, &cases[i].v, 1, &id);

        PerkResult *res = NULL;
        PerkValue arg = perk_int(id);
        if (err == PERK_OK)
            err = perk_select(db, "edge", vcol, 1, "id = ?", &arg, 1, &res);
        const PerkValue *got = perk_result_get(res, 0, 0);
        const PerkValue *want = &cases[i].v;

        int same = 0;
        const char *note = "";
        if (err == PERK_OK && got != NULL)
        {
            if (want->type == PERK_REAL && isnan(want->as.r))
            {
                same = got->type == PERK_REAL && isnan(got->as.r);
                if (!same && got->type == PERK_NULL)
                    note = "SQLite stores NaN as NULL";
            }
            else if (want->type == PERK_REAL)
            {
                same = got->type == PERK_REAL && memcmp(&got->as.r, &want->as.r, sizeof(double)) == 0;
                if (!same && got->type == PERK_REAL && got->as.r == want->as.r)
                    note = "equal value, sign of zero lost";
            }
            else if (want->type == PERK_INT)
                same = got->type == PERK_INT && got->as.i == want->as.i;
            else if (want->type == PERK_TEXT)
                same = text_eq(got, want->as.text.ptr, want->as.text.len);
            else if (want->type == PERK_BLOB)
                same = blob_eq(got, want->as.blob.ptr, want->as.blob.len);
            else
                same = got->type == PERK_NULL;
        }
        else
            note = perk_errmsg(db);

        printf("  %-26s %s%s%s\n", cases[i].label, same ? "exact" : "CHANGED",
               note[0] ? "  - " : "", note);
        // NaN and -0.0 are SQLite storage rules, not PerkDB bugs; everything else must be exact
        if (!(want->type == PERK_REAL && (isnan(want->as.r) || (want->as.r == 0.0 && signbit(want->as.r)))))
            CHECK(same);
        perk_result_free(res);
    }

    free(big_text);
    free(big_blob);
}

int main(int argc, char **argv)
{
    size_t n = argc > 1 ? (size_t)strtoull(argv[1], NULL, 10) : 20000;
    const char *path = argc > 2 ? argv[2] : "build/test_rw.db";
    if (n < 10)
        n = 10;
    size_t n_auto = n / 10 < 2000 ? n / 10 : 2000;
    size_t total = n + n_auto;

    char wal[512], shm[512];
    snprintf(wal, sizeof(wal), "%s-wal", path);
    snprintf(shm, sizeof(shm), "%s-shm", path);
    remove(path);
    remove(wal);
    remove(shm);

    Rec *recs = calloc(total, sizeof(*recs));
    if (recs == NULL)
        return 1;
    for (size_t i = 0; i < total; i++)
        if (!gen_rec(&recs[i], i))
            return 1;

    PerkDB *db = NULL;
    if (perk_open(path, &db) != PERK_OK)
    {
        printf("open failed: %s\n", perk_errmsg(NULL));
        return 1;
    }
    PerkColumn schema[] = {
        { "id", "INTEGER", PERK_COL_PK },
        { "name", "TEXT", PERK_COL_NOTNULL | PERK_COL_UNIQUE },
        { "qty", "INTEGER", 0 },
        { "price", "REAL", 0 },
        { "data", "BLOB", 0 },
        { "note", "TEXT", 0 },
    };
    CHECK(perk_create_table(db, "items", schema, 6) == PERK_OK);

    printf("PerkDB write/read suite: %zu records, file %s\n", total, path);

    // 1. Bulk insert inside one transaction
    double t = now();
    int ok = perk_begin(db) == PERK_OK;
    for (size_t i = 0; i < n && ok; i++)
        ok = insert_rec(db, &recs[i]);
    ok = ok && perk_commit(db) == PERK_OK;
    record_phase("insert, 1 transaction", n, now() - t, ok);

    // 2. Inserts with one transaction each (each is a separate durable commit)
    t = now();
    ok = 1;
    for (size_t i = n; i < total && ok; i++)
        ok = insert_rec(db, &recs[i]);
    record_phase("insert, autocommit", n_auto, now() - t, ok);

    // 3. Read everything back and compare byte-for-byte
    t = now();
    ok = verify_all(db, recs, total);
    record_phase("select all + verify", total, now() - t, ok);

    // 4. Random point lookups by primary key
    size_t lookups = total < 10000 ? total : 10000;
    t = now();
    ok = 1;
    for (size_t i = 0; i < lookups && ok; i++)
    {
        Rec *r = &recs[next_rand() % total];
        PerkValue arg = perk_int(r->id);
        PerkResult *res = NULL;
        ok = perk_select(db, "items", item_cols, 6, "id = ?", &arg, 1, &res) == PERK_OK &&
             perk_result_rows(res) == 1 && row_matches(res, 0, r);
        perk_result_free(res);
    }
    record_phase("point lookup by id", lookups, now() - t, ok);

    // 5. Range queries, counts checked against the in-memory data
    t = now();
    ok = 1;
    for (int q = 0; q < 100 && ok; q++)
    {
        int64_t lo = (int64_t)(next_rand() % 1000), hi = lo + (int64_t)(next_rand() % 100);
        size_t expected = 0;
        for (size_t i = 0; i < total; i++)
            expected += recs[i].qty >= lo && recs[i].qty <= hi;
        PerkValue args[] = { perk_int(lo), perk_int(hi) };
        const char *idcol[] = { "id" };
        PerkResult *res = NULL;
        ok = perk_select(db, "items", idcol, 1, "qty BETWEEN ? AND ?", args, 2, &res) == PERK_OK &&
             perk_result_rows(res) == expected;
        perk_result_free(res);
    }
    record_phase("range query (table scan)", 100, now() - t, ok);

    // 6. Update every other row in one transaction, then re-verify everything
    t = now();
    size_t updates = 0;
    ok = perk_begin(db) == PERK_OK;
    for (size_t i = 0; i < total && ok; i += 2)
    {
        Rec *r = &recs[i];
        r->qty += 1;
        r->price = -r->price;
        const char *set[] = { "qty", "price" };
        PerkValue vals[] = { perk_int(r->qty), perk_real(r->price) };
        PerkValue arg = perk_int(r->id);
        int64_t changed = 0;
        ok = perk_update(db, "items", set, vals, 2, "id = ?", &arg, 1, &changed) == PERK_OK && changed == 1;
        updates++;
    }
    ok = ok && perk_commit(db) == PERK_OK;
    double update_secs = now() - t;
    ok = ok && verify_all(db, recs, total);
    record_phase("update by id, 1 transaction", updates, update_secs, ok);

    // 7. Delete by condition
    t = now();
    int64_t expected_deleted = 0, changed = 0;
    for (size_t i = 0; i < total; i++)
        if (recs[i].qty < 100)
        {
            recs[i].deleted = 1;
            expected_deleted++;
        }
    PerkValue thresh = perk_int(100);
    ok = perk_delete(db, "items", "qty < ?", &thresh, 1, &changed) == PERK_OK && changed == expected_deleted;
    double delete_secs = now() - t;
    ok = ok && verify_all(db, recs, total);
    record_phase("delete where qty < ?", (size_t)expected_deleted, delete_secs, ok);

    // 8. A rolled-back delete-all leaves everything in place
    t = now();
    ok = perk_begin(db) == PERK_OK &&
         perk_delete(db, "items", NULL, NULL, 0, &changed) == PERK_OK &&
         perk_rollback(db) == PERK_OK && verify_all(db, recs, total);
    record_phase("delete all + rollback", 1, now() - t, ok);

    // 9. Data survives close and reopen
    t = now();
    perk_close(db);
    db = NULL;
    ok = perk_open(path, &db) == PERK_OK && verify_all(db, recs, total);
    record_phase("close, reopen + verify", total, now() - t, ok);

    if (db != NULL)
    {
        test_edge_values(db);
        perk_close(db);
    }

    printf("\n%-30s %10s %10s %14s  %s\n", "phase", "ops", "ms", "ops/sec", "result");
    for (size_t i = 0; i < nphases; i++)
    {
        Phase *p = &phases[i];
        printf("%-30s %10zu %10.1f %14.0f  %s\n", p->name, p->ops, p->secs * 1000.0,
               p->secs > 0 ? (double)p->ops / p->secs : 0.0, p->ok ? "PASS" : "FAIL");
    }

    for (size_t i = 0; i < total; i++)
    {
        free(recs[i].data);
        free(recs[i].note);
    }
    free(recs);
    remove(path);
    remove(wal);
    remove(shm);

    printf("\n%d/%d checks passed\n", checks - failures, checks);
    return failures != 0;
}
