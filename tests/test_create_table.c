// Tests for perk_create_table: happy path, duplicate tables (PERK_ERR_EXISTS),
// invalid names (PERK_ERR_BADNAME), and NULL / missing arguments
// (PERK_ERR_MISUSE). Every case also checks whether the table actually exists
// afterwards by looking at sqlite_master directly.

#include <stdio.h>
#include <string.h>

#include "perk_internal.h"
#include "perkdb.h"
#include "test_harness.h"

// Checks a PerkErr result and, on failure, also prints the last error message
#define CHECK_ERR(db, expr, expected)                                          \
  do {                                                                         \
    PerkErr e_ = (expr);                                                       \
    if (!harness_eq_int(__FILE__, __LINE__, #expr, #expected, e_, (expected))) \
      printf("  errmsg:   %s\n", perk_errmsg(db));                             \
  } while (0)

static const PerkColumn kUsers[] = {
    {"id", "INTEGER", PERK_COL_PK},
    {"name", "TEXT", PERK_COL_NOTNULL | PERK_COL_UNIQUE},
    {"age", "INT", 0},
};

/**
 * Opens a fresh in-memory database.
 * @return the handle, or NULL (after recording a failed check) on error
 */
static PerkDB* open_memory(void) {
  PerkDB* db = NULL;
  if (!ASSERT_EQ_INT(perk_open(":memory:", &db), PERK_OK))
    printf("  open failed: %s\n", perk_errmsg(NULL));
  return db;
}

/**
 * Counts objects named `name` (exact, case-sensitive) in the main schema.
 * @param db open handle
 * @param name object name to look for
 * @param type object type ("table", "index", ...) or NULL for any type
 * @return number of matching objects, or -1 if the query failed
 */
static int schema_count(PerkDB* db, const char* name, const char* type) {
  sqlite3_stmt* st = NULL;
  const char* sql =
      "SELECT count(*) FROM main.sqlite_master WHERE name = ?1 "
      "AND (?2 IS NULL OR type = ?2)";
  if (sqlite3_prepare_v2(db->conn, sql, -1, &st, NULL) != SQLITE_OK) return -1;
  sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
  sqlite3_bind_text(st, 2, type, -1, SQLITE_STATIC);
  int n = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : -1;
  sqlite3_finalize(st);
  return n;
}

/**
 * @param db open handle
 * @param name table name
 * @return 1 if a table with exactly this name exists in the main schema
 */
static int table_exists(PerkDB* db, const char* name) {
  return schema_count(db, name, "table") == 1;
}

/**
 * @param db open handle
 * @return total number of objects in the main schema, or -1 on error
 */
static int schema_size(PerkDB* db) {
  sqlite3_stmt* st = NULL;
  if (sqlite3_prepare_v2(db->conn, "SELECT count(*) FROM main.sqlite_master",
                         -1, &st, NULL) != SQLITE_OK)
    return -1;
  int n = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : -1;
  sqlite3_finalize(st);
  return n;
}

/**
 * Returns the stored CREATE statement for a table.
 * @param db open handle
 * @param name table name
 * @param buf output buffer, set to "" if the table is missing
 * @param cap size of buf
 */
static void table_sql(PerkDB* db, const char* name, char* buf, size_t cap) {
  sqlite3_stmt* st = NULL;
  buf[0] = '\0';
  if (sqlite3_prepare_v2(db->conn,
                         "SELECT sql FROM main.sqlite_master WHERE name = ?1",
                         -1, &st, NULL) != SQLITE_OK)
    return;
  sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
  if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0) != NULL)
    snprintf(buf, cap, "%s", (const char*)sqlite3_column_text(st, 0));
  sqlite3_finalize(st);
}

/**
 * Counts the rows of a table through the public API.
 * @param db open handle
 * @param table table name
 * @return row count, or -1 if the select failed
 */
static long long count_rows(PerkDB* db, const char* table) {
  PerkResult* res = NULL;
  if (perk_select(db, table, NULL, 0, NULL, NULL, 0, &res) != PERK_OK)
    return -1;
  long long n = (long long)perk_result_rows(res);
  perk_result_free(res);
  return n;
}

/** Creates tables with single, composite and untyped columns and uses them. */
static void test_happy_path(void) {
  printf("happy path\n");
  PerkDB* db = open_memory();
  if (db == NULL) return;

  ASSERT_TRUE(!table_exists(db, "users"));
  CHECK_ERR(db, perk_create_table(db, "users", kUsers, 3), PERK_OK);
  ASSERT_TRUE(table_exists(db, "users"));

  char sql[512];
  table_sql(db, "users", sql, sizeof(sql));
  ASSERT_EQ_STR(sql,
                "CREATE TABLE \"users\" (\"id\" INTEGER PRIMARY KEY, \"name\" "
                "TEXT NOT NULL UNIQUE, \"age\" INT)");

  // The table is usable through the rest of the API
  const char* cols[] = {"name", "age"};
  PerkValue vals[] = {perk_text("Yazan"), perk_int(21)};
  int64_t id = 0;
  CHECK_ERR(db, perk_insert(db, "users", cols, vals, 2, &id), PERK_OK);
  ASSERT_EQ_INT(id, 1);
  ASSERT_EQ_INT(count_rows(db, "users"), 1);

  // Composite primary key
  PerkColumn pair[] = {{"a", "INT", PERK_COL_PK}, {"b", "INT", PERK_COL_PK}};
  CHECK_ERR(db, perk_create_table(db, "pairs", pair, 2), PERK_OK);
  ASSERT_TRUE(table_exists(db, "pairs"));
  table_sql(db, "pairs", sql, sizeof(sql));
  ASSERT_EQ_STR(sql,
                "CREATE TABLE \"pairs\" (\"a\" INT, \"b\" INT, PRIMARY KEY "
                "(\"a\", \"b\"))");

  // Untyped column, lower-case type, keyword table name, 64-character name
  PerkColumn misc[] = {{"v", NULL, 0}, {"w", "real", 0}};
  CHECK_ERR(db, perk_create_table(db, "misc", misc, 2), PERK_OK);
  ASSERT_TRUE(table_exists(db, "misc"));
  PerkColumn one[] = {{"select", "TEXT", 0}};
  CHECK_ERR(db, perk_create_table(db, "order", one, 1), PERK_OK);
  ASSERT_TRUE(table_exists(db, "order"));
  char longest[PERK_MAX_IDENT + 1];
  memset(longest, 'a', PERK_MAX_IDENT);
  longest[PERK_MAX_IDENT] = '\0';
  CHECK_ERR(db, perk_create_table(db, longest, one, 1), PERK_OK);
  ASSERT_TRUE(table_exists(db, longest));
  CHECK_ERR(db, perk_create_table(db, "_under_score9", one, 1), PERK_OK);
  ASSERT_TRUE(table_exists(db, "_under_score9"));

  perk_close(db);
}

/** A name that is already taken returns PERK_ERR_EXISTS and changes nothing. */
static void test_duplicate(void) {
  printf("duplicate table\n");
  PerkDB* db = open_memory();
  if (db == NULL) return;

  CHECK_ERR(db, perk_create_table(db, "users", kUsers, 3), PERK_OK);
  const char* cols[] = {"name"};
  PerkValue vals[] = {perk_text("kept")};
  CHECK_ERR(db, perk_insert(db, "users", cols, vals, 1, NULL), PERK_OK);
  char before[512];
  table_sql(db, "users", before, sizeof(before));
  int objects = schema_size(db);

  // Same schema
  CHECK_ERR(db, perk_create_table(db, "users", kUsers, 3), PERK_ERR_EXISTS);
  ASSERT_TRUE(strstr(perk_errmsg(db), "already exists") != NULL);
  ASSERT_TRUE(strstr(perk_errmsg(db), "users") != NULL);

  // Different schema: the original table must be left as it was
  PerkColumn other[] = {{"x", "BLOB", 0}};
  CHECK_ERR(db, perk_create_table(db, "users", other, 1), PERK_ERR_EXISTS);

  // SQLite table names are case-insensitive
  CHECK_ERR(db, perk_create_table(db, "USERS", kUsers, 3), PERK_ERR_EXISTS);
  CHECK_ERR(db, perk_create_table(db, "Users", other, 1), PERK_ERR_EXISTS);
  ASSERT_EQ_INT(schema_count(db, "USERS", NULL), 0);

  char after[512];
  table_sql(db, "users", after, sizeof(after));
  ASSERT_EQ_STR(after, before);
  ASSERT_EQ_INT(schema_size(db), objects);
  ASSERT_EQ_INT(count_rows(db, "users"), 1);

  // Names taken by an index or a view are also reported as existing
  ASSERT_EQ_INT(sqlite3_exec(db->conn,
                             "CREATE INDEX idx_age ON users(age);"
                             "CREATE VIEW v_users AS SELECT name FROM users;",
                             NULL, NULL, NULL),
                SQLITE_OK);
  CHECK_ERR(db, perk_create_table(db, "idx_age", other, 1), PERK_ERR_EXISTS);
  ASSERT_TRUE(strstr(perk_errmsg(db), "index") != NULL);
  ASSERT_EQ_INT(schema_count(db, "idx_age", "table"), 0);
  CHECK_ERR(db, perk_create_table(db, "v_users", other, 1), PERK_ERR_EXISTS);
  ASSERT_TRUE(strstr(perk_errmsg(db), "view") != NULL);
  ASSERT_EQ_INT(schema_count(db, "v_users", "table"), 0);

  // The rest of the API still works on the original table
  ASSERT_EQ_INT(count_rows(db, "users"), 1);
  CHECK_ERR(db, perk_create_table(db, "fresh", other, 1), PERK_OK);
  ASSERT_TRUE(table_exists(db, "fresh"));

  perk_close(db);
}

/** A table created in a rolled-back transaction is gone and can be recreated.
 */
static void test_duplicate_transaction(void) {
  printf("duplicate inside transactions\n");
  PerkDB* db = open_memory();
  if (db == NULL) return;

  CHECK_ERR(db, perk_begin(db), PERK_OK);
  CHECK_ERR(db, perk_create_table(db, "t", kUsers, 3), PERK_OK);
  CHECK_ERR(db, perk_create_table(db, "t", kUsers, 3), PERK_ERR_EXISTS);
  ASSERT_TRUE(table_exists(db, "t"));
  CHECK_ERR(db, perk_rollback(db), PERK_OK);
  ASSERT_TRUE(!table_exists(db, "t"));

  CHECK_ERR(db, perk_create_table(db, "t", kUsers, 3), PERK_OK);
  ASSERT_TRUE(table_exists(db, "t"));

  perk_close(db);
}

/** A second connection to the same file sees the first one's table. */
static void test_duplicate_two_connections(void) {
  printf("duplicate across connections\n");
  const char* path = "build/test_create_table.db";
  const char* wal = "build/test_create_table.db-wal";
  const char* shm = "build/test_create_table.db-shm";
  remove(path);
  remove(wal);
  remove(shm);

  PerkDB* a = NULL;
  PerkDB* b = NULL;
  if (!ASSERT_EQ_INT(perk_open(path, &a), PERK_OK) ||
      !ASSERT_EQ_INT(perk_open(path, &b), PERK_OK)) {
    printf("  open failed: %s\n", perk_errmsg(NULL));
    perk_close(a);
    perk_close(b);
    return;
  }

  // b caches the (empty) schema before a creates the table
  ASSERT_TRUE(!table_exists(b, "shared"));
  CHECK_ERR(a, perk_create_table(a, "shared", kUsers, 3), PERK_OK);
  CHECK_ERR(b, perk_create_table(b, "shared", kUsers, 3), PERK_ERR_EXISTS);
  ASSERT_TRUE(table_exists(b, "shared"));

  perk_close(a);
  perk_close(b);

  // The table persists across reopen and is still reported as existing
  PerkDB* c = NULL;
  if (ASSERT_EQ_INT(perk_open(path, &c), PERK_OK)) {
    CHECK_ERR(c, perk_create_table(c, "shared", kUsers, 3), PERK_ERR_EXISTS);
    ASSERT_TRUE(table_exists(c, "shared"));
  }
  perk_close(c);
  remove(path);
  remove(wal);
  remove(shm);
}

/** Invalid table or column names return PERK_ERR_BADNAME and create nothing. */
static void test_invalid_names(void) {
  printf("invalid names\n");
  PerkDB* db = open_memory();
  if (db == NULL) return;

  CHECK_ERR(db, perk_create_table(db, "users", kUsers, 3), PERK_OK);
  int objects = schema_size(db);

  char too_long[PERK_MAX_IDENT + 2];
  memset(too_long, 'a', PERK_MAX_IDENT + 1);
  too_long[PERK_MAX_IDENT + 1] = '\0';

  const char* bad[] = {
      "",
      " ",
      "1abc",
      "9",
      "a b",
      "a-b",
      "a.b",
      "main.t",
      "a$b",
      "tabl\xc3\xa9",
      too_long,
      "sqlite_x",
      "SQLITE_master",
      "sqlite_master",
      "users; DROP TABLE users; --",
      "t\" (x); DROP TABLE users; --",
      "t' OR '1'='1",
      "t`x`",
      "[t]",
      "t/*c*/",
      "t\n",
      "users--",
  };
  PerkColumn one[] = {{"x", "TEXT", 0}};
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    PerkErr e = perk_create_table(db, bad[i], one, 1);
    if (!ASSERT_EQ_INT(e, PERK_ERR_BADNAME)) printf("  name: \"%s\"\n", bad[i]);
    ASSERT_TRUE(strlen(perk_errmsg(db)) > 0);
    if (!ASSERT_EQ_INT(schema_count(db, bad[i], NULL), 0))
      printf("  name: \"%s\"\n", bad[i]);
  }

  // Invalid column names are rejected the same way
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    PerkColumn c[] = {{"ok", "INT", 0}, {bad[i], "TEXT", 0}};
    PerkErr e = perk_create_table(db, "colcheck", c, 2);
    if (!ASSERT_EQ_INT(e, PERK_ERR_BADNAME))
      printf("  column: \"%s\"\n", bad[i]);
  }
  ASSERT_TRUE(!table_exists(db, "colcheck"));

  // A NULL column name or an unsupported type is a bad name too
  PerkColumn null_col[] = {{NULL, "TEXT", 0}};
  CHECK_ERR(db, perk_create_table(db, "t1", null_col, 1), PERK_ERR_BADNAME);
  PerkColumn bad_type[] = {{"x", "TEXT); DROP TABLE users; --", 0}};
  CHECK_ERR(db, perk_create_table(db, "t1", bad_type, 1), PERK_ERR_BADNAME);
  ASSERT_TRUE(!table_exists(db, "t1"));

  // Validation runs before the existence check
  CHECK_ERR(db, perk_create_table(db, "users", null_col, 1), PERK_ERR_BADNAME);

  // Nothing was created or dropped by any of the attempts
  ASSERT_EQ_INT(schema_size(db), objects);
  ASSERT_TRUE(table_exists(db, "users"));

  perk_close(db);
}

/** NULL or missing arguments return PERK_ERR_MISUSE without crashing. */
static void test_null_args(void) {
  printf("NULL args\n");
  ASSERT_EQ_INT(perk_create_table(NULL, "t", kUsers, 3), PERK_ERR_MISUSE);
  ASSERT_EQ_INT(perk_create_table(NULL, NULL, NULL, 0), PERK_ERR_MISUSE);

  PerkDB* db = open_memory();
  if (db == NULL) return;

  CHECK_ERR(db, perk_create_table(db, NULL, kUsers, 3), PERK_ERR_MISUSE);
  ASSERT_TRUE(strlen(perk_errmsg(db)) > 0);
  CHECK_ERR(db, perk_create_table(db, "t", NULL, 3), PERK_ERR_MISUSE);
  CHECK_ERR(db, perk_create_table(db, "t", NULL, 0), PERK_ERR_MISUSE);
  CHECK_ERR(db, perk_create_table(db, "t", kUsers, 0), PERK_ERR_MISUSE);
  CHECK_ERR(db, perk_create_table(db, NULL, NULL, 0), PERK_ERR_MISUSE);
  PerkColumn bad_flags[] = {{"x", "INT", 1 << 7}};
  CHECK_ERR(db, perk_create_table(db, "t", bad_flags, 1), PERK_ERR_MISUSE);

  ASSERT_TRUE(!table_exists(db, "t"));
  ASSERT_EQ_INT(schema_size(db), 0);

  // The handle is still usable afterwards
  CHECK_ERR(db, perk_create_table(db, "t", kUsers, 3), PERK_OK);
  ASSERT_TRUE(table_exists(db, "t"));

  perk_close(db);
}

/** Wide schemas, SQLite-level failures, and recovery after failed creates. */
static void test_wide_and_recovery(void) {
  printf("wide schemas and recovery\n");
  PerkDB* db = open_memory();
  if (db == NULL) return;

  // 100 columns with generated names
  enum { N = 100 };
  static char names[N][16];
  PerkColumn wide[N];
  for (int i = 0; i < N; i++) {
    snprintf(names[i], sizeof(names[i]), "col_%d", i);
    wide[i].name = names[i];
    wide[i].type = (i % 2) ? "TEXT" : "INTEGER";
    wide[i].flags = 0;
  }
  CHECK_ERR(db, perk_create_table(db, "wide", wide, N), PERK_OK);
  ASSERT_TRUE(table_exists(db, "wide"));
  sqlite3_stmt* st = NULL;
  int ncols = -1;
  if (sqlite3_prepare_v2(db->conn, "SELECT * FROM \"wide\"", -1, &st, NULL) ==
      SQLITE_OK)
    ncols = sqlite3_column_count(st);
  sqlite3_finalize(st);
  ASSERT_EQ_INT(ncols, N);

  // Duplicate column names fail inside SQLite: an error, and no table
  PerkColumn dup[] = {{"a", "INT", 0}, {"a", "TEXT", 0}};
  PerkErr e = perk_create_table(db, "dupcols", dup, 2);
  ASSERT_TRUE(e != PERK_OK);
  ASSERT_TRUE(strlen(perk_errmsg(db)) > 0);
  ASSERT_TRUE(!table_exists(db, "dupcols"));

  // Same name works once the schema is fixed, after both a BADNAME and a
  // SQL failure on the same connection
  PerkColumn good[] = {{"a", "INT", 0}, {"b", "TEXT", 0}};
  CHECK_ERR(db, perk_create_table(db, "bad name", good, 2), PERK_ERR_BADNAME);
  CHECK_ERR(db, perk_create_table(db, "dupcols", good, 2), PERK_OK);
  ASSERT_TRUE(table_exists(db, "dupcols"));
  CHECK_ERR(db, perk_create_table(db, "dupcols", good, 2), PERK_ERR_EXISTS);

  // A successful create after an EXISTS clears the stale error message state
  CHECK_ERR(db, perk_create_table(db, "another", good, 2), PERK_OK);
  ASSERT_TRUE(table_exists(db, "another"));

  // Single-character names and a column named like its table
  PerkColumn same[] = {{"t", "INT", 0}};
  CHECK_ERR(db, perk_create_table(db, "t", same, 1), PERK_OK);
  ASSERT_TRUE(table_exists(db, "t"));

  perk_close(db);
}

int main(void) {
  printf("PerkDB create-table suite\n");
  test_wide_and_recovery();
  test_happy_path();
  test_duplicate();
  test_duplicate_transaction();
  test_duplicate_two_connections();
  test_invalid_names();
  test_null_args();
  return harness_summary();
}
