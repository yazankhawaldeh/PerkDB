// Tests for insert / read (list-all, WHERE, by-id) / update / delete.
//
// For every operation: happy path, missing rows (PERK_ERR_NOT_FOUND where the
// API defines it), NULL arguments (PERK_ERR_MISUSE), empty and invalid
// identifiers (PERK_ERR_BADNAME), injection-shaped values (stored and read
// back literally, never executed), and "data unchanged after a failed
// operation" checks that compare a full snapshot of the table.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "perkdb.h"
#include "test_harness.h"

// Checks a PerkErr result and, on failure, also prints the last error message
#define CHECK_ERR(db, expr, expected)                                          \
  do {                                                                         \
    PerkErr e_ = (expr);                                                       \
    if (!harness_eq_int(__FILE__, __LINE__, #expr, #expected, e_, (expected))) \
      printf("  errmsg:   %s\n", perk_errmsg(db));                             \
  } while (0)

#define SNAP_CAP 8192

// Asserts the whole users table still matches an earlier snapshot()
#define CHECK_UNCHANGED(db, before)     \
  do {                                  \
    char now_[SNAP_CAP];                \
    snapshot((db), now_, sizeof(now_)); \
    ASSERT_EQ_STR(now_, (before));      \
  } while (0)

// Injection-shaped strings used as values
static const char* const kInjections[] = {
    "x'); DROP TABLE users; --",
    "' OR '1'='1",
    "Robert'); DROP TABLE users;--",
    "\" OR \"\"=\"",
    "1; DELETE FROM users",
    "%' UNION SELECT * FROM sqlite_master --",
    "?",
    "\\'; --",
};
#define N_INJECTIONS (sizeof(kInjections) / sizeof(kInjections[0]))

// Identifiers that must be rejected with PERK_ERR_BADNAME
static const char* const kBadNames[] = {
    "",
    " ",
    "1abc",
    "a b",
    "a-b",
    "a.b",
    "main.users",
    "sqlite_master",
    "users; DROP TABLE users; --",
    "users\" (x); --",
    "t' OR '1'='1",
    "[users]",
    "users/*c*/",
    "users\n",
};
#define N_BAD_NAMES (sizeof(kBadNames) / sizeof(kBadNames[0]))

static const PerkColumn kUsers[] = {
    {"id", "INTEGER", PERK_COL_PK}, {"name", "TEXT", 0}, {"age", "INT", 0},
    {"score", "REAL", 0},           {"data", "BLOB", 0},
};

static const PerkColumn kUniq[] = {
    {"id", "INTEGER", PERK_COL_PK},
    {"name", "TEXT", PERK_COL_NOTNULL | PERK_COL_UNIQUE},
};

/**
 * Opens a fresh in-memory database holding an empty "users" table.
 * @return the handle, or NULL (after recording a failed check) on error
 */
static PerkDB* open_users(void) {
  PerkDB* db = NULL;
  if (!ASSERT_EQ_INT(perk_open(":memory:", &db), PERK_OK)) {
    printf("  open failed: %s\n", perk_errmsg(NULL));
    return NULL;
  }
  if (!ASSERT_EQ_INT(perk_create_table(db, "users", kUsers, 5), PERK_OK)) {
    printf("  create failed: %s\n", perk_errmsg(db));
    perk_close(db);
    return NULL;
  }
  return db;
}

/**
 * Inserts one (name, age) row into users.
 * @return the new id, or -1 if the insert failed
 */
static int64_t add_user(PerkDB* db, const char* name, int64_t age) {
  const char* cols[] = {"name", "age"};
  PerkValue vals[] = {perk_text(name), perk_int(age)};
  int64_t id = -1;
  if (perk_insert(db, "users", cols, vals, 2, &id) != PERK_OK) return -1;
  return id;
}

/** Seeds alice/30 (id 1), bob/25 (id 2), carol/30 (id 3). */
static void seed(PerkDB* db) {
  ASSERT_EQ_INT(add_user(db, "alice", 30), 1);
  ASSERT_EQ_INT(add_user(db, "bob", 25), 2);
  ASSERT_EQ_INT(add_user(db, "carol", 30), 3);
}

/**
 * @return number of rows in table (list-all through the public API), or -1
 */
static long long count_rows(PerkDB* db, const char* table) {
  PerkResult* res = NULL;
  if (perk_select(db, table, NULL, 0, NULL, NULL, 0, &res) != PERK_OK)
    return -1;
  long long n = (long long)perk_result_rows(res);
  perk_result_free(res);
  return n;
}

/**
 * Writes every row and cell of the users table into buf in a canonical text
 * form (type tag + value), so two snapshots compare with strcmp.
 */
static void snapshot(PerkDB* db, char* buf, size_t cap) {
  buf[0] = '\0';
  PerkResult* res = NULL;
  if (perk_select(db, "users", NULL, 0, NULL, NULL, 0, &res) != PERK_OK) {
    snprintf(buf, cap, "<select failed>");
    return;
  }
  size_t used = 0;
  for (size_t r = 0; r < perk_result_rows(res) && used + 1 < cap; r++) {
    for (size_t c = 0; c < perk_result_cols(res) && used + 1 < cap; c++) {
      const PerkValue* v = perk_result_get(res, r, c);
      int n = 0;
      if (v == NULL) {
        n = snprintf(buf + used, cap - used, "?|");
      } else if (v->type == PERK_NULL) {
        n = snprintf(buf + used, cap - used, "N|");
      } else if (v->type == PERK_INT) {
        n = snprintf(buf + used, cap - used, "i%lld|", (long long)v->as.i);
      } else if (v->type == PERK_REAL) {
        n = snprintf(buf + used, cap - used, "r%.17g|", v->as.r);
      } else if (v->type == PERK_TEXT) {
        n = snprintf(buf + used, cap - used, "t%zu:%s|", v->as.text.len,
                     v->as.text.ptr);
      } else {
        n = snprintf(buf + used, cap - used, "b%zu|", v->as.blob.len);
      }
      if (n < 0) break;
      used += (size_t)n;
    }
    if (used + 1 < cap) {
      buf[used++] = '\n';
      buf[used] = '\0';
    }
  }
  perk_result_free(res);
}

/**
 * Fetches one column of the row with the given id.
 * @param res receives the result; the caller frees it (even on failure it is
 *        NULL, so freeing is safe)
 * @return the cell, or NULL if the row does not exist
 */
static const PerkValue* fetch(PerkDB* db, int64_t id, const char* col,
                              PerkResult** res) {
  const char* cols[] = {col};
  PerkValue key = perk_int(id);
  *res = NULL;
  if (perk_select_by_id(db, "users", cols, 1, "id", &key, res) != PERK_OK)
    return NULL;
  return perk_result_get(*res, 0, 0);
}

/**
 * @return 1 if the row with `id` has a TEXT `col` equal to `want`
 */
static int text_is(PerkDB* db, int64_t id, const char* col, const char* want) {
  PerkResult* res = NULL;
  const PerkValue* v = fetch(db, id, col, &res);
  int ok = v != NULL && v->type == PERK_TEXT &&
           strcmp(v->as.text.ptr, want) == 0 && v->as.text.len == strlen(want);
  perk_result_free(res);
  return ok;
}

/**
 * @return 1 if the row with `id` has an INT `col` equal to `want`
 */
static int int_is(PerkDB* db, int64_t id, const char* col, int64_t want) {
  PerkResult* res = NULL;
  const PerkValue* v = fetch(db, id, col, &res);
  int ok = v != NULL && v->type == PERK_INT && v->as.i == want;
  perk_result_free(res);
  return ok;
}

/**
 * @return 1 if the row with `id` has a NULL `col`
 */
static int is_null(PerkDB* db, int64_t id, const char* col) {
  PerkResult* res = NULL;
  const PerkValue* v = fetch(db, id, col, &res);
  int ok = v != NULL && v->type == PERK_NULL;
  perk_result_free(res);
  return ok;
}

/** @return 1 if a row with this id exists */
static int row_exists(PerkDB* db, int64_t id) {
  PerkResult* res = NULL;
  PerkValue key = perk_int(id);
  PerkErr e = perk_select_by_id(db, "users", NULL, 0, "id", &key, &res);
  perk_result_free(res);
  return e == PERK_OK;
}

// ---------------------------------------------------------------- insert ----

/** Inserts one row, reads it back, and checks rowid_out. */
static void test_insert_happy(void) {
  printf("insert: happy path\n");
  PerkDB* db = open_users();
  if (db == NULL) return;

  const char* cols[] = {"name", "age"};
  PerkValue vals[] = {perk_text("Yazan"), perk_int(21)};
  int64_t id = 0;
  CHECK_ERR(db, perk_insert(db, "users", cols, vals, 2, &id), PERK_OK);
  ASSERT_EQ_INT(id, 1);
  ASSERT_TRUE(text_is(db, 1, "name", "Yazan"));
  ASSERT_TRUE(int_is(db, 1, "age", 21));
  ASSERT_TRUE(is_null(db, 1, "score"));  // unlisted columns are NULL

  // rowid_out may be NULL
  CHECK_ERR(db, perk_insert(db, "users", cols, vals, 2, NULL), PERK_OK);
  ASSERT_EQ_INT(count_rows(db, "users"), 2);

  // Rowids keep increasing; an explicit id is honoured and reported
  const char* idcols[] = {"id", "name"};
  PerkValue idvals[] = {perk_int(100), perk_text("explicit")};
  CHECK_ERR(db, perk_insert(db, "users", idcols, idvals, 2, &id), PERK_OK);
  ASSERT_EQ_INT(id, 100);
  ASSERT_TRUE(text_is(db, 100, "name", "explicit"));
  CHECK_ERR(db, perk_insert(db, "users", cols, vals, 2, &id), PERK_OK);
  ASSERT_EQ_INT(id, 101);

  // Column order in the call does not need to match the table
  const char* rev[] = {"age", "name"};
  PerkValue rvals[] = {perk_int(9), perk_text("reversed")};
  CHECK_ERR(db, perk_insert(db, "users", rev, rvals, 2, &id), PERK_OK);
  ASSERT_TRUE(text_is(db, id, "name", "reversed"));
  ASSERT_TRUE(int_is(db, id, "age", 9));

  perk_close(db);
}

/** Every value type survives a write/read round trip. */
static void test_insert_value_types(void) {
  printf("insert: value types round trip\n");
  PerkDB* db = open_users();
  if (db == NULL) return;

  const unsigned char bytes[] = {0x00, 0x01, 0xFF, 0x00, 0x7F, 0x80};
  const char* cols[] = {"name", "age", "score", "data"};
  PerkValue vals[] = {perk_text("types"), perk_int(INT64_MIN), perk_real(-0.5),
                      perk_blob(bytes, sizeof(bytes))};
  int64_t id = 0;
  CHECK_ERR(db, perk_insert(db, "users", cols, vals, 4, &id), PERK_OK);

  PerkResult* res = NULL;
  PerkValue key = perk_int(id);
  CHECK_ERR(db, perk_select_by_id(db, "users", NULL, 0, "id", &key, &res),
            PERK_OK);
  ASSERT_EQ_INT(perk_result_rows(res), 1);
  ASSERT_EQ_INT(perk_result_cols(res), 5);
  ASSERT_EQ_STR(perk_result_colname(res, 0), "id");
  ASSERT_EQ_STR(perk_result_colname(res, 4), "data");
  const PerkValue* v = perk_result_get(res, 0, 0);
  if (ASSERT_TRUE(v != NULL)) {
    ASSERT_EQ_INT(v->type, PERK_INT);
    ASSERT_EQ_INT(v->as.i, id);
  }
  v = perk_result_get(res, 0, 1);
  if (ASSERT_TRUE(v != NULL) && ASSERT_EQ_INT(v->type, PERK_TEXT))
    ASSERT_EQ_STR(v->as.text.ptr, "types");
  v = perk_result_get(res, 0, 2);
  if (ASSERT_TRUE(v != NULL) && ASSERT_EQ_INT(v->type, PERK_INT))
    ASSERT_TRUE(v->as.i == INT64_MIN);
  v = perk_result_get(res, 0, 3);
  if (ASSERT_TRUE(v != NULL) && ASSERT_EQ_INT(v->type, PERK_REAL))
    ASSERT_TRUE(v->as.r == -0.5);
  v = perk_result_get(res, 0, 4);
  if (ASSERT_TRUE(v != NULL) && ASSERT_EQ_INT(v->type, PERK_BLOB)) {
    ASSERT_EQ_INT(v->as.blob.len, sizeof(bytes));
    ASSERT_TRUE(v->as.blob.len == sizeof(bytes) &&
                memcmp(v->as.blob.ptr, bytes, sizeof(bytes)) == 0);
  }
  perk_result_free(res);

  // INT64_MAX and a large real
  PerkValue big[] = {perk_text("big"), perk_int(INT64_MAX), perk_real(1e300)};
  const char* bcols[] = {"name", "age", "score"};
  CHECK_ERR(db, perk_insert(db, "users", bcols, big, 3, &id), PERK_OK);
  ASSERT_TRUE(int_is(db, id, "age", INT64_MAX));
  PerkResult* r2 = NULL;
  v = fetch(db, id, "score", &r2);
  if (ASSERT_TRUE(v != NULL) && ASSERT_EQ_INT(v->type, PERK_REAL))
    ASSERT_TRUE(v->as.r == 1e300);
  perk_result_free(r2);

  // Explicit NULLs: perk_null() and perk_text(NULL) both store SQL NULL
  const char* ncols[] = {"name", "age", "score"};
  PerkValue nulls[] = {perk_text(NULL), perk_null(), perk_null()};
  CHECK_ERR(db, perk_insert(db, "users", ncols, nulls, 3, &id), PERK_OK);
  ASSERT_TRUE(is_null(db, id, "name"));
  ASSERT_TRUE(is_null(db, id, "age"));
  ASSERT_TRUE(is_null(db, id, "score"));

  // Empty text is TEXT of length 0, not NULL
  PerkValue empty[] = {perk_text("")};
  const char* ecols[] = {"name"};
  CHECK_ERR(db, perk_insert(db, "users", ecols, empty, 1, &id), PERK_OK);
  ASSERT_TRUE(text_is(db, id, "name", ""));
  ASSERT_TRUE(!is_null(db, id, "name"));

  // Empty blob stays a BLOB
  PerkValue eb[] = {perk_blob(NULL, 0)};
  const char* bcol[] = {"data"};
  CHECK_ERR(db, perk_insert(db, "users", bcol, eb, 1, &id), PERK_OK);
  PerkResult* r3 = NULL;
  v = fetch(db, id, "data", &r3);
  if (ASSERT_TRUE(v != NULL) && ASSERT_EQ_INT(v->type, PERK_BLOB))
    ASSERT_EQ_INT(v->as.blob.len, 0);
  perk_result_free(r3);

  // Text with an explicit length keeps an embedded NUL
  PerkValue nul_text = {.type = PERK_TEXT, .as.text = {"a\0b", 3}};
  CHECK_ERR(db, perk_insert(db, "users", ecols, &nul_text, 1, &id), PERK_OK);
  PerkResult* r4 = NULL;
  v = fetch(db, id, "name", &r4);
  if (ASSERT_TRUE(v != NULL) && ASSERT_EQ_INT(v->type, PERK_TEXT)) {
    ASSERT_EQ_INT(v->as.text.len, 3);
    ASSERT_TRUE(v->as.text.len == 3 && memcmp(v->as.text.ptr, "a\0b", 3) == 0);
  }
  perk_result_free(r4);

  // UTF-8 and a large text value
  CHECK_ERR(db,
            perk_insert(db, "users", ecols,
                        (PerkValue[]){perk_text("h\xc3\xa9llo \xe2\x9c\x93")},
                        1, &id),
            PERK_OK);
  ASSERT_TRUE(text_is(db, id, "name", "h\xc3\xa9llo \xe2\x9c\x93"));
  static char large[100001];
  memset(large, 'x', sizeof(large) - 1);
  large[sizeof(large) - 1] = '\0';
  PerkValue lv = perk_text(large);
  CHECK_ERR(db, perk_insert(db, "users", ecols, &lv, 1, &id), PERK_OK);
  ASSERT_TRUE(text_is(db, id, "name", large));

  perk_close(db);
}

/** Injection-shaped values are stored and read back byte for byte. */
static void test_insert_injection(void) {
  printf("insert: injection-shaped values\n");
  PerkDB* db = open_users();
  if (db == NULL) return;

  const char* cols[] = {"name"};
  int64_t ids[N_INJECTIONS];
  for (size_t i = 0; i < N_INJECTIONS; i++) {
    PerkValue v = perk_text(kInjections[i]);
    CHECK_ERR(db, perk_insert(db, "users", cols, &v, 1, &ids[i]), PERK_OK);
  }
  ASSERT_EQ_INT(count_rows(db, "users"), (long long)N_INJECTIONS);
  for (size_t i = 0; i < N_INJECTIONS; i++) {
    if (!ASSERT_TRUE(text_is(db, ids[i], "name", kInjections[i])))
      printf("  value: %s\n", kInjections[i]);
  }

  // Injection text in a numeric column is stored as text, nothing runs
  const char* acols[] = {"age"};
  PerkValue av = perk_text("1); DROP TABLE users; --");
  int64_t id = 0;
  CHECK_ERR(db, perk_insert(db, "users", acols, &av, 1, &id), PERK_OK);
  ASSERT_TRUE(text_is(db, id, "age", "1); DROP TABLE users; --"));
  ASSERT_EQ_INT(count_rows(db, "users"), (long long)N_INJECTIONS + 1);

  perk_close(db);
}

/** NULL / empty / invalid arguments are rejected and add no rows. */
static void test_insert_errors(void) {
  printf("insert: errors\n");
  const char* cols[] = {"name"};
  PerkValue vals[] = {perk_text("x")};
  ASSERT_EQ_INT(perk_insert(NULL, "users", cols, vals, 1, NULL),
                PERK_ERR_MISUSE);

  PerkDB* db = open_users();
  if (db == NULL) return;
  seed(db);
  char before[SNAP_CAP];
  snapshot(db, before, sizeof(before));
  int64_t id = 777;

  CHECK_ERR(db, perk_insert(db, NULL, cols, vals, 1, &id), PERK_ERR_MISUSE);
  CHECK_ERR(db, perk_insert(db, "users", NULL, vals, 1, &id), PERK_ERR_MISUSE);
  CHECK_ERR(db, perk_insert(db, "users", cols, NULL, 1, &id), PERK_ERR_MISUSE);
  CHECK_ERR(db, perk_insert(db, "users", cols, vals, 0, &id), PERK_ERR_MISUSE);
  ASSERT_TRUE(strlen(perk_errmsg(db)) > 0);

  // Bad table names
  for (size_t i = 0; i < N_BAD_NAMES; i++) {
    PerkErr e = perk_insert(db, kBadNames[i], cols, vals, 1, &id);
    if (!ASSERT_EQ_INT(e, PERK_ERR_BADNAME))
      printf("  table name: \"%s\"\n", kBadNames[i]);
  }
  // Bad column names, as the only column and as the second column
  for (size_t i = 0; i < N_BAD_NAMES; i++) {
    const char* one[] = {kBadNames[i]};
    PerkErr e = perk_insert(db, "users", one, vals, 1, &id);
    if (!ASSERT_EQ_INT(e, PERK_ERR_BADNAME))
      printf("  column name: \"%s\"\n", kBadNames[i]);
    const char* two[] = {"name", kBadNames[i]};
    PerkValue tv[] = {perk_text("partial"), perk_int(1)};
    e = perk_insert(db, "users", two, tv, 2, &id);
    if (!ASSERT_EQ_INT(e, PERK_ERR_BADNAME))
      printf("  second column name: \"%s\"\n", kBadNames[i]);
  }
  const char* null_col[] = {NULL};
  CHECK_ERR(db, perk_insert(db, "users", null_col, vals, 1, &id),
            PERK_ERR_BADNAME);

  // Malformed values
  PerkValue bad_type = {.type = (PerkType)99};
  CHECK_ERR(db, perk_insert(db, "users", cols, &bad_type, 1, &id),
            PERK_ERR_MISUSE);
  PerkValue null_text = {.type = PERK_TEXT, .as.text = {NULL, 3}};
  CHECK_ERR(db, perk_insert(db, "users", cols, &null_text, 1, &id),
            PERK_ERR_MISUSE);
  PerkValue null_blob = {.type = PERK_BLOB, .as.blob = {NULL, 3}};
  const char* bcols[] = {"data"};
  CHECK_ERR(db, perk_insert(db, "users", bcols, &null_blob, 1, &id),
            PERK_ERR_MISUSE);

  // SQLite-level failures
  CHECK_ERR(db, perk_insert(db, "nosuchtable", cols, vals, 1, &id),
            PERK_ERR_SQL);
  ASSERT_TRUE(strlen(perk_errmsg(db)) > 0);
  const char* unknown[] = {"nosuchcolumn"};
  CHECK_ERR(db, perk_insert(db, "users", unknown, vals, 1, &id), PERK_ERR_SQL);

  // Nothing was added by any failed call
  CHECK_UNCHANGED(db, before);
  ASSERT_EQ_INT(count_rows(db, "users"), 3);

  // The handle still works afterwards
  CHECK_ERR(db, perk_insert(db, "users", cols, vals, 1, &id), PERK_OK);
  ASSERT_EQ_INT(id, 4);

  perk_close(db);
}

/** Constraint violations fail with PERK_ERR_SQL and leave the table alone. */
static void test_insert_constraints(void) {
  printf("insert: constraint violations\n");
  PerkDB* db = NULL;
  if (!ASSERT_EQ_INT(perk_open(":memory:", &db), PERK_OK)) return;
  CHECK_ERR(db, perk_create_table(db, "uniq", kUniq, 2), PERK_OK);

  const char* cols[] = {"name"};
  PerkValue v = perk_text("only");
  int64_t id = 0;
  CHECK_ERR(db, perk_insert(db, "uniq", cols, &v, 1, &id), PERK_OK);
  CHECK_ERR(db, perk_insert(db, "uniq", cols, &v, 1, &id), PERK_ERR_SQL);
  PerkValue nul = perk_null();
  CHECK_ERR(db, perk_insert(db, "uniq", cols, &nul, 1, &id), PERK_ERR_SQL);
  ASSERT_EQ_INT(count_rows(db, "uniq"), 1);

  // A duplicate primary key too
  const char* both[] = {"id", "name"};
  PerkValue pv[] = {perk_int(1), perk_text("other")};
  CHECK_ERR(db, perk_insert(db, "uniq", both, pv, 2, &id), PERK_ERR_SQL);
  ASSERT_EQ_INT(count_rows(db, "uniq"), 1);

  perk_close(db);
}

// ------------------------------------------------------------ select all ----

/** List-all with NULL / "" where, subsets of columns, and empty tables. */
static void test_select_list_all(void) {
  printf("select: list-all\n");
  PerkDB* db = open_users();
  if (db == NULL) return;

  // An empty table is a successful 0-row result, never NOT_FOUND
  PerkResult* res = NULL;
  CHECK_ERR(db, perk_select(db, "users", NULL, 0, NULL, NULL, 0, &res),
            PERK_OK);
  ASSERT_TRUE(res != NULL);
  ASSERT_EQ_INT(perk_result_rows(res), 0);
  ASSERT_TRUE(perk_result_get(res, 0, 0) == NULL);
  perk_result_free(res);
  res = NULL;
  CHECK_ERR(db, perk_select(db, "users", NULL, 0, "", NULL, 0, &res), PERK_OK);
  ASSERT_EQ_INT(perk_result_rows(res), 0);
  perk_result_free(res);

  seed(db);

  // NULL where, "" where, and whitespace-only where all list every row, in
  // insertion order
  const char* wheres[] = {NULL, "", "   "};
  for (size_t i = 0; i < 3; i++) {
    res = NULL;
    CHECK_ERR(db, perk_select(db, "users", NULL, 0, wheres[i], NULL, 0, &res),
              PERK_OK);
    if (!ASSERT_TRUE(res != NULL)) continue;
    ASSERT_EQ_INT(perk_result_rows(res), 3);
    ASSERT_EQ_INT(perk_result_cols(res), 5);
    const PerkValue* v = perk_result_get(res, 0, 1);
    if (ASSERT_TRUE(v != NULL)) ASSERT_EQ_STR(v->as.text.ptr, "alice");
    v = perk_result_get(res, 2, 1);
    if (ASSERT_TRUE(v != NULL)) ASSERT_EQ_STR(v->as.text.ptr, "carol");
    // Out-of-range indexes return NULL
    ASSERT_TRUE(perk_result_get(res, 3, 0) == NULL);
    ASSERT_TRUE(perk_result_get(res, 0, 5) == NULL);
    ASSERT_TRUE(perk_result_colname(res, 5) == NULL);
    perk_result_free(res);
  }

  // Column subset, in the requested order
  const char* cols[] = {"age", "name"};
  res = NULL;
  CHECK_ERR(db, perk_select(db, "users", cols, 2, NULL, NULL, 0, &res),
            PERK_OK);
  if (ASSERT_TRUE(res != NULL)) {
    ASSERT_EQ_INT(perk_result_rows(res), 3);
    ASSERT_EQ_INT(perk_result_cols(res), 2);
    ASSERT_EQ_STR(perk_result_colname(res, 0), "age");
    ASSERT_EQ_STR(perk_result_colname(res, 1), "name");
    const PerkValue* v = perk_result_get(res, 1, 0);
    if (ASSERT_TRUE(v != NULL) && ASSERT_EQ_INT(v->type, PERK_INT))
      ASSERT_EQ_INT(v->as.i, 25);
  }
  perk_result_free(res);

  // ncols == 0 ignores a non-NULL cols array
  res = NULL;
  CHECK_ERR(db, perk_select(db, "users", cols, 0, NULL, NULL, 0, &res),
            PERK_OK);
  if (ASSERT_TRUE(res != NULL)) ASSERT_EQ_INT(perk_result_cols(res), 5);
  perk_result_free(res);

  // A larger table
  for (int i = 0; i < 500; i++) add_user(db, "bulk", i);
  ASSERT_EQ_INT(count_rows(db, "users"), 503);

  // Freeing NULL is a no-op
  perk_result_free(NULL);

  perk_close(db);
}

/** NULL / invalid arguments to perk_select. */
static void test_select_errors(void) {
  printf("select: errors\n");
  PerkResult* res = NULL;
  ASSERT_EQ_INT(perk_select(NULL, "users", NULL, 0, NULL, NULL, 0, &res),
                PERK_ERR_MISUSE);

  PerkDB* db = open_users();
  if (db == NULL) return;
  seed(db);
  char before[SNAP_CAP];
  snapshot(db, before, sizeof(before));

  static int sentinel;
  const char* cols[] = {"name"};
  PerkValue arg = perk_int(1);

  // *out is reset to NULL on every failure
  res = (PerkResult*)&sentinel;
  CHECK_ERR(db, perk_select(db, NULL, NULL, 0, NULL, NULL, 0, &res),
            PERK_ERR_MISUSE);
  ASSERT_TRUE(res == NULL);
  CHECK_ERR(db, perk_select(db, "users", NULL, 0, NULL, NULL, 0, NULL),
            PERK_ERR_MISUSE);
  res = (PerkResult*)&sentinel;
  CHECK_ERR(db, perk_select(db, "users", NULL, 2, NULL, NULL, 0, &res),
            PERK_ERR_MISUSE);
  ASSERT_TRUE(res == NULL);
  res = (PerkResult*)&sentinel;
  CHECK_ERR(db, perk_select(db, "users", cols, 1, "id = ?", NULL, 1, &res),
            PERK_ERR_MISUSE);
  ASSERT_TRUE(res == NULL);

  // Invalid identifiers
  for (size_t i = 0; i < N_BAD_NAMES; i++) {
    res = (PerkResult*)&sentinel;
    PerkErr e = perk_select(db, kBadNames[i], NULL, 0, NULL, NULL, 0, &res);
    if (!ASSERT_EQ_INT(e, PERK_ERR_BADNAME))
      printf("  table name: \"%s\"\n", kBadNames[i]);
    ASSERT_TRUE(res == NULL);
    const char* one[] = {kBadNames[i]};
    res = (PerkResult*)&sentinel;
    e = perk_select(db, "users", one, 1, NULL, NULL, 0, &res);
    if (!ASSERT_EQ_INT(e, PERK_ERR_BADNAME))
      printf("  column name: \"%s\"\n", kBadNames[i]);
    ASSERT_TRUE(res == NULL);
  }

  // SQLite-level failures are PERK_ERR_SQL, not NOT_FOUND
  res = (PerkResult*)&sentinel;
  CHECK_ERR(db, perk_select(db, "nosuchtable", NULL, 0, NULL, NULL, 0, &res),
            PERK_ERR_SQL);
  ASSERT_TRUE(res == NULL);
  const char* unknown[] = {"nosuchcolumn"};
  res = (PerkResult*)&sentinel;
  CHECK_ERR(db, perk_select(db, "users", unknown, 1, NULL, NULL, 0, &res),
            PERK_ERR_SQL);
  ASSERT_TRUE(res == NULL);
  res = (PerkResult*)&sentinel;
  CHECK_ERR(
      db, perk_select(db, "users", NULL, 0, "nosuchcolumn = ?", &arg, 1, &res),
      PERK_ERR_SQL);
  ASSERT_TRUE(res == NULL);

  CHECK_UNCHANGED(db, before);
  perk_close(db);
}

/** WHERE filters: operators, multiple args, no match, injection shapes. */
static void test_select_where(void) {
  printf("select: where\n");
  PerkDB* db = open_users();
  if (db == NULL) return;
  seed(db);
  const char* nullcols[] = {"name"};
  PerkValue nv = perk_null();
  int64_t nid = 0;
  CHECK_ERR(db, perk_insert(db, "users", nullcols, &nv, 1, &nid), PERK_OK);
  ASSERT_EQ_INT(nid, 4);  // row 4 has NULL name and NULL age

  struct {
    const char* where;
    PerkValue args[3];
    size_t nargs;
    size_t rows;
  } cases[] = {
      {"age = ?", {perk_int(30)}, 1, 2},
      {"age > ?", {perk_int(25)}, 1, 2},
      {"age >= ? AND name = ?", {perk_int(30), perk_text("carol")}, 2, 1},
      {"name = ? OR name IS NULL", {perk_text("bob")}, 1, 2},
      {"name IS NULL", {perk_null()}, 0, 1},
      {"age BETWEEN ? AND ?", {perk_int(26), perk_int(40)}, 2, 2},
      {"name IN (?, ?)", {perk_text("alice"), perk_text("bob")}, 2, 2},
      {"name LIKE ?", {perk_text("c%")}, 1, 1},
      {"NOT (age = ?)", {perk_int(30)}, 1, 1},  // NULL age row is not matched
      {"age = ?", {perk_int(999)}, 1, 0},       // no match: OK, 0 rows
      {"id = ?", {perk_int(-1)}, 1, 0},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    PerkResult* res = NULL;
    PerkErr e = perk_select(db, "users", NULL, 0, cases[i].where, cases[i].args,
                            cases[i].nargs, &res);
    if (!ASSERT_EQ_INT(e, PERK_OK) ||
        !ASSERT_EQ_INT(perk_result_rows(res), cases[i].rows))
      printf("  where: %s\n", cases[i].where);
    perk_result_free(res);
  }

  // Injection-shaped args never match anything and never execute
  for (size_t i = 0; i < N_INJECTIONS; i++) {
    PerkValue v = perk_text(kInjections[i]);
    PerkResult* res = NULL;
    CHECK_ERR(db, perk_select(db, "users", NULL, 0, "name = ?", &v, 1, &res),
              PERK_OK);
    ASSERT_EQ_INT(perk_result_rows(res), 0);
    perk_result_free(res);
    res = NULL;
    CHECK_ERR(db, perk_select(db, "users", NULL, 0, "id = ?", &v, 1, &res),
              PERK_OK);
    ASSERT_EQ_INT(perk_result_rows(res), 0);
    perk_result_free(res);
  }

  // Injection-shaped WHERE text is rejected, not executed
  const char* bad_wheres[] = {
      "age = 1 OR 1 = 1",
      "name = 'x'",
      "name = \"x\"",
      "1=1; DROP TABLE users",
      "age > ? --",
      "age > ? /* c */",
      "name = ? ; DELETE FROM users",
      "age = ?)",
      "(age = ?",
      "abs(age) = ?",
      "name = ? UNION SELECT 1",
  };
  PerkValue one = perk_int(1);
  for (size_t i = 0; i < sizeof(bad_wheres) / sizeof(bad_wheres[0]); i++) {
    PerkResult* res = NULL;
    PerkErr e = perk_select(db, "users", NULL, 0, bad_wheres[i], &one, 1, &res);
    if (!ASSERT_TRUE(e != PERK_OK && e != PERK_ERR_NOT_FOUND))
      printf("  where: %s\n", bad_wheres[i]);
    ASSERT_TRUE(res == NULL);
  }
  // Placeholder / arg count mismatches
  PerkResult* res = NULL;
  CHECK_ERR(db, perk_select(db, "users", NULL, 0, "age > ?", &one, 0, &res),
            PERK_ERR_MISUSE);
  CHECK_ERR(db, perk_select(db, "users", NULL, 0, "age > 1", &one, 1, &res),
            PERK_ERR_MISUSE);
  CHECK_ERR(db, perk_select(db, "users", NULL, 0, NULL, &one, 1, &res),
            PERK_ERR_MISUSE);
  ASSERT_TRUE(res == NULL);

  // Table is intact
  ASSERT_EQ_INT(count_rows(db, "users"), 4);
  perk_close(db);
}

// ------------------------------------------------------------- by id --------

/** By-id read: found, subset of columns, missing, and repeated ids. */
static void test_select_by_id(void) {
  printf("select_by_id: happy and missing\n");
  PerkDB* db = open_users();
  if (db == NULL) return;
  static int sentinel;

  // Empty table: NOT_FOUND, *out is NULL
  PerkResult* res = (PerkResult*)&sentinel;
  PerkValue key = perk_int(1);
  CHECK_ERR(db, perk_select_by_id(db, "users", NULL, 0, "id", &key, &res),
            PERK_ERR_NOT_FOUND);
  ASSERT_TRUE(res == NULL);
  ASSERT_TRUE(strlen(perk_errmsg(db)) > 0);

  seed(db);
  res = NULL;
  CHECK_ERR(db, perk_select_by_id(db, "users", NULL, 0, "id", &key, &res),
            PERK_OK);
  if (ASSERT_TRUE(res != NULL)) {
    ASSERT_EQ_INT(perk_result_rows(res), 1);
    ASSERT_EQ_INT(perk_result_cols(res), 5);
    const PerkValue* v = perk_result_get(res, 0, 1);
    if (ASSERT_TRUE(v != NULL)) ASSERT_EQ_STR(v->as.text.ptr, "alice");
  }
  perk_result_free(res);

  // Column subset
  const char* cols[] = {"age", "name"};
  key = perk_int(2);
  res = NULL;
  CHECK_ERR(db, perk_select_by_id(db, "users", cols, 2, "id", &key, &res),
            PERK_OK);
  if (ASSERT_TRUE(res != NULL)) {
    ASSERT_EQ_INT(perk_result_cols(res), 2);
    const PerkValue* v = perk_result_get(res, 0, 0);
    if (ASSERT_TRUE(v != NULL) && ASSERT_EQ_INT(v->type, PERK_INT))
      ASSERT_EQ_INT(v->as.i, 25);
    v = perk_result_get(res, 0, 1);
    if (ASSERT_TRUE(v != NULL)) ASSERT_EQ_STR(v->as.text.ptr, "bob");
  }
  perk_result_free(res);

  // Missing ids, including extremes
  int64_t missing[] = {0, 4, -1, 1000000, INT64_MAX, INT64_MIN};
  for (size_t i = 0; i < sizeof(missing) / sizeof(missing[0]); i++) {
    key = perk_int(missing[i]);
    res = (PerkResult*)&sentinel;
    PerkErr e = perk_select_by_id(db, "users", NULL, 0, "id", &key, &res);
    if (!ASSERT_EQ_INT(e, PERK_ERR_NOT_FOUND))
      printf("  id: %lld\n", (long long)missing[i]);
    ASSERT_TRUE(res == NULL);
  }

  // A deleted row is NOT_FOUND
  PerkValue del_key[] = {perk_int(2)};
  CHECK_ERR(db, perk_delete(db, "users", "id = ?", del_key, 1, NULL), PERK_OK);
  key = perk_int(2);
  res = NULL;
  CHECK_ERR(db, perk_select_by_id(db, "users", NULL, 0, "id", &key, &res),
            PERK_ERR_NOT_FOUND);

  // Another key column (text); a non-unique column returns every match
  PerkValue name = perk_text("carol");
  res = NULL;
  CHECK_ERR(db, perk_select_by_id(db, "users", NULL, 0, "name", &name, &res),
            PERK_OK);
  if (ASSERT_TRUE(res != NULL)) ASSERT_EQ_INT(perk_result_rows(res), 1);
  perk_result_free(res);
  PerkValue age = perk_int(30);
  res = NULL;
  CHECK_ERR(db, perk_select_by_id(db, "users", NULL, 0, "age", &age, &res),
            PERK_OK);
  if (ASSERT_TRUE(res != NULL)) ASSERT_EQ_INT(perk_result_rows(res), 2);
  perk_result_free(res);

  perk_close(db);
}

/** A NULL id matches nothing, even rows whose key column is NULL. */
static void test_select_by_id_null_key(void) {
  printf("select_by_id: PERK_NULL id\n");
  PerkDB* db = open_users();
  if (db == NULL) return;
  const char* cols[] = {"name"};  // age left NULL
  PerkValue v = perk_text("nullage");
  int64_t id = 0;
  CHECK_ERR(db, perk_insert(db, "users", cols, &v, 1, &id), PERK_OK);
  ASSERT_TRUE(is_null(db, id, "age"));

  static int sentinel;
  PerkValue key = perk_null();
  PerkResult* res = (PerkResult*)&sentinel;
  CHECK_ERR(db, perk_select_by_id(db, "users", NULL, 0, "age", &key, &res),
            PERK_ERR_NOT_FOUND);
  ASSERT_TRUE(res == NULL);
  res = (PerkResult*)&sentinel;
  CHECK_ERR(db, perk_select_by_id(db, "users", NULL, 0, "id", &key, &res),
            PERK_ERR_NOT_FOUND);
  ASSERT_TRUE(res == NULL);
  PerkValue tn = perk_text(NULL);  // also SQL NULL
  CHECK_ERR(db, perk_select_by_id(db, "users", NULL, 0, "age", &tn, &res),
            PERK_ERR_NOT_FOUND);

  perk_close(db);
}

/** Injection-shaped ids and an injection-shaped stored name. */
static void test_select_by_id_injection(void) {
  printf("select_by_id: injection\n");
  PerkDB* db = open_users();
  if (db == NULL) return;
  seed(db);
  char before[SNAP_CAP];
  snapshot(db, before, sizeof(before));

  static int sentinel;
  for (size_t i = 0; i < N_INJECTIONS; i++) {
    PerkValue key = perk_text(kInjections[i]);
    PerkResult* res = (PerkResult*)&sentinel;
    PerkErr e = perk_select_by_id(db, "users", NULL, 0, "id", &key, &res);
    if (!ASSERT_EQ_INT(e, PERK_ERR_NOT_FOUND))
      printf("  id: %s\n", kInjections[i]);
    ASSERT_TRUE(res == NULL);
  }
  const char* idshaped[] = {"1 OR 1=1", "1; DROP TABLE users", "1' OR '1'='1",
                            "1 --"};
  for (size_t i = 0; i < 4; i++) {
    PerkValue key = perk_text(idshaped[i]);
    PerkResult* res = (PerkResult*)&sentinel;
    PerkErr e = perk_select_by_id(db, "users", NULL, 0, "id", &key, &res);
    if (!ASSERT_EQ_INT(e, PERK_ERR_NOT_FOUND))
      printf("  id: %s\n", idshaped[i]);
  }
  CHECK_UNCHANGED(db, before);

  // A stored injection string is found by exact match only
  for (size_t i = 0; i < N_INJECTIONS; i++) {
    int64_t id = add_user(db, kInjections[i], 50 + (int64_t)i);
    PerkValue key = perk_text(kInjections[i]);
    PerkResult* res = NULL;
    CHECK_ERR(db, perk_select_by_id(db, "users", NULL, 0, "name", &key, &res),
              PERK_OK);
    if (ASSERT_TRUE(res != NULL) && ASSERT_EQ_INT(perk_result_rows(res), 1)) {
      const PerkValue* v = perk_result_get(res, 0, 0);
      if (ASSERT_TRUE(v != NULL)) ASSERT_EQ_INT(v->as.i, id);
    }
    perk_result_free(res);
  }

  perk_close(db);
}

/** NULL / empty / invalid arguments to perk_select_by_id. */
static void test_select_by_id_errors(void) {
  printf("select_by_id: errors\n");
  PerkValue key = perk_int(1);
  PerkResult* res = NULL;
  ASSERT_EQ_INT(perk_select_by_id(NULL, "users", NULL, 0, "id", &key, &res),
                PERK_ERR_MISUSE);

  PerkDB* db = open_users();
  if (db == NULL) return;
  seed(db);
  char before[SNAP_CAP];
  snapshot(db, before, sizeof(before));
  static int sentinel;
  const char* cols[] = {"name"};

#define BY_ID_EXPECT(call, expected)   \
  do {                                 \
    res = (PerkResult*)&sentinel;      \
    CHECK_ERR(db, (call), (expected)); \
    ASSERT_TRUE(res == NULL);          \
  } while (0)

  BY_ID_EXPECT(perk_select_by_id(db, NULL, NULL, 0, "id", &key, &res),
               PERK_ERR_MISUSE);
  BY_ID_EXPECT(perk_select_by_id(db, "users", NULL, 0, NULL, &key, &res),
               PERK_ERR_MISUSE);
  BY_ID_EXPECT(perk_select_by_id(db, "users", NULL, 0, "id", NULL, &res),
               PERK_ERR_MISUSE);
  BY_ID_EXPECT(perk_select_by_id(db, "users", NULL, 1, "id", &key, &res),
               PERK_ERR_MISUSE);
  CHECK_ERR(db, perk_select_by_id(db, "users", NULL, 0, "id", &key, NULL),
            PERK_ERR_MISUSE);

  // Empty and invalid table / id column / selected column names
  for (size_t i = 0; i < N_BAD_NAMES; i++) {
    BY_ID_EXPECT(perk_select_by_id(db, kBadNames[i], NULL, 0, "id", &key, &res),
                 PERK_ERR_BADNAME);
    BY_ID_EXPECT(
        perk_select_by_id(db, "users", NULL, 0, kBadNames[i], &key, &res),
        PERK_ERR_BADNAME);
    const char* one[] = {kBadNames[i]};
    BY_ID_EXPECT(perk_select_by_id(db, "users", one, 1, "id", &key, &res),
                 PERK_ERR_BADNAME);
  }
#undef BY_ID_EXPECT

  // SQL errors are PERK_ERR_SQL, never NOT_FOUND
  res = NULL;
  CHECK_ERR(db, perk_select_by_id(db, "nosuchtable", NULL, 0, "id", &key, &res),
            PERK_ERR_SQL);
  CHECK_ERR(db,
            perk_select_by_id(db, "users", NULL, 0, "nosuchcol", &key, &res),
            PERK_ERR_SQL);
  CHECK_ERR(db,
            perk_select_by_id(db, "users", (const char*[]){"nosuchcol"}, 1,
                              "id", &key, &res),
            PERK_ERR_SQL);
  ASSERT_TRUE(res == NULL);

  // A malformed id value
  PerkValue bad = {.type = (PerkType)99};
  CHECK_ERR(db, perk_select_by_id(db, "users", cols, 1, "id", &bad, &res),
            PERK_ERR_MISUSE);

  CHECK_UNCHANGED(db, before);
  perk_close(db);
}

// ---------------------------------------------------------------- update ----

/** Update one row by id: only that row changes. */
static void test_update_happy(void) {
  printf("update: happy path\n");
  PerkDB* db = open_users();
  if (db == NULL) return;
  seed(db);

  const char* cols[] = {"name", "age"};
  PerkValue sets[] = {perk_text("robert"), perk_int(26)};
  PerkValue where_args[] = {perk_int(2)};
  int64_t changed = -1;
  CHECK_ERR(db,
            perk_update(db, "users", cols, sets, 2, "id = ?", where_args, 1,
                        &changed),
            PERK_OK);
  ASSERT_EQ_INT(changed, 1);
  ASSERT_TRUE(text_is(db, 2, "name", "robert"));
  ASSERT_TRUE(int_is(db, 2, "age", 26));
  // Neighbours untouched
  ASSERT_TRUE(text_is(db, 1, "name", "alice"));
  ASSERT_TRUE(int_is(db, 1, "age", 30));
  ASSERT_TRUE(text_is(db, 3, "name", "carol"));
  ASSERT_EQ_INT(count_rows(db, "users"), 3);

  // changed may be NULL
  PerkValue s2[] = {perk_int(27)};
  const char* c2[] = {"age"};
  CHECK_ERR(db,
            perk_update(db, "users", c2, s2, 1, "id = ?", where_args, 1, NULL),
            PERK_OK);
  ASSERT_TRUE(int_is(db, 2, "age", 27));

  // Setting a value to itself still counts as a changed (matched) row
  changed = -1;
  CHECK_ERR(
      db,
      perk_update(db, "users", c2, s2, 1, "id = ?", where_args, 1, &changed),
      PERK_OK);
  ASSERT_EQ_INT(changed, 1);

  // Set to NULL, to empty text, to another type
  PerkValue nul[] = {perk_null()};
  CHECK_ERR(
      db,
      perk_update(db, "users", c2, nul, 1, "id = ?", where_args, 1, &changed),
      PERK_OK);
  ASSERT_TRUE(is_null(db, 2, "age"));
  const char* nc[] = {"name"};
  PerkValue empty[] = {perk_text("")};
  CHECK_ERR(
      db,
      perk_update(db, "users", nc, empty, 1, "id = ?", where_args, 1, &changed),
      PERK_OK);
  ASSERT_TRUE(text_is(db, 2, "name", ""));

  // Value types round trip through update
  const unsigned char bytes[] = {0, 1, 2, 0, 255};
  const char* tc[] = {"score", "data"};
  PerkValue tv[] = {perk_real(2.5), perk_blob(bytes, sizeof(bytes))};
  CHECK_ERR(
      db,
      perk_update(db, "users", tc, tv, 2, "id = ?", where_args, 1, &changed),
      PERK_OK);
  PerkResult* res = NULL;
  const PerkValue* v = fetch(db, 2, "score", &res);
  if (ASSERT_TRUE(v != NULL) && ASSERT_EQ_INT(v->type, PERK_REAL))
    ASSERT_TRUE(v->as.r == 2.5);
  perk_result_free(res);
  v = fetch(db, 2, "data", &res);
  if (ASSERT_TRUE(v != NULL) && ASSERT_EQ_INT(v->type, PERK_BLOB))
    ASSERT_TRUE(v->as.blob.len == sizeof(bytes) &&
                memcmp(v->as.blob.ptr, bytes, sizeof(bytes)) == 0);
  perk_result_free(res);

  perk_close(db);
}

/** Multi-row updates report how many rows changed. */
static void test_update_multi_row(void) {
  printf("update: multi-row changed counts\n");
  PerkDB* db = open_users();
  if (db == NULL) return;
  seed(db);
  const char* cols[] = {"score"};
  PerkValue set[] = {perk_real(1.5)};
  int64_t changed = -1;

  // Two rows have age 30
  PerkValue age30[] = {perk_int(30)};
  CHECK_ERR(
      db, perk_update(db, "users", cols, set, 1, "age = ?", age30, 1, &changed),
      PERK_OK);
  ASSERT_EQ_INT(changed, 2);

  // NULL where and "" where update every row
  changed = -1;
  CHECK_ERR(db, perk_update(db, "users", cols, set, 1, NULL, NULL, 0, &changed),
            PERK_OK);
  ASSERT_EQ_INT(changed, 3);
  changed = -1;
  CHECK_ERR(db, perk_update(db, "users", cols, set, 1, "", NULL, 0, &changed),
            PERK_OK);
  ASSERT_EQ_INT(changed, 3);

  // Compound where with args after the SET values
  PerkValue sets2[] = {perk_text("zed"), perk_int(31)};
  const char* cols2[] = {"name", "age"};
  PerkValue args2[] = {perk_int(30), perk_text("alice")};
  CHECK_ERR(db,
            perk_update(db, "users", cols2, sets2, 2, "age = ? AND name = ?",
                        args2, 2, &changed),
            PERK_OK);
  ASSERT_EQ_INT(changed, 1);
  ASSERT_TRUE(text_is(db, 1, "name", "zed"));
  ASSERT_TRUE(text_is(db, 3, "name", "carol"));

  // Large table
  for (int i = 0; i < 500; i++) add_user(db, "bulk", 7);
  CHECK_ERR(db, perk_update(db, "users", cols, set, 1, NULL, NULL, 0, &changed),
            PERK_OK);
  ASSERT_EQ_INT(changed, 503);

  perk_close(db);
}

/** Missing rows give PERK_ERR_NOT_FOUND, changed == 0, and no data change. */
static void test_update_missing(void) {
  printf("update: missing rows\n");
  PerkDB* db = open_users();
  if (db == NULL) return;

  const char* cols[] = {"name"};
  PerkValue set[] = {perk_text("ghost")};

  // Empty table, with and without a WHERE
  int64_t changed = 99;
  CHECK_ERR(db, perk_update(db, "users", cols, set, 1, NULL, NULL, 0, &changed),
            PERK_ERR_NOT_FOUND);
  ASSERT_EQ_INT(changed, 0);
  PerkValue id1[] = {perk_int(1)};
  changed = 99;
  CHECK_ERR(db,
            perk_update(db, "users", cols, set, 1, "id = ?", id1, 1, &changed),
            PERK_ERR_NOT_FOUND);
  ASSERT_EQ_INT(changed, 0);
  ASSERT_TRUE(strlen(perk_errmsg(db)) > 0);

  seed(db);
  char before[SNAP_CAP];
  snapshot(db, before, sizeof(before));

  int64_t missing[] = {0, 4, -5, INT64_MAX};
  for (size_t i = 0; i < sizeof(missing) / sizeof(missing[0]); i++) {
    PerkValue a[] = {perk_int(missing[i])};
    changed = 99;
    PerkErr e =
        perk_update(db, "users", cols, set, 1, "id = ?", a, 1, &changed);
    if (!ASSERT_EQ_INT(e, PERK_ERR_NOT_FOUND))
      printf("  id: %lld\n", (long long)missing[i]);
    ASSERT_EQ_INT(changed, 0);
  }
  // changed == NULL is fine on NOT_FOUND
  PerkValue a9[] = {perk_int(9)};
  CHECK_ERR(db, perk_update(db, "users", cols, set, 1, "id = ?", a9, 1, NULL),
            PERK_ERR_NOT_FOUND);
  // PERK_NULL never matches with =
  PerkValue an[] = {perk_null()};
  CHECK_ERR(db,
            perk_update(db, "users", cols, set, 1, "id = ?", an, 1, &changed),
            PERK_ERR_NOT_FOUND);
  CHECK_UNCHANGED(db, before);

  // After a row is deleted, updating it is NOT_FOUND
  CHECK_ERR(db, perk_delete(db, "users", "id = ?", id1, 1, NULL), PERK_OK);
  CHECK_ERR(db,
            perk_update(db, "users", cols, set, 1, "id = ?", id1, 1, &changed),
            PERK_ERR_NOT_FOUND);
  ASSERT_TRUE(!row_exists(db, 1));

  perk_close(db);
}

/** Injection-shaped values, including the flagged EXISTING id case. */
static void test_update_injection(void) {
  printf("update: injection-shaped values\n");
  PerkDB* db = open_users();
  if (db == NULL) return;
  seed(db);

  // Existing id, injection-shaped new value: PERK_OK, changed == 1, stored
  // literally and readable
  const char* cols[] = {"name"};
  PerkValue id2[] = {perk_int(2)};
  for (size_t i = 0; i < N_INJECTIONS; i++) {
    PerkValue set[] = {perk_text(kInjections[i])};
    int64_t changed = -1;
    PerkErr e =
        perk_update(db, "users", cols, set, 1, "id = ?", id2, 1, &changed);
    if (!ASSERT_EQ_INT(e, PERK_OK) || !ASSERT_EQ_INT(changed, 1))
      printf("  value: %s\n", kInjections[i]);
    if (!ASSERT_TRUE(text_is(db, 2, "name", kInjections[i])))
      printf("  value: %s\n", kInjections[i]);
    // The other rows and the table itself are untouched
    ASSERT_TRUE(text_is(db, 1, "name", "alice"));
    ASSERT_TRUE(text_is(db, 3, "name", "carol"));
    ASSERT_EQ_INT(count_rows(db, "users"), 3);
  }

  // Injection-shaped WHERE args on a missing / mismatching key: NOT_FOUND and
  // nothing changes
  char before[SNAP_CAP];
  snapshot(db, before, sizeof(before));
  PerkValue set[] = {perk_text("pwned")};
  const char* keys[] = {"1 OR 1=1", "1; DROP TABLE users", "' OR '1'='1",
                        "2 --"};
  for (size_t i = 0; i < 4; i++) {
    PerkValue a[] = {perk_text(keys[i])};
    int64_t changed = 99;
    PerkErr e =
        perk_update(db, "users", cols, set, 1, "id = ?", a, 1, &changed);
    if (!ASSERT_EQ_INT(e, PERK_ERR_NOT_FOUND)) printf("  key: %s\n", keys[i]);
    ASSERT_EQ_INT(changed, 0);
  }
  CHECK_UNCHANGED(db, before);

  // An injection-shaped string used as the key matches only the row that holds
  // that exact string (row 2 holds the last one from the loop above)
  PerkValue a[] = {perk_text(kInjections[N_INJECTIONS - 1])};
  int64_t changed = -1;
  CHECK_ERR(db,
            perk_update(db, "users", cols, set, 1, "name = ?", a, 1, &changed),
            PERK_OK);
  ASSERT_EQ_INT(changed, 1);
  ASSERT_TRUE(text_is(db, 2, "name", "pwned"));
  ASSERT_TRUE(text_is(db, 1, "name", "alice"));

  // Injection in WHERE text is rejected and nothing changes
  snapshot(db, before, sizeof(before));
  PerkErr e = perk_update(db, "users", cols, set, 1, "id = 1 OR 1 = 1", NULL, 0,
                          &changed);
  ASSERT_TRUE(e != PERK_OK);
  e = perk_update(db, "users", cols, set, 1, "id = ?; DROP TABLE users", id2, 1,
                  &changed);
  ASSERT_TRUE(e != PERK_OK);
  CHECK_UNCHANGED(db, before);

  perk_close(db);
}

/** NULL / empty / invalid arguments to perk_update. */
static void test_update_errors(void) {
  printf("update: errors\n");
  const char* cols[] = {"name"};
  PerkValue set[] = {perk_text("x")};
  PerkValue id1[] = {perk_int(1)};
  int64_t changed = 5;
  ASSERT_EQ_INT(
      perk_update(NULL, "users", cols, set, 1, "id = ?", id1, 1, &changed),
      PERK_ERR_MISUSE);

  PerkDB* db = open_users();
  if (db == NULL) return;
  seed(db);
  char before[SNAP_CAP];
  snapshot(db, before, sizeof(before));

  CHECK_ERR(db, perk_update(db, NULL, cols, set, 1, "id = ?", id1, 1, &changed),
            PERK_ERR_MISUSE);
  CHECK_ERR(db,
            perk_update(db, "users", NULL, set, 1, "id = ?", id1, 1, &changed),
            PERK_ERR_MISUSE);
  CHECK_ERR(db,
            perk_update(db, "users", cols, NULL, 1, "id = ?", id1, 1, &changed),
            PERK_ERR_MISUSE);
  CHECK_ERR(db,
            perk_update(db, "users", cols, set, 0, "id = ?", id1, 1, &changed),
            PERK_ERR_MISUSE);
  CHECK_ERR(db,
            perk_update(db, "users", cols, set, 1, "id = ?", NULL, 1, &changed),
            PERK_ERR_MISUSE);
  // ? count must equal nargs
  CHECK_ERR(db,
            perk_update(db, "users", cols, set, 1, "id = ?", id1, 0, &changed),
            PERK_ERR_MISUSE);
  CHECK_ERR(db,
            perk_update(db, "users", cols, set, 1, "id = ? OR id = ?", id1, 1,
                        &changed),
            PERK_ERR_MISUSE);
  CHECK_ERR(db, perk_update(db, "users", cols, set, 1, NULL, id1, 1, &changed),
            PERK_ERR_MISUSE);
  ASSERT_EQ_INT(changed, 5);  // never written on failure

  // Empty and invalid identifiers
  for (size_t i = 0; i < N_BAD_NAMES; i++) {
    PerkErr e =
        perk_update(db, kBadNames[i], cols, set, 1, "id = ?", id1, 1, &changed);
    if (!ASSERT_EQ_INT(e, PERK_ERR_BADNAME))
      printf("  table name: \"%s\"\n", kBadNames[i]);
    const char* bad[] = {kBadNames[i]};
    e = perk_update(db, "users", bad, set, 1, "id = ?", id1, 1, &changed);
    if (!ASSERT_EQ_INT(e, PERK_ERR_BADNAME))
      printf("  set column: \"%s\"\n", kBadNames[i]);
  }
  const char* null_col[] = {NULL};
  CHECK_ERR(
      db,
      perk_update(db, "users", null_col, set, 1, "id = ?", id1, 1, &changed),
      PERK_ERR_BADNAME);
  // A bad name in the WHERE is rejected too
  CHECK_ERR(
      db,
      perk_update(db, "users", cols, set, 1, "bad-name = ?", id1, 1, &changed),
      PERK_ERR_MISUSE);

  // Malformed values
  PerkValue bad_type[] = {{.type = (PerkType)99}};
  CHECK_ERR(
      db,
      perk_update(db, "users", cols, bad_type, 1, "id = ?", id1, 1, &changed),
      PERK_ERR_MISUSE);
  PerkValue bad_arg[] = {{.type = (PerkType)99}};
  CHECK_ERR(
      db,
      perk_update(db, "users", cols, set, 1, "id = ?", bad_arg, 1, &changed),
      PERK_ERR_MISUSE);

  // SQL errors are not NOT_FOUND
  CHECK_ERR(
      db,
      perk_update(db, "nosuchtable", cols, set, 1, "id = ?", id1, 1, &changed),
      PERK_ERR_SQL);
  const char* unknown[] = {"nosuchcol"};
  CHECK_ERR(
      db, perk_update(db, "users", unknown, set, 1, "id = ?", id1, 1, &changed),
      PERK_ERR_SQL);
  CHECK_ERR(
      db,
      perk_update(db, "users", cols, set, 1, "nosuchcol = ?", id1, 1, &changed),
      PERK_ERR_SQL);

  CHECK_UNCHANGED(db, before);
  perk_close(db);
}

/** A constraint violation is PERK_ERR_SQL and the whole update is rolled back.
 */
static void test_update_constraint(void) {
  printf("update: constraint violation\n");
  PerkDB* db = NULL;
  if (!ASSERT_EQ_INT(perk_open(":memory:", &db), PERK_OK)) return;
  CHECK_ERR(db, perk_create_table(db, "uniq", kUniq, 2), PERK_OK);
  const char* nc[] = {"name"};
  const char* names[] = {"a", "b", "c"};
  for (size_t i = 0; i < 3; i++) {
    PerkValue v = perk_text(names[i]);
    CHECK_ERR(db, perk_insert(db, "uniq", nc, &v, 1, NULL), PERK_OK);
  }

  // Collides on the second row: no row may keep the new value
  PerkValue same[] = {perk_text("same")};
  int64_t changed = 77;
  CHECK_ERR(db, perk_update(db, "uniq", nc, same, 1, NULL, NULL, 0, &changed),
            PERK_ERR_SQL);
  PerkResult* res = NULL;
  PerkValue key = perk_text("same");
  CHECK_ERR(db, perk_select_by_id(db, "uniq", NULL, 0, "name", &key, &res),
            PERK_ERR_NOT_FOUND);
  ASSERT_EQ_INT(count_rows(db, "uniq"), 3);

  // Setting NOT NULL column to NULL
  PerkValue nul[] = {perk_null()};
  PerkValue id1[] = {perk_int(1)};
  CHECK_ERR(db, perk_update(db, "uniq", nc, nul, 1, "id = ?", id1, 1, &changed),
            PERK_ERR_SQL);
  PerkValue a = perk_text("a");
  res = NULL;
  CHECK_ERR(db, perk_select_by_id(db, "uniq", NULL, 0, "name", &a, &res),
            PERK_OK);
  perk_result_free(res);

  perk_close(db);
}

// ---------------------------------------------------------------- delete ----

/** Delete one row by id; others stay. */
static void test_delete_happy(void) {
  printf("delete: happy path\n");
  PerkDB* db = open_users();
  if (db == NULL) return;
  seed(db);

  PerkValue id2[] = {perk_int(2)};
  int64_t changed = -1;
  CHECK_ERR(db, perk_delete(db, "users", "id = ?", id2, 1, &changed), PERK_OK);
  ASSERT_EQ_INT(changed, 1);
  ASSERT_TRUE(!row_exists(db, 2));
  ASSERT_TRUE(row_exists(db, 1));
  ASSERT_TRUE(row_exists(db, 3));
  ASSERT_EQ_INT(count_rows(db, "users"), 2);

  // changed may be NULL
  PerkValue id1[] = {perk_int(1)};
  CHECK_ERR(db, perk_delete(db, "users", "id = ?", id1, 1, NULL), PERK_OK);
  ASSERT_TRUE(!row_exists(db, 1));
  ASSERT_EQ_INT(count_rows(db, "users"), 1);

  // The table still accepts inserts, and the deleted id is not found
  ASSERT_TRUE(add_user(db, "dave", 40) > 0);
  ASSERT_EQ_INT(count_rows(db, "users"), 2);

  perk_close(db);
}

/** Multi-row deletes report how many rows went. */
static void test_delete_multi_row(void) {
  printf("delete: multi-row changed counts\n");
  PerkDB* db = open_users();
  if (db == NULL) return;
  seed(db);

  PerkValue age30[] = {perk_int(30)};
  int64_t changed = -1;
  CHECK_ERR(db, perk_delete(db, "users", "age = ?", age30, 1, &changed),
            PERK_OK);
  ASSERT_EQ_INT(changed, 2);
  ASSERT_EQ_INT(count_rows(db, "users"), 1);
  ASSERT_TRUE(row_exists(db, 2));

  // Compound filter
  for (int i = 0; i < 10; i++) add_user(db, i % 2 ? "odd" : "even", i);
  PerkValue args[] = {perk_text("odd"), perk_int(5)};
  CHECK_ERR(db,
            perk_delete(db, "users", "name = ? AND age < ?", args, 2, &changed),
            PERK_OK);
  ASSERT_EQ_INT(changed, 2);  // odd rows with ages 1 and 3
  ASSERT_EQ_INT(count_rows(db, "users"), 9);

  perk_close(db);
}

/** NULL / "" where deletes everything; deleting from an empty table is
 * NOT_FOUND. */
static void test_delete_all(void) {
  printf("delete: delete-all and empty table\n");
  PerkDB* db = open_users();
  if (db == NULL) return;

  int64_t changed = 99;
  CHECK_ERR(db, perk_delete(db, "users", NULL, NULL, 0, &changed),
            PERK_ERR_NOT_FOUND);
  ASSERT_EQ_INT(changed, 0);

  seed(db);
  changed = -1;
  CHECK_ERR(db, perk_delete(db, "users", NULL, NULL, 0, &changed), PERK_OK);
  ASSERT_EQ_INT(changed, 3);
  ASSERT_EQ_INT(count_rows(db, "users"), 0);

  seed(db);
  changed = -1;
  CHECK_ERR(db, perk_delete(db, "users", "", NULL, 0, &changed), PERK_OK);
  ASSERT_EQ_INT(changed, 3);
  ASSERT_EQ_INT(count_rows(db, "users"), 0);

  // Second delete-all finds nothing
  changed = 99;
  CHECK_ERR(db, perk_delete(db, "users", "", NULL, 0, &changed),
            PERK_ERR_NOT_FOUND);
  ASSERT_EQ_INT(changed, 0);

  // A big table
  for (int i = 0; i < 500; i++) add_user(db, "bulk", i);
  CHECK_ERR(db, perk_delete(db, "users", NULL, NULL, 0, &changed), PERK_OK);
  ASSERT_EQ_INT(changed, 500);

  perk_close(db);
}

/** Missing rows: PERK_ERR_NOT_FOUND, changed == 0, data unchanged. */
static void test_delete_missing(void) {
  printf("delete: missing rows\n");
  PerkDB* db = open_users();
  if (db == NULL) return;
  seed(db);
  char before[SNAP_CAP];
  snapshot(db, before, sizeof(before));

  int64_t missing[] = {0, 4, -1, INT64_MAX, INT64_MIN};
  for (size_t i = 0; i < sizeof(missing) / sizeof(missing[0]); i++) {
    PerkValue a[] = {perk_int(missing[i])};
    int64_t changed = 99;
    PerkErr e = perk_delete(db, "users", "id = ?", a, 1, &changed);
    if (!ASSERT_EQ_INT(e, PERK_ERR_NOT_FOUND))
      printf("  id: %lld\n", (long long)missing[i]);
    ASSERT_EQ_INT(changed, 0);
  }
  ASSERT_TRUE(strlen(perk_errmsg(db)) > 0);
  PerkValue a9[] = {perk_int(9)};
  CHECK_ERR(db, perk_delete(db, "users", "id = ?", a9, 1, NULL),
            PERK_ERR_NOT_FOUND);
  PerkValue an[] = {perk_null()};
  CHECK_ERR(db, perk_delete(db, "users", "id = ?", an, 1, NULL),
            PERK_ERR_NOT_FOUND);
  CHECK_UNCHANGED(db, before);

  // Deleting the same row twice: OK then NOT_FOUND
  PerkValue id1[] = {perk_int(1)};
  int64_t changed = -1;
  CHECK_ERR(db, perk_delete(db, "users", "id = ?", id1, 1, &changed), PERK_OK);
  ASSERT_EQ_INT(changed, 1);
  changed = 99;
  CHECK_ERR(db, perk_delete(db, "users", "id = ?", id1, 1, &changed),
            PERK_ERR_NOT_FOUND);
  ASSERT_EQ_INT(changed, 0);
  ASSERT_EQ_INT(count_rows(db, "users"), 2);

  perk_close(db);
}

/** Injection-shaped values never widen a delete. */
static void test_delete_injection(void) {
  printf("delete: injection-shaped values\n");
  PerkDB* db = open_users();
  if (db == NULL) return;
  seed(db);
  char before[SNAP_CAP];
  snapshot(db, before, sizeof(before));

  // Injection-shaped args that match no row: NOT_FOUND, table intact
  for (size_t i = 0; i < N_INJECTIONS; i++) {
    PerkValue a[] = {perk_text(kInjections[i])};
    int64_t changed = 99;
    PerkErr e = perk_delete(db, "users", "name = ?", a, 1, &changed);
    if (!ASSERT_EQ_INT(e, PERK_ERR_NOT_FOUND)) printf("  %s\n", kInjections[i]);
    ASSERT_EQ_INT(changed, 0);
    e = perk_delete(db, "users", "id = ?", a, 1, &changed);
    if (!ASSERT_EQ_INT(e, PERK_ERR_NOT_FOUND)) printf("  %s\n", kInjections[i]);
  }
  const char* idshaped[] = {"1 OR 1=1", "1; DROP TABLE users", "2 --"};
  for (size_t i = 0; i < 3; i++) {
    PerkValue a[] = {perk_text(idshaped[i])};
    PerkErr e = perk_delete(db, "users", "id = ?", a, 1, NULL);
    if (!ASSERT_EQ_INT(e, PERK_ERR_NOT_FOUND)) printf("  %s\n", idshaped[i]);
  }
  CHECK_UNCHANGED(db, before);

  // A row that literally holds an injection string is deleted alone
  int64_t id = add_user(db, kInjections[1], 1);
  PerkValue a[] = {perk_text(kInjections[1])};
  int64_t changed = -1;
  CHECK_ERR(db, perk_delete(db, "users", "name = ?", a, 1, &changed), PERK_OK);
  ASSERT_EQ_INT(changed, 1);
  ASSERT_TRUE(!row_exists(db, id));
  ASSERT_EQ_INT(count_rows(db, "users"), 3);
  CHECK_UNCHANGED(db, before);

  // Injection in WHERE text is rejected and nothing is deleted
  const char* bad_wheres[] = {"1 = 1", "id = 1 OR 1 = 1",
                              "id = ?; DROP TABLE users", "id = ? --",
                              "name = 'x'"};
  PerkValue one[] = {perk_int(1)};
  for (size_t i = 0; i < sizeof(bad_wheres) / sizeof(bad_wheres[0]); i++) {
    PerkErr e = perk_delete(db, "users", bad_wheres[i], one, 1, &changed);
    if (!ASSERT_TRUE(e != PERK_OK && e != PERK_ERR_NOT_FOUND))
      printf("  where: %s\n", bad_wheres[i]);
  }
  CHECK_UNCHANGED(db, before);

  perk_close(db);
}

/** NULL / empty / invalid arguments to perk_delete. */
static void test_delete_errors(void) {
  printf("delete: errors\n");
  PerkValue id1[] = {perk_int(1)};
  int64_t changed = 5;
  ASSERT_EQ_INT(perk_delete(NULL, "users", "id = ?", id1, 1, &changed),
                PERK_ERR_MISUSE);

  PerkDB* db = open_users();
  if (db == NULL) return;
  seed(db);
  char before[SNAP_CAP];
  snapshot(db, before, sizeof(before));

  CHECK_ERR(db, perk_delete(db, NULL, "id = ?", id1, 1, &changed),
            PERK_ERR_MISUSE);
  CHECK_ERR(db, perk_delete(db, NULL, NULL, NULL, 0, &changed),
            PERK_ERR_MISUSE);
  CHECK_ERR(db, perk_delete(db, "users", "id = ?", NULL, 1, &changed),
            PERK_ERR_MISUSE);
  CHECK_ERR(db, perk_delete(db, "users", "id = ?", id1, 0, &changed),
            PERK_ERR_MISUSE);
  CHECK_ERR(db, perk_delete(db, "users", "id = ? OR id = ?", id1, 1, &changed),
            PERK_ERR_MISUSE);
  CHECK_ERR(db, perk_delete(db, "users", NULL, id1, 1, &changed),
            PERK_ERR_MISUSE);
  CHECK_ERR(db, perk_delete(db, "users", "", id1, 1, &changed),
            PERK_ERR_MISUSE);
  ASSERT_TRUE(strlen(perk_errmsg(db)) > 0);
  ASSERT_EQ_INT(changed, 5);

  for (size_t i = 0; i < N_BAD_NAMES; i++) {
    PerkErr e = perk_delete(db, kBadNames[i], "id = ?", id1, 1, &changed);
    if (!ASSERT_EQ_INT(e, PERK_ERR_BADNAME))
      printf("  table name: \"%s\"\n", kBadNames[i]);
  }

  PerkValue bad[] = {{.type = (PerkType)99}};
  CHECK_ERR(db, perk_delete(db, "users", "id = ?", bad, 1, &changed),
            PERK_ERR_MISUSE);

  // SQL errors are not NOT_FOUND
  CHECK_ERR(db, perk_delete(db, "nosuchtable", "id = ?", id1, 1, &changed),
            PERK_ERR_SQL);
  CHECK_ERR(db, perk_delete(db, "users", "nosuchcol = ?", id1, 1, &changed),
            PERK_ERR_SQL);
  CHECK_ERR(db, perk_delete(db, "nosuchtable", NULL, NULL, 0, &changed),
            PERK_ERR_SQL);

  CHECK_UNCHANGED(db, before);
  perk_close(db);
}

// ------------------------------------------------------------- lifecycle ----

/** insert -> read -> update -> read -> delete -> read (NOT_FOUND). */
static void test_lifecycle(void) {
  printf("lifecycle and transactions\n");
  PerkDB* db = open_users();
  if (db == NULL) return;

  int64_t id = add_user(db, "life", 1);
  ASSERT_TRUE(id > 0);
  PerkValue key = perk_int(id);
  PerkResult* res = NULL;
  CHECK_ERR(db, perk_select_by_id(db, "users", NULL, 0, "id", &key, &res),
            PERK_OK);
  perk_result_free(res);

  const char* cols[] = {"name"};
  PerkValue set[] = {perk_text("lived")};
  int64_t changed = 0;
  CHECK_ERR(db,
            perk_update(db, "users", cols, set, 1, "id = ?", &key, 1, &changed),
            PERK_OK);
  ASSERT_EQ_INT(changed, 1);
  ASSERT_TRUE(text_is(db, id, "name", "lived"));

  // A rolled-back update and delete restore the data
  CHECK_ERR(db, perk_begin(db), PERK_OK);
  PerkValue set2[] = {perk_text("temp")};
  CHECK_ERR(
      db, perk_update(db, "users", cols, set2, 1, "id = ?", &key, 1, &changed),
      PERK_OK);
  CHECK_ERR(db, perk_delete(db, "users", "id = ?", &key, 1, &changed), PERK_OK);
  ASSERT_TRUE(!row_exists(db, id));
  CHECK_ERR(db, perk_rollback(db), PERK_OK);
  ASSERT_TRUE(text_is(db, id, "name", "lived"));

  // A committed delete sticks
  CHECK_ERR(db, perk_begin(db), PERK_OK);
  CHECK_ERR(db, perk_delete(db, "users", "id = ?", &key, 1, &changed), PERK_OK);
  CHECK_ERR(db, perk_commit(db), PERK_OK);
  res = NULL;
  CHECK_ERR(db, perk_select_by_id(db, "users", NULL, 0, "id", &key, &res),
            PERK_ERR_NOT_FOUND);
  CHECK_ERR(db,
            perk_update(db, "users", cols, set, 1, "id = ?", &key, 1, &changed),
            PERK_ERR_NOT_FOUND);
  CHECK_ERR(db, perk_delete(db, "users", "id = ?", &key, 1, &changed),
            PERK_ERR_NOT_FOUND);
  ASSERT_EQ_INT(count_rows(db, "users"), 0);

  perk_close(db);
}

int main(void) {
  printf("PerkDB CRUD suite\n");
  test_insert_happy();
  test_insert_value_types();
  test_insert_injection();
  test_insert_errors();
  test_insert_constraints();
  test_select_list_all();
  test_select_errors();
  test_select_where();
  test_select_by_id();
  test_select_by_id_null_key();
  test_select_by_id_injection();
  test_select_by_id_errors();
  test_update_happy();
  test_update_multi_row();
  test_update_missing();
  test_update_injection();
  test_update_errors();
  test_update_constraint();
  test_delete_happy();
  test_delete_multi_row();
  test_delete_all();
  test_delete_missing();
  test_delete_injection();
  test_delete_errors();
  test_lifecycle();
  return harness_summary();
}
