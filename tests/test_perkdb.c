#include "perkdb.h"
#include <stdio.h>
#include <string.h>

#include "test_harness.h"

// Checks a PerkErr result and, on failure, also prints the database's last error message
#define CHECK_ERR(expr, expected) do { PerkErr e_ = (expr); \
    if (!harness_eq_int(__FILE__, __LINE__, #expr, #expected, e_, (expected))) \
        printf("  errmsg:   %s\n", perk_errmsg(db)); } while (0)

static PerkDB *db;

static int64_t count_rows(const char *table)
{
    PerkResult *res = NULL;
    if (perk_select(db, table, NULL, 0, NULL, NULL, 0, &res) != PERK_OK)
        return -1;
    int64_t n = (int64_t)perk_result_rows(res);
    perk_result_free(res);
    return n;
}

static void test_open(void)
{
    PerkDB *bad = (PerkDB *)1;
    ASSERT_EQ_INT(perk_open("/nonexistent-dir/x.db", &bad), PERK_ERR_OPEN);
    ASSERT_TRUE(bad == NULL);
    ASSERT_TRUE(strlen(perk_errmsg(NULL)) > 0);
    ASSERT_EQ_INT(perk_open(NULL, &bad), PERK_ERR_MISUSE);
    ASSERT_EQ_INT(perk_open(":memory:", NULL), PERK_ERR_MISUSE);
    perk_close(NULL);
}

static void test_create_table(void)
{
    PerkColumn users[] = {
        { "id",    "INTEGER", PERK_COL_PK },
        { "name",  "text",    PERK_COL_NOTNULL | PERK_COL_UNIQUE },
        { "age",   "INT",     0 },
        { "score", "REAL",    0 },
        { "pic",   "BLOB",    0 },
        { "misc",  NULL,      0 },
    };
    CHECK_ERR(perk_create_table(db, "users", users, 6), PERK_OK);
    CHECK_ERR(perk_create_table(db, "users", users, 6), PERK_ERR_EXISTS);  // duplicates are rejected

    PerkColumn pair[] = { { "a", "INT", PERK_COL_PK }, { "b", "INT", PERK_COL_PK } };
    CHECK_ERR(perk_create_table(db, "pairs", pair, 2), PERK_OK);
    const char *pc[] = { "a", "b" };
    PerkValue pv[] = { perk_int(1), perk_int(2) };
    CHECK_ERR(perk_insert(db, "pairs", pc, pv, 2, NULL), PERK_OK);
    CHECK_ERR(perk_insert(db, "pairs", pc, pv, 2, NULL), PERK_ERR_SQL);  // composite key holds

    PerkColumn kw[] = { { "select", "INT", 0 } };
    CHECK_ERR(perk_create_table(db, "order", kw, 1), PERK_OK);

    PerkColumn bad_type[] = { { "x", "TEXT); DROP TABLE users; --", 0 } };
    CHECK_ERR(perk_create_table(db, "t1", bad_type, 1), PERK_ERR_BADNAME);
    PerkColumn bad_name[] = { { "x y", "INT", 0 } };
    CHECK_ERR(perk_create_table(db, "t1", bad_name, 1), PERK_ERR_BADNAME);
    PerkColumn null_name[] = { { NULL, "INT", 0 } };
    CHECK_ERR(perk_create_table(db, "t1", null_name, 1), PERK_ERR_BADNAME);
    PerkColumn bad_flags[] = { { "x", "INT", 1 << 7 } };
    CHECK_ERR(perk_create_table(db, "t1", bad_flags, 1), PERK_ERR_MISUSE);
    CHECK_ERR(perk_create_table(db, "sqlite_x", users, 1), PERK_ERR_BADNAME);
    CHECK_ERR(perk_create_table(db, "t1", NULL, 1), PERK_ERR_MISUSE);
    CHECK_ERR(perk_create_table(db, "t1", users, 0), PERK_ERR_MISUSE);
    ASSERT_EQ_INT(perk_create_table(NULL, "t1", users, 1), PERK_ERR_MISUSE);
}

static void test_insert(void)
{
    const char *cols[] = { "name", "age", "score", "pic" };
    int64_t id = -1;

    PerkValue v1[] = { perk_text("Robert'); DROP TABLE users;--"), perk_int(30), perk_real(1.5),
                       perk_blob("\0\1\2", 3) };
    CHECK_ERR(perk_insert(db, "users", cols, v1, 4, &id), PERK_OK);
    ASSERT_EQ_INT(id, 1);

    PerkValue v2[] = { perk_text("a\"b"), perk_int(20), perk_null(), perk_blob(NULL, 0) };
    CHECK_ERR(perk_insert(db, "users", cols, v2, 4, &id), PERK_OK);
    ASSERT_EQ_INT(id, 2);

    PerkValue v3[] = { perk_text(""), perk_int(40), perk_real(-2.25), perk_null() };
    CHECK_ERR(perk_insert(db, "users", cols, v3, 4, NULL), PERK_OK);

    // Text with an embedded NUL keeps its full length
    PerkValue nul = { .type = PERK_TEXT, .as.text = { "x\0y", 3 } };
    const char *name_col[] = { "name" };
    CHECK_ERR(perk_insert(db, "users", name_col, &nul, 1, NULL), PERK_OK);

    CHECK_ERR(perk_insert(db, "users", cols, v1, 4, NULL), PERK_ERR_SQL);  // UNIQUE
    PerkValue age[] = { perk_int(1) };
    const char *age_col[] = { "age" };
    CHECK_ERR(perk_insert(db, "users", age_col, age, 1, NULL), PERK_ERR_SQL);  // NOT NULL

    CHECK_ERR(perk_insert(db, "users; DROP TABLE users", age_col, age, 1, NULL), PERK_ERR_BADNAME);
    CHECK_ERR(perk_insert(db, "users\" --", age_col, age, 1, NULL), PERK_ERR_BADNAME);
    const char *bad_col[] = { "age) VALUES (1);--" };
    CHECK_ERR(perk_insert(db, "users", bad_col, age, 1, NULL), PERK_ERR_BADNAME);
    const char *null_col[] = { NULL };
    CHECK_ERR(perk_insert(db, "users", null_col, age, 1, NULL), PERK_ERR_BADNAME);
    CHECK_ERR(perk_insert(db, "sqlite_master", age_col, age, 1, NULL), PERK_ERR_BADNAME);
    CHECK_ERR(perk_insert(db, "1users", age_col, age, 1, NULL), PERK_ERR_BADNAME);
    CHECK_ERR(perk_insert(db, "", age_col, age, 1, NULL), PERK_ERR_BADNAME);
    char long_name[64 + 2];  // one past the 64-char limit
    memset(long_name, 'a', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    CHECK_ERR(perk_insert(db, long_name, age_col, age, 1, NULL), PERK_ERR_BADNAME);
    CHECK_ERR(perk_insert(db, "nope", age_col, age, 1, NULL), PERK_ERR_SQL);

    const char *kw_col[] = { "select" };
    CHECK_ERR(perk_insert(db, "order", kw_col, age, 1, NULL), PERK_OK);

    ASSERT_EQ_INT(perk_insert(NULL, "users", age_col, age, 1, NULL), PERK_ERR_MISUSE);
    CHECK_ERR(perk_insert(db, "users", age_col, age, 0, NULL), PERK_ERR_MISUSE);
    PerkValue null_text = { .type = PERK_TEXT };
    CHECK_ERR(perk_insert(db, "users", name_col, &null_text, 1, NULL), PERK_ERR_MISUSE);
    PerkValue null_blob = { .type = PERK_BLOB, .as.blob = { NULL, 5 } };
    CHECK_ERR(perk_insert(db, "users", name_col, &null_blob, 1, NULL), PERK_ERR_MISUSE);
    PerkValue bad_type = { .type = (PerkType)99 };
    CHECK_ERR(perk_insert(db, "users", name_col, &bad_type, 1, NULL), PERK_ERR_MISUSE);

    ASSERT_EQ_INT(count_rows("users"), 4);
}

static void test_select(void)
{
    PerkResult *res = NULL;
    const char *cols[] = { "id", "name", "pic" };
    PerkValue arg[] = { perk_int(1) };

    CHECK_ERR(perk_select(db, "users", cols, 3, "id = ?", arg, 1, &res), PERK_OK);
    ASSERT_EQ_INT(perk_result_rows(res), 1);
    ASSERT_EQ_INT(perk_result_cols(res), 3);
    ASSERT_EQ_STR(perk_result_colname(res, 1), "name");
    const PerkValue *name = perk_result_get(res, 0, 1);
    ASSERT_TRUE(name != NULL && name->type == PERK_TEXT);
    ASSERT_TRUE(name != NULL && strcmp(name->as.text.ptr, "Robert'); DROP TABLE users;--") == 0);
    const PerkValue *pic = perk_result_get(res, 0, 2);
    ASSERT_TRUE(pic != NULL && pic->type == PERK_BLOB && pic->as.blob.len == 3 &&
                memcmp(pic->as.blob.ptr, "\0\1\2", 3) == 0);
    ASSERT_TRUE(perk_result_get(res, 1, 0) == NULL);
    ASSERT_TRUE(perk_result_get(res, 0, 3) == NULL);
    ASSERT_TRUE(perk_result_colname(res, 3) == NULL);
    perk_result_free(res);

    // Values that look like SQL are only ever compared as data
    PerkValue inj[] = { perk_text("x' OR '1'='1") };
    CHECK_ERR(perk_select(db, "users", NULL, 0, "name = ?", inj, 1, &res), PERK_OK);
    ASSERT_EQ_INT(perk_result_rows(res), 0);
    perk_result_free(res);

    PerkValue range[] = { perk_int(20), perk_int(30) };
    CHECK_ERR(perk_select(db, "users", NULL, 0,
                          "age between ? and ? AND (score IS NULL OR score > ?)",
                          range, 2, &res), PERK_ERR_MISUSE);  // 3 ?s, 2 args
    PerkValue range3[] = { perk_int(20), perk_int(30), perk_real(1.0) };
    CHECK_ERR(perk_select(db, "users", NULL, 0,
                          "age between ? and ? AND (score IS NULL OR score > ?)",
                          range3, 3, &res), PERK_OK);
    ASSERT_EQ_INT(perk_result_rows(res), 2);
    perk_result_free(res);

    PerkValue in_args[] = { perk_int(20), perk_int(40) };
    CHECK_ERR(perk_select(db, "users", NULL, 0, "age IN (?, ?)", in_args, 2, &res), PERK_OK);
    ASSERT_EQ_INT(perk_result_rows(res), 2);
    perk_result_free(res);

    // Empty text and embedded NULs round-trip
    PerkValue empty[] = { perk_text("") };
    CHECK_ERR(perk_select(db, "users", NULL, 0, "name = ?", empty, 1, &res), PERK_OK);
    ASSERT_EQ_INT(perk_result_rows(res), 1);
    perk_result_free(res);
    PerkValue nul = { .type = PERK_TEXT, .as.text = { "x\0y", 3 } };
    const char *name_col[] = { "name" };
    CHECK_ERR(perk_select(db, "users", name_col, 1, "name = ?", &nul, 1, &res), PERK_OK);
    name = perk_result_get(res, 0, 0);
    ASSERT_TRUE(name != NULL && name->as.text.len == 3 && memcmp(name->as.text.ptr, "x\0y", 4) == 0);
    perk_result_free(res);

    // Keyword-named column referenced in a where clause
    PerkValue one[] = { perk_int(1) };
    CHECK_ERR(perk_select(db, "order", NULL, 0, "select = ?", one, 1, &res), PERK_OK);
    ASSERT_EQ_INT(perk_result_rows(res), 1);
    perk_result_free(res);

    // Rejected where clauses: literals, quotes, statement breaks, comments, other param styles
    const char *bad_where[] = {
        "id = 1", "name = 'x'", "name = \"x\"", "id = ? ; DROP TABLE users", "id = ? -- c",
        "id = ? /* c */", "id = ?1", "id = :id", "id = @id", "id = $id", "id = [id]",
        "id = `id`", "(id = ?", "id = ?)", "id = ? || ?", "id\x01= ?", "length(name) > ?",
        "abs (id) = ?", "id = randomblob(?)",
    };
    for (size_t i = 0; i < sizeof(bad_where) / sizeof(bad_where[0]); i++)
    {
        PerkValue a[] = { perk_int(1), perk_int(1) };
        res = (PerkResult *)1;
        PerkErr e = perk_select(db, "users", NULL, 0, bad_where[i], a, 1, &res);
        if (!ASSERT_TRUE(e != PERK_OK && res == NULL))
            printf("  where \"%s\" was accepted\n", bad_where[i]);
    }
    CHECK_ERR(perk_select(db, "users", NULL, 0, "sqlite_x = ?", one, 1, &res), PERK_ERR_BADNAME);

    // Subqueries can't form: unknown words become quoted column names
    ASSERT_TRUE(perk_select(db, "users", NULL, 0, "id IN (SELECT id FROM users)", NULL, 0, &res) != PERK_OK);
    ASSERT_TRUE(res == NULL);
    ASSERT_EQ_INT(count_rows("users"), 4);

    CHECK_ERR(perk_select(db, "users", NULL, 0, NULL, one, 1, &res), PERK_ERR_MISUSE);
    CHECK_ERR(perk_select(db, "users", NULL, 0, "id = ?", NULL, 1, &res), PERK_ERR_MISUSE);
    CHECK_ERR(perk_select(db, "users", NULL, 2, NULL, NULL, 0, &res), PERK_ERR_MISUSE);
    CHECK_ERR(perk_select(db, "users", NULL, 0, NULL, NULL, 0, NULL), PERK_ERR_MISUSE);
    CHECK_ERR(perk_select(db, "nope", NULL, 0, NULL, NULL, 0, &res), PERK_ERR_SQL);
    ASSERT_TRUE(res == NULL);

    // Accessors and free tolerate NULL
    ASSERT_TRUE(perk_result_rows(NULL) == 0 && perk_result_cols(NULL) == 0);
    ASSERT_TRUE(perk_result_get(NULL, 0, 0) == NULL && perk_result_colname(NULL, 0) == NULL);
    perk_result_free(NULL);
}

static void test_large_select(void)
{
    PerkColumn c[] = { { "n", "INTEGER", 0 }, { "s", "TEXT", 0 } };
    CHECK_ERR(perk_create_table(db, "big", c, 2), PERK_OK);
    const char *cols[] = { "n", "s" };
    CHECK_ERR(perk_begin(db), PERK_OK);
    for (int i = 0; i < 5000; i++)
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "row-%d", i);
        PerkValue v[] = { perk_int(i), perk_text(buf) };
        if (perk_insert(db, "big", cols, v, 2, NULL) != PERK_OK)
        {
            ASSERT_TRUE(!"insert into big failed");
            break;
        }
    }
    CHECK_ERR(perk_commit(db), PERK_OK);

    PerkResult *res = NULL;
    CHECK_ERR(perk_select(db, "big", cols, 2, NULL, NULL, 0, &res), PERK_OK);
    ASSERT_EQ_INT(perk_result_rows(res), 5000);
    const PerkValue *last = perk_result_get(res, 4999, 1);
    ASSERT_TRUE(last != NULL && strcmp(last->as.text.ptr, "row-4999") == 0);
    perk_result_free(res);
}

static void test_update(void)
{
    int64_t changed = -1;
    const char *set[] = { "score", "misc" };
    PerkValue vals[] = { perk_real(9.5), perk_text("updated") };
    PerkValue where_args[] = { perk_int(20) };

    CHECK_ERR(perk_update(db, "users", set, vals, 2, "age > ?", where_args, 1, &changed), PERK_OK);
    ASSERT_EQ_INT(changed, 2);

    PerkResult *res = NULL;
    const char *cols[] = { "misc" };
    CHECK_ERR(perk_select(db, "users", cols, 1, "score = ?", vals, 1, &res), PERK_OK);
    ASSERT_EQ_INT(perk_result_rows(res), 2);
    perk_result_free(res);

    // Injection-shaped value is stored literally
    const char *misc[] = { "misc" };
    PerkValue evil[] = { perk_text("'; DELETE FROM users; --") };
    CHECK_ERR(perk_update(db, "users", misc, evil, 1, "id = ?", where_args, 1, &changed), PERK_ERR_NOT_FOUND);
    ASSERT_EQ_INT(changed, 0);
    ASSERT_EQ_INT(count_rows("users"), 4);

    CHECK_ERR(perk_update(db, "users", misc, evil, 1, NULL, NULL, 0, &changed), PERK_OK);
    ASSERT_EQ_INT(changed, 4);
    CHECK_ERR(perk_update(db, "users", misc, evil, 1, "id = 1", NULL, 0, &changed), PERK_ERR_MISUSE);
    const char *bad_set[] = { "misc = 1, name" };
    CHECK_ERR(perk_update(db, "users", bad_set, evil, 1, NULL, NULL, 0, NULL), PERK_ERR_BADNAME);
    CHECK_ERR(perk_update(db, "users", misc, evil, 0, NULL, NULL, 0, NULL), PERK_ERR_MISUSE);
    CHECK_ERR(perk_update(db, "users", misc, NULL, 1, NULL, NULL, 0, NULL), PERK_ERR_MISUSE);
    CHECK_ERR(perk_update(db, "users", misc, evil, 1, "id = ?", NULL, 1, NULL), PERK_ERR_MISUSE);
    ASSERT_EQ_INT(perk_update(NULL, "users", misc, evil, 1, NULL, NULL, 0, NULL), PERK_ERR_MISUSE);
}

static void test_delete(void)
{
    int64_t changed = -1;
    PerkValue arg[] = { perk_int(1) };
    CHECK_ERR(perk_delete(db, "users", "id = ? OR 1 = 1", arg, 1, &changed), PERK_ERR_MISUSE);
    CHECK_ERR(perk_delete(db, "users", "id = ?; DELETE FROM users", arg, 1, &changed), PERK_ERR_MISUSE);
    ASSERT_EQ_INT(count_rows("users"), 4);

    CHECK_ERR(perk_delete(db, "users", "id = ?", arg, 1, &changed), PERK_OK);
    ASSERT_EQ_INT(changed, 1);
    ASSERT_EQ_INT(count_rows("users"), 3);

    CHECK_ERR(perk_delete(db, "users; DROP TABLE big", NULL, NULL, 0, NULL), PERK_ERR_BADNAME);
    CHECK_ERR(perk_delete(db, NULL, NULL, NULL, 0, NULL), PERK_ERR_MISUSE);
    CHECK_ERR(perk_delete(db, "users", "id = ?", NULL, 1, NULL), PERK_ERR_MISUSE);
    ASSERT_EQ_INT(perk_delete(NULL, "users", NULL, NULL, 0, NULL), PERK_ERR_MISUSE);

    CHECK_ERR(perk_delete(db, "big", NULL, NULL, 0, &changed), PERK_OK);
    ASSERT_EQ_INT(changed, 5000);
}

static void test_transactions(void)
{
    const char *cols[] = { "name" };
    PerkValue v[] = { perk_text("tx-rollback") };

    CHECK_ERR(perk_commit(db), PERK_ERR_MISUSE);
    CHECK_ERR(perk_rollback(db), PERK_ERR_MISUSE);

    CHECK_ERR(perk_begin(db), PERK_OK);
    CHECK_ERR(perk_begin(db), PERK_ERR_MISUSE);
    CHECK_ERR(perk_insert(db, "users", cols, v, 1, NULL), PERK_OK);
    ASSERT_EQ_INT(count_rows("users"), 4);
    CHECK_ERR(perk_rollback(db), PERK_OK);
    ASSERT_EQ_INT(count_rows("users"), 3);

    v[0] = perk_text("tx-commit");
    CHECK_ERR(perk_begin(db), PERK_OK);
    CHECK_ERR(perk_insert(db, "users", cols, v, 1, NULL), PERK_OK);
    CHECK_ERR(perk_commit(db), PERK_OK);
    ASSERT_EQ_INT(count_rows("users"), 4);

    ASSERT_EQ_INT(perk_begin(NULL), PERK_ERR_MISUSE);
    ASSERT_EQ_INT(perk_commit(NULL), PERK_ERR_MISUSE);
    ASSERT_EQ_INT(perk_rollback(NULL), PERK_ERR_MISUSE);

    // Closing with an open transaction rolls it back without leaking
    CHECK_ERR(perk_begin(db), PERK_OK);
}

int main(void)
{
    test_open();
    if (perk_open(":memory:", &db) != PERK_OK)
    {
        printf("FAIL: could not open database: %s\n", perk_errmsg(NULL));
        return 1;
    }
    test_create_table();
    test_insert();
    test_select();
    test_large_select();
    test_update();
    test_delete();
    test_transactions();
    perk_close(db);

    return harness_summary();
}
