// Memory safety tests.
//
// Every public function is fed NULL pointers, empty strings, identifiers at
// and just past the 64 character limit, and text/blob lengths that are far
// larger than the real buffer (the pointer must never be read: the buffers
// used are tiny, so ASan flags any over-read). Rejected calls must leave the
// out-parameters untouched/NULL and the handle usable. The suite is meant to
// be run under ASan/UBSan with zero reports.

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "perk_internal.h"
#include "test_harness.h"

#define CHECK_ERR(db, expr, expected)                                          \
  do {                                                                         \
    PerkErr e_ = (expr);                                                       \
    if (!harness_eq_int(__FILE__, __LINE__, #expr, #expected, e_, (expected))) \
      printf("  errmsg:   %s\n", perk_errmsg(db));                             \
  } while (0)

#define SENTINEL_DB ((PerkDB*)(uintptr_t)1)
#define SENTINEL_RES ((PerkResult*)(uintptr_t)1)
#define SENTINEL_I64 ((int64_t)-7777)

/* ----------------------------------------------------------------- helpers */

/**
 * Opens an in-memory database holding t(id INTEGER PK, name TEXT, data BLOB,
 * n INTEGER).
 */
static PerkDB* open_db(void) {
  PerkDB* db = NULL;
  if (perk_open(":memory:", &db) != PERK_OK || db == NULL) {
    printf("FATAL: cannot open in-memory db: %s\n", perk_errmsg(NULL));
    exit(2);
  }
  PerkColumn cols[] = {{"id", "INTEGER", PERK_COL_PK},
                       {"name", "TEXT", 0},
                       {"data", "BLOB", 0},
                       {"n", "INTEGER", 0}};
  if (perk_create_table(db, "t", cols, 4) != PERK_OK) {
    printf("FATAL: create t: %s\n", perk_errmsg(db));
    exit(2);
  }
  return db;
}

/** Inserts (id, name, n) into t. */
static PerkErr put(PerkDB* db, int64_t id, const char* name, int64_t n) {
  const char* cols[] = {"id", "name", "n"};
  PerkValue vals[] = {perk_int(id), perk_text(name), perk_int(n)};
  return perk_insert(db, "t", cols, vals, 3, NULL);
}

/** Number of rows in table, or -1 if the read failed. */
static long long count_rows(PerkDB* db, const char* table) {
  PerkResult* r = NULL;
  if (perk_select(db, table, NULL, 0, NULL, NULL, 0, &r) != PERK_OK) return -1;
  long long n = (long long)perk_result_rows(r);
  perk_result_free(r);
  return n;
}

/** Returns 1 if t still has exactly one row (1, "alice", 30) and is usable. */
static int one_row_intact(PerkDB* db) {
  PerkResult* r = NULL;
  PerkValue id = perk_int(1);
  if (perk_select_by_id(db, "t", NULL, 0, "id", &id, &r) != PERK_OK) return 0;
  int ok = perk_result_rows(r) == 1 && perk_result_cols(r) == 4;
  const PerkValue* name = perk_result_get(r, 0, 1);
  const PerkValue* n = perk_result_get(r, 0, 3);
  ok = ok && name && name->type == PERK_TEXT &&
       strcmp(name->as.text.ptr, "alice") == 0;
  ok = ok && n && n->type == PERK_INT && n->as.i == 30;
  perk_result_free(r);
  return ok && count_rows(db, "t") == 1;
}

static PerkDB* open_seeded(void) {
  PerkDB* db = open_db();
  if (put(db, 1, "alice", 30) != PERK_OK) {
    printf("FATAL: seed: %s\n", perk_errmsg(db));
    exit(2);
  }
  return db;
}

static PerkValue raw_text(const char* p, size_t len) {
  PerkValue v = {.type = PERK_TEXT, .as.text = {p, len}};
  return v;
}

/** Builds a name of exactly len 'a'..'z' characters into buf (len + 1 bytes).
 */
static void make_name(char* buf, size_t len) {
  for (size_t i = 0; i < len; i++) buf[i] = (char)('a' + (i % 26));
  buf[len] = '\0';
}

/* ------------------------------------------------------------- 5. perk_open */

static void test_open_nulls(void) {
  puts("open: NULL / bad arguments");
  PerkDB* db = SENTINEL_DB;
  CHECK_ERR(NULL, perk_open(NULL, &db), PERK_ERR_MISUSE);
  ASSERT_TRUE(db == NULL);  // *out must be NULL on every failure
  ASSERT_TRUE(perk_errmsg(NULL) != NULL);
  ASSERT_TRUE(strlen(perk_errmsg(NULL)) > 0);

  CHECK_ERR(NULL, perk_open(":memory:", NULL), PERK_ERR_MISUSE);
  CHECK_ERR(NULL, perk_open(NULL, NULL), PERK_ERR_MISUSE);

  db = SENTINEL_DB;
  CHECK_ERR(NULL, perk_open("/nonexistent_perk_dir/sub/x.db", &db),
            PERK_ERR_OPEN);
  ASSERT_TRUE(db == NULL);
  ASSERT_TRUE(strlen(perk_errmsg(NULL)) > 0);

  // Empty path: SQLite private temporary database. Pin the behaviour.
  db = SENTINEL_DB;
  PerkErr pe = perk_open("", &db);
  ASSERT_EQ_INT(pe, PERK_OK);
  if (pe == PERK_OK) {
    ASSERT_TRUE(db != NULL && db != SENTINEL_DB);
    PerkColumn tc[] = {{"id", "INTEGER", PERK_COL_PK}};
    CHECK_ERR(db, perk_create_table(db, "tmp", tc, 1), PERK_OK);
    perk_close(db);
  } else {
    ASSERT_TRUE(db == NULL);
  }

  // Very long path: clean PERK_ERR_OPEN, *out NULL
  size_t plen = 10000;
  char* longp = malloc(plen + 1);
  if (!longp) exit(2);
  memset(longp, 'p', plen);
  longp[plen] = '\0';
  db = SENTINEL_DB;
  CHECK_ERR(NULL, perk_open(longp, &db), PERK_ERR_OPEN);
  ASSERT_TRUE(db == NULL);
  free(longp);

  // A later good open clears the message and works
  db = SENTINEL_DB;
  CHECK_ERR(NULL, perk_open(":memory:", &db), PERK_OK);
  ASSERT_TRUE(db != NULL && db != SENTINEL_DB);
  ASSERT_EQ_STR(perk_errmsg(NULL), "");
  perk_close(db);
}

static void test_close_errmsg_nulls(void) {
  puts("close / errmsg: NULL");
  perk_close(NULL);  // no-op
  const char* m = perk_errmsg(NULL);
  ASSERT_TRUE(m != NULL);
  PerkDB* db = open_db();
  ASSERT_TRUE(perk_errmsg(db) != NULL);
  perk_close(db);
}

/* -------------------------------------------------- 1. NULL per parameter */

static void test_create_table_nulls(void) {
  puts("create_table: NULL arguments");
  PerkColumn good[] = {{"id", "INTEGER", PERK_COL_PK}, {"v", NULL, 0}};
  CHECK_ERR(NULL, perk_create_table(NULL, "x", good, 2), PERK_ERR_MISUSE);
  CHECK_ERR(NULL, perk_create_table(NULL, NULL, NULL, 0), PERK_ERR_MISUSE);

  PerkDB* db = open_seeded();
  CHECK_ERR(db, perk_create_table(db, NULL, good, 2), PERK_ERR_MISUSE);
  CHECK_ERR(db, perk_create_table(db, "x", NULL, 2), PERK_ERR_MISUSE);
  CHECK_ERR(db, perk_create_table(db, "x", good, 0), PERK_ERR_MISUSE);
  CHECK_ERR(db, perk_create_table(db, NULL, NULL, 0), PERK_ERR_MISUSE);
  PerkColumn nullname[] = {{NULL, "INTEGER", 0}};
  CHECK_ERR(db, perk_create_table(db, "x", nullname, 1), PERK_ERR_BADNAME);
  PerkColumn second_null[] = {{"ok", NULL, 0}, {NULL, NULL, 0}};
  CHECK_ERR(db, perk_create_table(db, "x", second_null, 2), PERK_ERR_BADNAME);
  PerkColumn nulltype[] = {{"a", NULL, 0}};  // NULL type is legal
  CHECK_ERR(db, perk_create_table(db, "x", nulltype, 1), PERK_OK);
  ASSERT_TRUE(one_row_intact(db));
  perk_close(db);
}

static void test_insert_nulls(void) {
  puts("insert: NULL arguments");
  const char* cols[] = {"name"};
  PerkValue vals[] = {perk_text("bob")};
  int64_t rid = SENTINEL_I64;
  CHECK_ERR(NULL, perk_insert(NULL, "t", cols, vals, 1, &rid), PERK_ERR_MISUSE);
  CHECK_ERR(NULL, perk_insert(NULL, NULL, NULL, NULL, 0, NULL),
            PERK_ERR_MISUSE);
  ASSERT_EQ_INT(rid, SENTINEL_I64);

  PerkDB* db = open_seeded();
  CHECK_ERR(db, perk_insert(db, NULL, cols, vals, 1, &rid), PERK_ERR_MISUSE);
  CHECK_ERR(db, perk_insert(db, "t", NULL, vals, 1, &rid), PERK_ERR_MISUSE);
  CHECK_ERR(db, perk_insert(db, "t", cols, NULL, 1, &rid), PERK_ERR_MISUSE);
  CHECK_ERR(db, perk_insert(db, "t", cols, vals, 0, &rid), PERK_ERR_MISUSE);
  CHECK_ERR(db, perk_insert(db, NULL, NULL, NULL, 0, NULL), PERK_ERR_MISUSE);
  ASSERT_EQ_INT(rid, SENTINEL_I64);

  const char* null_entry[] = {NULL};
  CHECK_ERR(db, perk_insert(db, "t", null_entry, vals, 1, &rid),
            PERK_ERR_BADNAME);
  const char* second_null[] = {"name", NULL};
  PerkValue two[] = {perk_text("x"), perk_int(1)};
  CHECK_ERR(db, perk_insert(db, "t", second_null, two, 2, &rid),
            PERK_ERR_BADNAME);
  ASSERT_EQ_INT(rid, SENTINEL_I64);

  // A PERK_TEXT whose pointer is NULL (built by hand) is MISUSE, never read
  PerkValue nulltext = {.type = PERK_TEXT, .as.text = {NULL, 5}};
  CHECK_ERR(db, perk_insert(db, "t", cols, &nulltext, 1, &rid),
            PERK_ERR_MISUSE);
  PerkValue nulltext0 = {.type = PERK_TEXT, .as.text = {NULL, 0}};
  CHECK_ERR(db, perk_insert(db, "t", cols, &nulltext0, 1, &rid),
            PERK_ERR_MISUSE);
  // A NULL blob pointer with len > 0 is MISUSE
  PerkValue nullblob = perk_blob(NULL, 3);
  const char* dcol[] = {"data"};
  CHECK_ERR(db, perk_insert(db, "t", dcol, &nullblob, 1, &rid),
            PERK_ERR_MISUSE);
  ASSERT_EQ_INT(rid, SENTINEL_I64);
  ASSERT_TRUE(one_row_intact(db));

  // rowid_out NULL is fine and perk_text(NULL) is stored as SQL NULL
  PerkValue nv = perk_text(NULL);
  ASSERT_EQ_INT(nv.type, PERK_NULL);
  CHECK_ERR(db, perk_insert(db, "t", cols, &nv, 1, NULL), PERK_OK);
  CHECK_ERR(db, perk_insert(db, "t", cols, vals, 1, &rid), PERK_OK);
  ASSERT_TRUE(rid != SENTINEL_I64);
  ASSERT_EQ_INT(count_rows(db, "t"), 3);
  perk_close(db);
}

static void test_select_nulls(void) {
  puts("select: NULL arguments");
  PerkResult* out = SENTINEL_RES;
  CHECK_ERR(NULL, perk_select(NULL, "t", NULL, 0, NULL, NULL, 0, &out),
            PERK_ERR_MISUSE);
  ASSERT_TRUE(out == NULL);
  CHECK_ERR(NULL, perk_select(NULL, NULL, NULL, 0, NULL, NULL, 0, NULL),
            PERK_ERR_MISUSE);

  PerkDB* db = open_seeded();
  out = SENTINEL_RES;
  CHECK_ERR(db, perk_select(db, NULL, NULL, 0, NULL, NULL, 0, &out),
            PERK_ERR_MISUSE);
  ASSERT_TRUE(out == NULL);
  CHECK_ERR(db, perk_select(db, "t", NULL, 0, NULL, NULL, 0, NULL),
            PERK_ERR_MISUSE);
  CHECK_ERR(db, perk_select(db, NULL, NULL, 0, NULL, NULL, 0, NULL),
            PERK_ERR_MISUSE);
  out = SENTINEL_RES;
  CHECK_ERR(db, perk_select(db, "t", NULL, 3, NULL, NULL, 0, &out),
            PERK_ERR_MISUSE);  // cols NULL but ncols > 0
  ASSERT_TRUE(out == NULL);
  out = SENTINEL_RES;
  CHECK_ERR(db, perk_select(db, "t", NULL, 0, "id = ?", NULL, 1, &out),
            PERK_ERR_MISUSE);  // args NULL but nargs > 0
  ASSERT_TRUE(out == NULL);
  const char* nullcol[] = {"name", NULL};
  out = SENTINEL_RES;
  CHECK_ERR(db, perk_select(db, "t", nullcol, 2, NULL, NULL, 0, &out),
            PERK_ERR_BADNAME);
  ASSERT_TRUE(out == NULL);
  // where NULL with an args count is MISUSE; NULL cols with ncols 0 is fine
  PerkValue a = perk_int(1);
  out = SENTINEL_RES;
  CHECK_ERR(db, perk_select(db, "t", NULL, 0, NULL, &a, 1, &out),
            PERK_ERR_MISUSE);
  ASSERT_TRUE(out == NULL);
  out = NULL;
  CHECK_ERR(db, perk_select(db, "t", NULL, 0, NULL, NULL, 0, &out), PERK_OK);
  ASSERT_EQ_INT(perk_result_rows(out), 1);
  perk_result_free(out);
  ASSERT_TRUE(one_row_intact(db));
  perk_close(db);
}

static void test_select_by_id_nulls(void) {
  puts("select_by_id: NULL arguments");
  PerkValue id = perk_int(1);
  PerkResult* out = SENTINEL_RES;
  CHECK_ERR(NULL, perk_select_by_id(NULL, "t", NULL, 0, "id", &id, &out),
            PERK_ERR_MISUSE);
  ASSERT_TRUE(out == NULL);
  CHECK_ERR(NULL, perk_select_by_id(NULL, NULL, NULL, 0, NULL, NULL, NULL),
            PERK_ERR_MISUSE);

  PerkDB* db = open_seeded();
  out = SENTINEL_RES;
  CHECK_ERR(db, perk_select_by_id(db, NULL, NULL, 0, "id", &id, &out),
            PERK_ERR_MISUSE);
  ASSERT_TRUE(out == NULL);
  out = SENTINEL_RES;
  CHECK_ERR(db, perk_select_by_id(db, "t", NULL, 0, NULL, &id, &out),
            PERK_ERR_MISUSE);
  ASSERT_TRUE(out == NULL);
  out = SENTINEL_RES;
  CHECK_ERR(db, perk_select_by_id(db, "t", NULL, 0, "id", NULL, &out),
            PERK_ERR_MISUSE);
  ASSERT_TRUE(out == NULL);
  CHECK_ERR(db, perk_select_by_id(db, "t", NULL, 0, "id", &id, NULL),
            PERK_ERR_MISUSE);
  out = SENTINEL_RES;
  CHECK_ERR(db, perk_select_by_id(db, "t", NULL, 2, "id", &id, &out),
            PERK_ERR_MISUSE);
  ASSERT_TRUE(out == NULL);
  const char* nullcol[] = {NULL};
  out = SENTINEL_RES;
  CHECK_ERR(db, perk_select_by_id(db, "t", nullcol, 1, "id", &id, &out),
            PERK_ERR_BADNAME);
  ASSERT_TRUE(out == NULL);
  CHECK_ERR(db, perk_select_by_id(db, NULL, NULL, 0, NULL, NULL, NULL),
            PERK_ERR_MISUSE);
  // PERK_NULL id matches nothing
  PerkValue nid = perk_null();
  out = SENTINEL_RES;
  CHECK_ERR(db, perk_select_by_id(db, "t", NULL, 0, "id", &nid, &out),
            PERK_ERR_NOT_FOUND);
  ASSERT_TRUE(out == NULL);
  ASSERT_TRUE(one_row_intact(db));
  perk_close(db);
}

static void test_update_nulls(void) {
  puts("update: NULL arguments");
  const char* cols[] = {"name"};
  PerkValue vals[] = {perk_text("zed")};
  int64_t ch = SENTINEL_I64;
  CHECK_ERR(NULL, perk_update(NULL, "t", cols, vals, 1, NULL, NULL, 0, &ch),
            PERK_ERR_MISUSE);
  CHECK_ERR(NULL, perk_update(NULL, NULL, NULL, NULL, 0, NULL, NULL, 0, NULL),
            PERK_ERR_MISUSE);
  ASSERT_EQ_INT(ch, SENTINEL_I64);

  PerkDB* db = open_seeded();
  CHECK_ERR(db, perk_update(db, NULL, cols, vals, 1, NULL, NULL, 0, &ch),
            PERK_ERR_MISUSE);
  CHECK_ERR(db, perk_update(db, "t", NULL, vals, 1, NULL, NULL, 0, &ch),
            PERK_ERR_MISUSE);
  CHECK_ERR(db, perk_update(db, "t", cols, NULL, 1, NULL, NULL, 0, &ch),
            PERK_ERR_MISUSE);
  CHECK_ERR(db, perk_update(db, "t", cols, vals, 0, NULL, NULL, 0, &ch),
            PERK_ERR_MISUSE);
  CHECK_ERR(db, perk_update(db, "t", cols, vals, 1, "id = ?", NULL, 1, &ch),
            PERK_ERR_MISUSE);
  CHECK_ERR(db, perk_update(db, NULL, NULL, NULL, 0, NULL, NULL, 0, NULL),
            PERK_ERR_MISUSE);
  const char* nullcol[] = {NULL};
  CHECK_ERR(db, perk_update(db, "t", nullcol, vals, 1, NULL, NULL, 0, &ch),
            PERK_ERR_BADNAME);
  PerkValue nulltext = {.type = PERK_TEXT, .as.text = {NULL, 4}};
  CHECK_ERR(db, perk_update(db, "t", cols, &nulltext, 1, NULL, NULL, 0, &ch),
            PERK_ERR_MISUSE);
  PerkValue wa = raw_text(NULL, 2);
  CHECK_ERR(db, perk_update(db, "t", cols, vals, 1, "name = ?", &wa, 1, &ch),
            PERK_ERR_MISUSE);
  ASSERT_EQ_INT(ch, SENTINEL_I64);
  ASSERT_TRUE(one_row_intact(db));

  // NULL where + NULL changed is legal
  CHECK_ERR(db, perk_update(db, "t", cols, vals, 1, NULL, NULL, 0, NULL),
            PERK_OK);
  CHECK_ERR(
      db,
      perk_update(db, "t", cols, vals, 1, "id = ?", &(PerkValue){0}, 1, &ch),
      PERK_ERR_NOT_FOUND);  // {0} is PERK_NULL: matches nothing
  ASSERT_EQ_INT(ch, 0);
  perk_close(db);
}

static void test_delete_nulls(void) {
  puts("delete: NULL arguments");
  int64_t ch = SENTINEL_I64;
  CHECK_ERR(NULL, perk_delete(NULL, "t", NULL, NULL, 0, &ch), PERK_ERR_MISUSE);
  CHECK_ERR(NULL, perk_delete(NULL, NULL, NULL, NULL, 0, NULL),
            PERK_ERR_MISUSE);
  ASSERT_EQ_INT(ch, SENTINEL_I64);

  PerkDB* db = open_seeded();
  CHECK_ERR(db, perk_delete(db, NULL, NULL, NULL, 0, &ch), PERK_ERR_MISUSE);
  CHECK_ERR(db, perk_delete(db, "t", "id = ?", NULL, 1, &ch), PERK_ERR_MISUSE);
  CHECK_ERR(db, perk_delete(db, NULL, NULL, NULL, 0, NULL), PERK_ERR_MISUSE);
  CHECK_ERR(db, perk_delete(db, "t", NULL, NULL, 1, &ch),
            PERK_ERR_MISUSE);  // nargs without a where
  PerkValue wa = raw_text(NULL, 2);
  CHECK_ERR(db, perk_delete(db, "t", "name = ?", &wa, 1, &ch), PERK_ERR_MISUSE);
  ASSERT_EQ_INT(ch, SENTINEL_I64);
  ASSERT_TRUE(one_row_intact(db));

  CHECK_ERR(db, perk_delete(db, "t", NULL, NULL, 0, NULL), PERK_OK);
  ASSERT_EQ_INT(count_rows(db, "t"), 0);
  CHECK_ERR(db, perk_delete(db, "t", NULL, NULL, 0, &ch), PERK_ERR_NOT_FOUND);
  ASSERT_EQ_INT(ch, 0);
  perk_close(db);
}

static void test_result_and_tx_nulls(void) {
  puts("result accessors / free / transactions: NULL and out of range");
  ASSERT_EQ_INT(perk_result_rows(NULL), 0);
  ASSERT_EQ_INT(perk_result_cols(NULL), 0);
  ASSERT_TRUE(perk_result_colname(NULL, 0) == NULL);
  ASSERT_TRUE(perk_result_get(NULL, 0, 0) == NULL);
  perk_result_free(NULL);

  CHECK_ERR(NULL, perk_begin(NULL), PERK_ERR_MISUSE);
  CHECK_ERR(NULL, perk_commit(NULL), PERK_ERR_MISUSE);
  CHECK_ERR(NULL, perk_rollback(NULL), PERK_ERR_MISUSE);

  PerkDB* db = open_seeded();
  PerkResult* r = NULL;
  CHECK_ERR(db, perk_select(db, "t", NULL, 0, NULL, NULL, 0, &r), PERK_OK);
  ASSERT_EQ_INT(perk_result_rows(r), 1);
  ASSERT_EQ_INT(perk_result_cols(r), 4);
  ASSERT_TRUE(perk_result_get(r, 1, 0) == NULL);
  ASSERT_TRUE(perk_result_get(r, 0, 4) == NULL);
  ASSERT_TRUE(perk_result_get(r, SIZE_MAX, 0) == NULL);
  ASSERT_TRUE(perk_result_get(r, 0, SIZE_MAX) == NULL);
  ASSERT_TRUE(perk_result_get(r, SIZE_MAX, SIZE_MAX) == NULL);
  ASSERT_TRUE(perk_result_colname(r, 4) == NULL);
  ASSERT_TRUE(perk_result_colname(r, SIZE_MAX) == NULL);
  ASSERT_EQ_STR(perk_result_colname(r, 3), "n");
  perk_result_free(r);

  // Transaction misuse on a valid handle
  CHECK_ERR(db, perk_commit(db), PERK_ERR_MISUSE);
  CHECK_ERR(db, perk_rollback(db), PERK_ERR_MISUSE);
  CHECK_ERR(db, perk_begin(db), PERK_OK);
  CHECK_ERR(db, perk_begin(db), PERK_ERR_MISUSE);
  CHECK_ERR(db, perk_rollback(db), PERK_OK);
  ASSERT_TRUE(one_row_intact(db));
  perk_close(db);
}

/* ------------------------------------------------------ 2. empty strings */

static void test_empty_strings(void) {
  puts("empty strings");
  PerkDB* db = open_seeded();
  PerkColumn c1[] = {{"a", NULL, 0}};

  // "" for names
  CHECK_ERR(db, perk_create_table(db, "", c1, 1), PERK_ERR_BADNAME);
  PerkColumn emptycol[] = {{"", NULL, 0}};
  CHECK_ERR(db, perk_create_table(db, "x", emptycol, 1), PERK_ERR_BADNAME);
  PerkColumn emptytype[] = {{"a", "", 0}};
  CHECK_ERR(db, perk_create_table(db, "x", emptytype, 1), PERK_ERR_BADNAME);

  const char* ecol[] = {""};
  PerkValue v = perk_int(1);
  CHECK_ERR(db, perk_insert(db, "", ecol, &v, 1, NULL), PERK_ERR_BADNAME);
  CHECK_ERR(db, perk_insert(db, "t", ecol, &v, 1, NULL), PERK_ERR_BADNAME);
  PerkResult* r = SENTINEL_RES;
  CHECK_ERR(db, perk_select(db, "", NULL, 0, NULL, NULL, 0, &r),
            PERK_ERR_BADNAME);
  ASSERT_TRUE(r == NULL);
  r = SENTINEL_RES;
  CHECK_ERR(db, perk_select(db, "t", ecol, 1, NULL, NULL, 0, &r),
            PERK_ERR_BADNAME);
  ASSERT_TRUE(r == NULL);
  r = SENTINEL_RES;
  CHECK_ERR(db, perk_select_by_id(db, "", NULL, 0, "id", &v, &r),
            PERK_ERR_BADNAME);
  ASSERT_TRUE(r == NULL);
  r = SENTINEL_RES;
  CHECK_ERR(db, perk_select_by_id(db, "t", NULL, 0, "", &v, &r),
            PERK_ERR_BADNAME);
  ASSERT_TRUE(r == NULL);
  CHECK_ERR(db, perk_update(db, "", ecol, &v, 1, NULL, NULL, 0, NULL),
            PERK_ERR_BADNAME);
  CHECK_ERR(db, perk_update(db, "t", ecol, &v, 1, NULL, NULL, 0, NULL),
            PERK_ERR_BADNAME);
  CHECK_ERR(db, perk_delete(db, "", NULL, NULL, 0, NULL), PERK_ERR_BADNAME);
  ASSERT_TRUE(one_row_intact(db));

  // "" where lists / updates / deletes every row (same as NULL)
  ASSERT_EQ_INT(put(db, 2, "bob", 5), PERK_OK);
  r = NULL;
  CHECK_ERR(db, perk_select(db, "t", NULL, 0, "", NULL, 0, &r), PERK_OK);
  ASSERT_EQ_INT(perk_result_rows(r), 2);
  perk_result_free(r);
  CHECK_ERR(db, perk_select(db, "t", NULL, 0, "   \t\n", NULL, 0, &r), PERK_OK);
  ASSERT_EQ_INT(perk_result_rows(r), 2);
  perk_result_free(r);
  int64_t ch = SENTINEL_I64;
  const char* ncol[] = {"n"};
  PerkValue nv = perk_int(9);
  CHECK_ERR(db, perk_update(db, "t", ncol, &nv, 1, "", NULL, 0, &ch), PERK_OK);
  ASSERT_EQ_INT(ch, 2);

  // "" text values and zero-length blobs round trip
  const char* cols[] = {"id", "name", "data"};
  PerkValue ev[] = {perk_int(10), perk_text(""), perk_blob("", 0)};
  CHECK_ERR(db, perk_insert(db, "t", cols, ev, 3, NULL), PERK_OK);
  PerkValue id = perk_int(10);
  r = NULL;
  CHECK_ERR(db, perk_select_by_id(db, "t", NULL, 0, "id", &id, &r), PERK_OK);
  const PerkValue* name = perk_result_get(r, 0, 1);
  const PerkValue* data = perk_result_get(r, 0, 2);
  ASSERT_TRUE(name != NULL && name->type == PERK_TEXT);
  if (name && name->type == PERK_TEXT) {
    ASSERT_EQ_INT(name->as.text.len, 0);
    ASSERT_TRUE(name->as.text.ptr != NULL);
    ASSERT_EQ_STR(name->as.text.ptr, "");
  }
  ASSERT_TRUE(data != NULL);
  if (data) {
    ASSERT_EQ_INT(data->type, PERK_BLOB);
    if (data->type == PERK_BLOB) ASSERT_EQ_INT(data->as.blob.len, 0);
  }
  perk_result_free(r);

  // Blob with NULL pointer and len 0 is legal; text "" is matched by arg
  PerkValue nb = perk_blob(NULL, 0);
  PerkValue ev2[] = {perk_int(11), nb};
  const char* cols2[] = {"id", "data"};
  CHECK_ERR(db, perk_insert(db, "t", cols2, ev2, 2, NULL), PERK_OK);
  PerkValue empty = perk_text("");
  r = NULL;
  CHECK_ERR(db, perk_select(db, "t", NULL, 0, "name = ?", &empty, 1, &r),
            PERK_OK);
  ASSERT_EQ_INT(perk_result_rows(r), 1);
  perk_result_free(r);
  CHECK_ERR(db, perk_delete(db, "t", "name = ?", &empty, 1, &ch), PERK_OK);
  ASSERT_EQ_INT(ch, 1);
  // PERK_TEXT len 0 with a real pointer vs perk_text(NULL) (SQL NULL)
  PerkValue t0 = raw_text("x", 0);
  CHECK_ERR(db,
            perk_insert(db, "t", (const char*[]){"id", "name"},
                        (PerkValue[]){perk_int(12), t0}, 2, NULL),
            PERK_OK);
  PerkValue id12 = perk_int(12);
  CHECK_ERR(db, perk_select_by_id(db, "t", NULL, 0, "id", &id12, &r), PERK_OK);
  name = perk_result_get(r, 0, 1);
  ASSERT_TRUE(name != NULL && name->type == PERK_TEXT);
  if (name && name->type == PERK_TEXT) ASSERT_EQ_INT(name->as.text.len, 0);
  perk_result_free(r);
  ASSERT_EQ_INT(perk_text(NULL).type, PERK_NULL);
  ASSERT_EQ_INT(perk_text("").type, PERK_TEXT);
  ASSERT_EQ_INT(perk_text("").as.text.len, 0);
  perk_close(db);
}

/* --------------------------------------------- 3. max-length identifiers */

static void test_max_identifiers(void) {
  puts("identifiers: 64 accepted, 65 rejected");
  char n64[65], n65[66], c64[65], c65[66];
  make_name(n64, 64);
  make_name(n65, 65);
  memset(c64, 'c', 64), c64[64] = '\0';
  memset(c65, 'c', 65), c65[65] = '\0';
  ASSERT_EQ_INT(validate_identifier(n64), 1);
  ASSERT_EQ_INT(validate_identifier(n65), 0);

  PerkDB* db = open_seeded();
  PerkColumn cols[] = {{c64, "INTEGER", PERK_COL_PK}, {"v", "TEXT", 0}};
  CHECK_ERR(db, perk_create_table(db, n64, cols, 2), PERK_OK);
  CHECK_ERR(db, perk_create_table(db, n64, cols, 2), PERK_ERR_EXISTS);

  // Full lifecycle with 64-char table and column names
  const char* icols[] = {c64, "v"};
  PerkValue ivals[] = {perk_int(1), perk_text("hello")};
  int64_t rid = 0;
  CHECK_ERR(db, perk_insert(db, n64, icols, ivals, 2, &rid), PERK_OK);
  ASSERT_EQ_INT(rid, 1);
  char where[200];
  snprintf(where, sizeof(where), "%s = ? AND v IS NOT NULL", c64);
  PerkValue one = perk_int(1);
  PerkResult* r = NULL;
  CHECK_ERR(db, perk_select(db, n64, icols, 2, where, &one, 1, &r), PERK_OK);
  ASSERT_EQ_INT(perk_result_rows(r), 1);
  ASSERT_EQ_STR(perk_result_colname(r, 0), c64);
  perk_result_free(r);
  CHECK_ERR(db, perk_select_by_id(db, n64, NULL, 0, c64, &one, &r), PERK_OK);
  ASSERT_EQ_STR(perk_result_colname(r, 0), c64);
  perk_result_free(r);
  const char* scol[] = {"v"};
  PerkValue nv = perk_text("bye");
  int64_t ch = 0;
  snprintf(where, sizeof(where), "%s = ?", c64);
  CHECK_ERR(db, perk_update(db, n64, scol, &nv, 1, where, &one, 1, &ch),
            PERK_OK);
  ASSERT_EQ_INT(ch, 1);
  const char* upd64[] = {c64};
  PerkValue newid = perk_int(2);
  CHECK_ERR(db, perk_update(db, n64, upd64, &newid, 1, NULL, NULL, 0, &ch),
            PERK_OK);
  PerkValue two = perk_int(2);
  CHECK_ERR(db, perk_delete(db, n64, where, &two, 1, &ch), PERK_OK);
  ASSERT_EQ_INT(ch, 1);
  ASSERT_EQ_INT(count_rows(db, n64), 0);

  // 65 characters is rejected in every identifier position
  PerkColumn bad_tbl[] = {{"a", NULL, 0}};
  CHECK_ERR(db, perk_create_table(db, n65, bad_tbl, 1), PERK_ERR_BADNAME);
  PerkColumn bad_col[] = {{c65, NULL, 0}};
  CHECK_ERR(db, perk_create_table(db, "ok_t", bad_col, 1), PERK_ERR_BADNAME);
  PerkColumn bad_col2[] = {{"fine", NULL, 0}, {c65, NULL, 0}};
  CHECK_ERR(db, perk_create_table(db, "ok_t", bad_col2, 2), PERK_ERR_BADNAME);

  const char* b65[] = {c65};
  const char* name_col[] = {"name"};
  PerkValue v = perk_text("x");
  CHECK_ERR(db, perk_insert(db, n65, name_col, &v, 1, NULL), PERK_ERR_BADNAME);
  CHECK_ERR(db, perk_insert(db, "t", b65, &v, 1, NULL), PERK_ERR_BADNAME);

  r = SENTINEL_RES;
  CHECK_ERR(db, perk_select(db, n65, NULL, 0, NULL, NULL, 0, &r),
            PERK_ERR_BADNAME);
  ASSERT_TRUE(r == NULL);
  r = SENTINEL_RES;
  CHECK_ERR(db, perk_select(db, "t", b65, 1, NULL, NULL, 0, &r),
            PERK_ERR_BADNAME);
  ASSERT_TRUE(r == NULL);
  char w65[200];
  snprintf(w65, sizeof(w65), "%s = ?", c65);
  r = SENTINEL_RES;
  CHECK_ERR(db, perk_select(db, "t", NULL, 0, w65, &one, 1, &r),
            PERK_ERR_BADNAME);
  ASSERT_TRUE(r == NULL);

  r = SENTINEL_RES;
  CHECK_ERR(db, perk_select_by_id(db, n65, NULL, 0, "id", &one, &r),
            PERK_ERR_BADNAME);
  ASSERT_TRUE(r == NULL);
  r = SENTINEL_RES;
  CHECK_ERR(db, perk_select_by_id(db, "t", b65, 1, "id", &one, &r),
            PERK_ERR_BADNAME);
  ASSERT_TRUE(r == NULL);
  r = SENTINEL_RES;
  CHECK_ERR(db, perk_select_by_id(db, "t", NULL, 0, c65, &one, &r),
            PERK_ERR_BADNAME);
  ASSERT_TRUE(r == NULL);

  ch = SENTINEL_I64;
  CHECK_ERR(db, perk_update(db, n65, name_col, &v, 1, NULL, NULL, 0, &ch),
            PERK_ERR_BADNAME);
  CHECK_ERR(db, perk_update(db, "t", b65, &v, 1, NULL, NULL, 0, &ch),
            PERK_ERR_BADNAME);
  CHECK_ERR(db, perk_update(db, "t", name_col, &v, 1, w65, &one, 1, &ch),
            PERK_ERR_BADNAME);
  CHECK_ERR(db, perk_delete(db, n65, NULL, NULL, 0, &ch), PERK_ERR_BADNAME);
  CHECK_ERR(db, perk_delete(db, "t", w65, &one, 1, &ch), PERK_ERR_BADNAME);
  ASSERT_EQ_INT(ch, SENTINEL_I64);
  ASSERT_TRUE(one_row_intact(db));
  perk_close(db);
}

/* ------------------------------------------------ 4. oversized strings */

/** Calls every value-binding function with one oversized value. */
static void expect_oversized_rejected(PerkDB* db, PerkValue big,
                                      const char* label) {
  printf("  oversized: %s\n", label);
  const char* col = big.type == PERK_BLOB ? "data" : "name";
  const char* cols[1];
  cols[0] = col;
  long long rows_before = count_rows(db, "t");

  int64_t rid = SENTINEL_I64;
  CHECK_ERR(db, perk_insert(db, "t", cols, &big, 1, &rid), PERK_ERR_SQL);
  ASSERT_EQ_INT(rid, SENTINEL_I64);
  // big in the middle of several values
  const char* c3[] = {"id", col, "n"};
  PerkValue v3[] = {perk_int(50), big, perk_int(1)};
  CHECK_ERR(db, perk_insert(db, "t", c3, v3, 3, &rid), PERK_ERR_SQL);
  ASSERT_EQ_INT(rid, SENTINEL_I64);

  PerkResult* r = SENTINEL_RES;
  CHECK_ERR(db, perk_select(db, "t", NULL, 0, "name = ?", &big, 1, &r),
            PERK_ERR_SQL);
  ASSERT_TRUE(r == NULL);
  r = SENTINEL_RES;
  CHECK_ERR(db, perk_select_by_id(db, "t", NULL, 0, "id", &big, &r),
            PERK_ERR_SQL);
  ASSERT_TRUE(r == NULL);

  int64_t ch = SENTINEL_I64;
  CHECK_ERR(db, perk_update(db, "t", cols, &big, 1, NULL, NULL, 0, &ch),
            PERK_ERR_SQL);
  CHECK_ERR(db,
            perk_update(db, "t", (const char*[]){"n"}, &(PerkValue){0}, 1,
                        "name = ?", &big, 1, &ch),
            PERK_ERR_SQL);
  CHECK_ERR(db, perk_delete(db, "t", "name = ?", &big, 1, &ch), PERK_ERR_SQL);
  ASSERT_EQ_INT(ch, SENTINEL_I64);

  ASSERT_EQ_INT(count_rows(db, "t"), rows_before);
  ASSERT_TRUE(one_row_intact(db));
}

static void test_oversized_lengths(void) {
  puts("oversized lengths (pointer must not be read)");
  PerkDB* db = open_seeded();
  int limit = sqlite3_limit(db->conn, SQLITE_LIMIT_LENGTH, -1);
  ASSERT_TRUE(limit > 0);
  char tiny[4] = {'a', 'b', 'c', '\0'};  // an over-read here trips ASan
  size_t lens[] = {SIZE_MAX,         (size_t)1 << 63,     ((size_t)1 << 63) - 1,
                   (size_t)1 << 32,  (size_t)INT_MAX + 1, (size_t)INT_MAX,
                   (size_t)limit + 1};
  const char* labels[] = {"SIZE_MAX",  "2^63",    "2^63-1", "2^32",
                          "INT_MAX+1", "INT_MAX", "limit+1"};
  for (size_t i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
    char lab[64];
    snprintf(lab, sizeof(lab), "text %s", labels[i]);
    expect_oversized_rejected(db, raw_text(tiny, lens[i]), lab);
    snprintf(lab, sizeof(lab), "blob %s", labels[i]);
    expect_oversized_rejected(db, perk_blob(tiny, lens[i]), lab);
  }
  // Also a NULL-pointer blob with a huge length: rejected without touching it
  PerkValue nb = perk_blob(NULL, SIZE_MAX);
  const char* dcol[] = {"data"};
  CHECK_ERR(db, perk_insert(db, "t", dcol, &nb, 1, NULL), PERK_ERR_MISUSE);
  ASSERT_TRUE(one_row_intact(db));
  perk_close(db);
}

static void test_lowered_limit(void) {
  puts("lowered SQLITE_LIMIT_LENGTH (200)");
  PerkDB* db = open_seeded();
  sqlite3_limit(db->conn, SQLITE_LIMIT_LENGTH, 200);
  ASSERT_EQ_INT(sqlite3_limit(db->conn, SQLITE_LIMIT_LENGTH, -1), 200);

  char buf[256];
  memset(buf, 'x', sizeof(buf));
  char sbuf[256];
  memset(sbuf, 'y', sizeof(sbuf));

  // Well below the limit (room for the record header): stored and read back
  const char* name_col[] = {"id", "name"};
  PerkValue vals[] = {perk_int(2), raw_text(buf, 150)};
  CHECK_ERR(db, perk_insert(db, "t", name_col, vals, 2, NULL), PERK_OK);
  PerkValue id2 = perk_int(2);
  PerkResult* r = NULL;
  CHECK_ERR(db, perk_select_by_id(db, "t", NULL, 0, "id", &id2, &r), PERK_OK);
  const PerkValue* got = perk_result_get(r, 0, 1);
  ASSERT_TRUE(got && got->type == PERK_TEXT && got->as.text.len == 150 &&
              memcmp(got->as.text.ptr, buf, 150) == 0);
  perk_result_free(r);

  // len == limit: not rejected by the length check. Bound as a comparison
  // argument it succeeds; as an insert value it may exceed the row-size limit
  // and then must fail cleanly at step, not with the "too big" message.
  PerkValue at = raw_text(buf, 200);
  r = NULL;
  CHECK_ERR(db, perk_select(db, "t", NULL, 0, "name = ?", &at, 1, &r), PERK_OK);
  ASSERT_EQ_INT(perk_result_rows(r), 0);
  perk_result_free(r);
  int64_t ch = SENTINEL_I64;
  CHECK_ERR(db, perk_delete(db, "t", "name = ?", &at, 1, &ch),
            PERK_ERR_NOT_FOUND);
  PerkValue ab = perk_blob(sbuf, 200);
  CHECK_ERR(db, perk_select(db, "t", NULL, 0, "data = ?", &ab, 1, &r), PERK_OK);
  perk_result_free(r);

  const char* ncol[] = {"name"};
  PerkErr e = perk_insert(db, "t", ncol, &at, 1, NULL);
  ASSERT_TRUE(e == PERK_OK || e == PERK_ERR_SQL);
  if (e == PERK_ERR_SQL)
    ASSERT_TRUE(strstr(perk_errmsg(db), "value is too big") == NULL);
  const char* bcol[] = {"data"};
  e = perk_insert(db, "t", bcol, &ab, 1, NULL);
  ASSERT_TRUE(e == PERK_OK || e == PERK_ERR_SQL);
  if (e == PERK_ERR_SQL)
    ASSERT_TRUE(strstr(perk_errmsg(db), "value is too big") == NULL);

  // limit + 1 is rejected up front, by every function
  PerkValue over_t = raw_text(buf, 201);
  PerkValue over_b = perk_blob(sbuf, 201);
  long long rows = count_rows(db, "t");
  int64_t rid = SENTINEL_I64;
  ch = SENTINEL_I64;
  CHECK_ERR(db, perk_insert(db, "t", ncol, &over_t, 1, &rid), PERK_ERR_SQL);
  ASSERT_TRUE(strstr(perk_errmsg(db), "value is too big") != NULL);
  CHECK_ERR(db, perk_insert(db, "t", bcol, &over_b, 1, &rid), PERK_ERR_SQL);
  CHECK_ERR(db, perk_select(db, "t", NULL, 0, "name = ?", &over_t, 1, &r),
            PERK_ERR_SQL);
  CHECK_ERR(db, perk_select_by_id(db, "t", NULL, 0, "id", &over_b, &r),
            PERK_ERR_SQL);
  CHECK_ERR(db, perk_update(db, "t", ncol, &over_t, 1, NULL, NULL, 0, &ch),
            PERK_ERR_SQL);
  CHECK_ERR(db, perk_delete(db, "t", "data = ?", &over_b, 1, &ch),
            PERK_ERR_SQL);
  ASSERT_EQ_INT(rid, SENTINEL_I64);
  ASSERT_EQ_INT(ch, SENTINEL_I64);
  ASSERT_EQ_INT(count_rows(db, "t"), rows);

  // The length check follows the connection's limit when raised again
  sqlite3_limit(db->conn, SQLITE_LIMIT_LENGTH, 100000);
  PerkValue ok = raw_text(buf, 201);
  CHECK_ERR(db, perk_select(db, "t", NULL, 0, "name = ?", &ok, 1, &r), PERK_OK);
  perk_result_free(r);
  perk_close(db);
}

static void test_large_valid_values(void) {
  puts("large valid values and a very long WHERE");
  PerkDB* db = open_seeded();
  const size_t big = 5u * 1024 * 1024;
  char* text = malloc(big + 1);
  unsigned char* blob = malloc(big);
  if (!text || !blob) {
    puts("FATAL: malloc");
    exit(2);
  }
  for (size_t i = 0; i < big; i++) {
    text[i] = (char)('a' + (i % 26));
    blob[i] = (unsigned char)(i * 31u);  // includes embedded zero bytes
  }
  text[big] = '\0';
  const char* cols[] = {"id", "name", "data"};
  PerkValue vals[] = {perk_int(5), perk_text(text), perk_blob(blob, big)};
  CHECK_ERR(db, perk_insert(db, "t", cols, vals, 3, NULL), PERK_OK);
  PerkValue id = perk_int(5);
  PerkResult* r = NULL;
  CHECK_ERR(db, perk_select_by_id(db, "t", NULL, 0, "id", &id, &r), PERK_OK);
  const PerkValue* t = perk_result_get(r, 0, 1);
  const PerkValue* b = perk_result_get(r, 0, 2);
  ASSERT_TRUE(t && t->type == PERK_TEXT && t->as.text.len == big &&
              memcmp(t->as.text.ptr, text, big) == 0 &&
              t->as.text.ptr[big] == '\0');
  ASSERT_TRUE(b && b->type == PERK_BLOB && b->as.blob.len == big &&
              memcmp(b->as.blob.ptr, blob, big) == 0);
  perk_result_free(r);

  // Matching on the big value through a WHERE argument
  PerkValue arg = perk_text(text);
  CHECK_ERR(db, perk_select(db, "t", NULL, 0, "name = ?", &arg, 1, &r),
            PERK_OK);
  ASSERT_EQ_INT(perk_result_rows(r), 1);
  perk_result_free(r);

  // Update a big value in place, delete it
  int64_t ch = 0;
  const char* ncol[] = {"name"};
  PerkValue small = perk_text("tiny");
  CHECK_ERR(db, perk_update(db, "t", ncol, &small, 1, "id = ?", &id, 1, &ch),
            PERK_OK);
  ASSERT_EQ_INT(ch, 1);
  CHECK_ERR(db, perk_delete(db, "t", "id = ?", &id, 1, &ch), PERK_OK);
  free(text);
  free(blob);

  // A 1 MB WHERE: a single long "name" is rejected, so is 1 MB of '('
  size_t wl = 1024 * 1024;
  char* w = malloc(wl + 1);
  if (!w) exit(2);
  memset(w, 'a', wl);
  w[wl] = '\0';
  r = SENTINEL_RES;
  PerkErr e = perk_select(db, "t", NULL, 0, w, NULL, 0, &r);
  ASSERT_EQ_INT(e, PERK_ERR_BADNAME);
  ASSERT_TRUE(r == NULL);
  memset(w, '(', wl);
  r = SENTINEL_RES;
  e = perk_select(db, "t", NULL, 0, w, NULL, 0, &r);
  ASSERT_EQ_INT(e, PERK_ERR_MISUSE);
  ASSERT_TRUE(r == NULL);
  CHECK_ERR(db, perk_delete(db, "t", w, NULL, 0, &ch), PERK_ERR_MISUSE);
  CHECK_ERR(db, perk_update(db, "t", ncol, &small, 1, w, NULL, 0, &ch),
            PERK_ERR_MISUSE);
  // 1 MB of blanks followed by a real filter is a valid WHERE
  memset(w, ' ', wl);
  memcpy(w + wl - 6, "id = ?", 6);
  PerkValue one = perk_int(1);
  r = NULL;
  CHECK_ERR(db, perk_select(db, "t", NULL, 0, w, &one, 1, &r), PERK_OK);
  ASSERT_EQ_INT(perk_result_rows(r), 1);
  perk_result_free(r);
  // 1 MB of "?" placeholders: count mismatch, not a crash
  memset(w, '?', wl);
  r = SENTINEL_RES;
  e = perk_select(db, "t", NULL, 0, w, &one, 1, &r);
  ASSERT_TRUE(e != PERK_OK);
  ASSERT_TRUE(r == NULL);
  free(w);

  // Very long identifier as table / column (1 MB, no NUL until the end)
  char* nm = malloc(wl + 1);
  if (!nm) exit(2);
  memset(nm, 'n', wl);
  nm[wl] = '\0';
  const char* lc[] = {nm};
  CHECK_ERR(db, perk_insert(db, nm, lc, &one, 1, NULL), PERK_ERR_BADNAME);
  CHECK_ERR(db, perk_insert(db, "t", lc, &one, 1, NULL), PERK_ERR_BADNAME);
  PerkColumn lcol[] = {{nm, NULL, 0}};
  CHECK_ERR(db, perk_create_table(db, "z", lcol, 1), PERK_ERR_BADNAME);
  free(nm);
  perk_close(db);
}

/* ------------------------------------------------------ 6. lifecycle */

static void test_lifecycle(void) {
  puts("lifecycle: close with open transaction and live results");
  const char* path = "build/test_memsafety.db";
  char wal[128], shm[128];
  snprintf(wal, sizeof(wal), "%s-wal", path);
  snprintf(shm, sizeof(shm), "%s-shm", path);
  unlink(path), unlink(wal), unlink(shm);

  PerkDB* db = NULL;
  CHECK_ERR(NULL, perk_open(path, &db), PERK_OK);
  if (db == NULL) {
    unlink(path), unlink(wal), unlink(shm);
    return;
  }
  PerkColumn cols[] = {{"id", "INTEGER", PERK_COL_PK}, {"name", "TEXT", 0}};
  CHECK_ERR(db, perk_create_table(db, "t", cols, 2), PERK_OK);
  const char* ic[] = {"id", "name"};
  PerkValue iv[] = {perk_int(1), perk_text("committed")};
  CHECK_ERR(db, perk_insert(db, "t", ic, iv, 2, NULL), PERK_OK);

  PerkResult* live = NULL;
  CHECK_ERR(db, perk_select(db, "t", NULL, 0, NULL, NULL, 0, &live), PERK_OK);
  PerkResult* by_id = NULL;
  PerkValue one = perk_int(1);
  CHECK_ERR(db, perk_select_by_id(db, "t", NULL, 0, "id", &one, &by_id),
            PERK_OK);

  CHECK_ERR(db, perk_begin(db), PERK_OK);
  PerkValue iv2[] = {perk_int(2), perk_text("uncommitted")};
  CHECK_ERR(db, perk_insert(db, "t", ic, iv2, 2, NULL), PERK_OK);
  perk_close(db);  // rolls the open transaction back

  // Results outlive the handle: readable, then freed after the close
  ASSERT_EQ_INT(perk_result_rows(live), 1);
  ASSERT_EQ_INT(perk_result_cols(live), 2);
  ASSERT_EQ_STR(perk_result_colname(live, 1), "name");
  const PerkValue* v = perk_result_get(live, 0, 1);
  ASSERT_TRUE(v && v->type == PERK_TEXT);
  if (v && v->type == PERK_TEXT) ASSERT_EQ_STR(v->as.text.ptr, "committed");
  v = perk_result_get(by_id, 0, 1);
  ASSERT_TRUE(v && v->type == PERK_TEXT);
  perk_result_free(live);
  perk_result_free(by_id);

  // Reopen: the uncommitted row is gone, the committed one is there
  db = NULL;
  CHECK_ERR(NULL, perk_open(path, &db), PERK_OK);
  if (db) {
    ASSERT_EQ_INT(count_rows(db, "t"), 1);
    // Repeated failed selects leave out == NULL and the handle usable
    for (int i = 0; i < 20; i++) {
      PerkResult* r = SENTINEL_RES;
      CHECK_ERR(db, perk_select(db, "nope", NULL, 0, NULL, NULL, 0, &r),
                PERK_ERR_SQL);
      ASSERT_TRUE(r == NULL);
    }
    ASSERT_EQ_INT(count_rows(db, "t"), 1);
    perk_close(db);
  }
  // Close and reopen the file repeatedly
  for (int i = 0; i < 10; i++) {
    PerkDB* again = NULL;
    CHECK_ERR(NULL, perk_open(path, &again), PERK_OK);
    if (again == NULL) break;
    ASSERT_EQ_INT(count_rows(again, "t"), 1);
    perk_close(again);
  }
  unlink(path), unlink(wal), unlink(shm);

  // Many open/close cycles of the in-memory handle with a rollback pending
  for (int i = 0; i < 50; i++) {
    PerkDB* m = open_seeded();
    CHECK_ERR(m, perk_begin(m), PERK_OK);
    CHECK_ERR(m, put(m, 100, "tmp", 1), PERK_OK);
    perk_close(m);
  }
}

int main(void) {
  test_open_nulls();
  test_close_errmsg_nulls();
  test_create_table_nulls();
  test_insert_nulls();
  test_select_nulls();
  test_select_by_id_nulls();
  test_update_nulls();
  test_delete_nulls();
  test_result_and_tx_nulls();
  test_empty_strings();
  test_max_identifiers();
  test_oversized_lengths();
  test_lowered_limit();
  test_large_valid_values();
  test_lifecycle();
  return harness_summary();
}
