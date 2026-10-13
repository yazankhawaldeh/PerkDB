// SQL injection hardening tests.
//
// Every public function that takes a string is fed hostile identifiers, WHERE
// text and values. Rejected calls must return the documented error and leave a
// full snapshot (schema listing + every row of every table) unchanged; values
// must be stored and matched byte-exact and never be interpreted as SQL.
// Also covers the identifier regex exhaustively and the runtime connection
// hardening (defensive mode, no ATTACH, no load_extension, no DQS).

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "perk_internal.h"
#include "test_harness.h"

#define MASK(e) (1u << (e))
#define M_BAD MASK(PERK_ERR_BADNAME)
#define M_WHERE \
  (MASK(PERK_ERR_MISUSE) | MASK(PERK_ERR_BADNAME) | MASK(PERK_ERR_SQL))

static const char kToken[] = "TOP-SECRET-TOKEN-9f3a";
static const char kFlag[] = "SENTINEL-INTACT";

/* ---------------------------------------------------------------- snapshot */

typedef struct {
  char* p;
  size_t n, cap;
} Str;

/**
 * Appends len bytes to s (always NUL terminated).
 */
static void str_add(Str* s, const char* data, size_t len) {
  if (s->n + len + 1 > s->cap) {
    size_t cap = s->cap ? s->cap * 2 : 1024;
    while (cap < s->n + len + 1) cap *= 2;
    s->p = realloc(s->p, cap);
    s->cap = cap;
  }
  memcpy(s->p + s->n, data, len);
  s->n += len;
  s->p[s->n] = '\0';
}

static void str_hex(Str* s, const unsigned char* d, int n) {
  static const char hex[] = "0123456789abcdef";
  for (int i = 0; i < n; i++) {
    char two[2] = {hex[d[i] >> 4], hex[d[i] & 15]};
    str_add(s, two, 2);
  }
}

/**
 * Appends every row of a query as text; blobs and text are hex encoded.
 */
static void dump_query(sqlite3* c, Str* s, const char* sql) {
  sqlite3_stmt* st = NULL;
  if (sqlite3_prepare_v2(c, sql, -1, &st, NULL) != SQLITE_OK) {
    str_add(s, "<prepare failed>\n", 17);
    return;
  }
  int ncol = sqlite3_column_count(st);
  while (sqlite3_step(st) == SQLITE_ROW) {
    for (int i = 0; i < ncol; i++) {
      char tmp[64];
      switch (sqlite3_column_type(st, i)) {
        case SQLITE_INTEGER:
          snprintf(tmp, sizeof(tmp), "I%lld",
                   (long long)sqlite3_column_int64(st, i));
          str_add(s, tmp, strlen(tmp));
          break;
        case SQLITE_FLOAT:
          snprintf(tmp, sizeof(tmp), "F%.17g", sqlite3_column_double(st, i));
          str_add(s, tmp, strlen(tmp));
          break;
        case SQLITE_TEXT: {
          const unsigned char* t = sqlite3_column_text(st, i);
          int n = sqlite3_column_bytes(st, i);
          str_add(s, "T", 1);
          str_hex(s, t, n);
          break;
        }
        case SQLITE_BLOB: {
          const unsigned char* t = sqlite3_column_blob(st, i);
          int n = sqlite3_column_bytes(st, i);
          str_add(s, "B", 1);
          if (t) str_hex(s, t, n);
          break;
        }
        default:
          str_add(s, "N", 1);
      }
      str_add(s, "|", 1);
    }
    str_add(s, "\n", 1);
  }
  sqlite3_finalize(st);
}

static char* dup_str(const char* src) {
  size_t n = strlen(src) + 1;
  char* out = malloc(n);
  memcpy(out, src, n);
  return out;
}

/**
 * Full picture of the database: schema listing, user_version and every row of
 * every table (the "users" table only when include_users is set).
 * @return malloc'd string; caller frees
 */
static char* snapshot(PerkDB* db, int include_users) {
  Str s = {0};
  sqlite3* c = db->conn;
  str_add(&s, "", 0);
  dump_query(c, &s,
             "SELECT type,name,tbl_name,rootpage,sql FROM sqlite_master "
             "ORDER BY name");
  dump_query(c, &s, "PRAGMA user_version");

  char* names[256];
  int nn = 0;
  sqlite3_stmt* st = NULL;
  if (sqlite3_prepare_v2(
          c,
          "SELECT name FROM sqlite_master WHERE type='table' "
          "AND name NOT LIKE 'sqlite!_%' ESCAPE '!' ORDER BY name",
          -1, &st, NULL) == SQLITE_OK) {
    while (nn < 256 && sqlite3_step(st) == SQLITE_ROW)
      names[nn++] = dup_str((const char*)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
  }
  for (int i = 0; i < nn; i++) {
    if (include_users || strcmp(names[i], "users") != 0) {
      char* sql = sqlite3_mprintf("SELECT rowid,* FROM \"%w\" ORDER BY rowid",
                                  names[i]);
      str_add(&s, names[i], strlen(names[i]));
      str_add(&s, ":\n", 2);
      dump_query(c, &s, sql);
      sqlite3_free(sql);
    }
    free(names[i]);
  }
  if (include_users) {
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "changes=%d\n", sqlite3_total_changes(c));
    str_add(&s, tmp, strlen(tmp));
  }
  return s.p;
}

/* ---------------------------------------------------------------- fixture */

static PerkDB* g_db;
static char* g_base;  // snapshot(g_db, 1) taken after seeding
static char g_long65[66];
static char g_long1000[1001];

static PerkDB* open_mem(void) {
  PerkDB* db = NULL;
  if (!ASSERT_EQ_INT(perk_open(":memory:", &db), PERK_OK)) {
    printf("  open failed: %s\n", perk_errmsg(NULL));
    exit(2);
  }
  return db;
}

static void must(PerkErr e, const char* what) {
  if (!harness_true(__FILE__, __LINE__, what, e == PERK_OK))
    printf("  setup step failed with %d\n", (int)e);
}

static void seed_row(PerkDB* db, const char* table, const char** cols,
                     const PerkValue* vals, size_t n) {
  must(perk_insert(db, table, cols, vals, n, NULL), "seed insert");
}

/**
 * Builds users (3 rows), secret (token), sentinel (flag) and x (1 row).
 */
static PerkDB* open_victim(void) {
  PerkDB* db = open_mem();
  static const PerkColumn users[] = {{"id", "INTEGER", PERK_COL_PK},
                                     {"name", "TEXT", 0},
                                     {"age", "INT", 0},
                                     {"data", "BLOB", 0}};
  static const PerkColumn kv[] = {{"id", "INTEGER", PERK_COL_PK},
                                  {"v", "TEXT", 0}};
  must(perk_create_table(db, "users", users, 4), "create users");
  must(perk_create_table(db, "secret", kv, 2), "create secret");
  must(perk_create_table(db, "sentinel", kv, 2), "create sentinel");
  must(perk_create_table(db, "x", kv, 2), "create x");

  const char* uc[] = {"name", "age", "data"};
  const unsigned char blob[] = {1, 2, 3};
  PerkValue a[] = {perk_text("alice"), perk_int(30), perk_blob(blob, 3)};
  PerkValue b[] = {perk_text("bob"), perk_int(25), perk_null()};
  PerkValue c[] = {perk_text("carol"), perk_int(30), perk_null()};
  seed_row(db, "users", uc, a, 3);
  seed_row(db, "users", uc, b, 3);
  seed_row(db, "users", uc, c, 3);
  const char* vc[] = {"v"};
  PerkValue t[] = {perk_text(kToken)};
  PerkValue f[] = {perk_text(kFlag)};
  PerkValue xv[] = {perk_text("xval")};
  seed_row(db, "secret", vc, t, 1);
  seed_row(db, "sentinel", vc, f, 1);
  seed_row(db, "x", vc, xv, 1);
  return db;
}

static void fixture_up(void) {
  g_db = open_victim();
  g_base = snapshot(g_db, 1);
}

static void fixture_down(void) {
  free(g_base);
  g_base = NULL;
  perk_close(g_db);
  g_db = NULL;
}

/**
 * Asserts the db still matches the fixture snapshot.
 */
static void check_unchanged(const char* api, const char* payload) {
  char* now = snapshot(g_db, 1);
  if (!harness_true(__FILE__, __LINE__, "snapshot unchanged",
                    strcmp(now, g_base) == 0))
    printf("  after %s with payload \"%s\"\n", api, payload);
  free(now);
}

/**
 * Asserts a call returned one of the errors in mask and changed nothing.
 */
static void rejected(const char* api, const char* payload, PerkErr got,
                     unsigned mask) {
  if (!harness_true(__FILE__, __LINE__, api, ((mask >> got) & 1u) != 0))
    printf("  payload: \"%s\" got err %d (%s)\n", payload, (int)got,
           perk_errmsg(g_db));
  check_unchanged(api, payload);
}

static void rejected_select(const char* api, const char* payload, PerkErr got,
                            PerkResult* res, unsigned mask) {
  if (!harness_true(__FILE__, __LINE__, "out is NULL on failure",
                    res == NULL || got == PERK_OK))
    printf("  api %s payload \"%s\"\n", api, payload);
  if (res) perk_result_free(res);
  rejected(api, payload, got, mask);
}

/**
 * @return 1 if any text/blob cell or column name of res contains needle
 */
static int result_has(const PerkResult* res, const char* needle) {
  size_t nl = strlen(needle);
  for (size_t c = 0; c < perk_result_cols(res); c++) {
    const char* nm = perk_result_colname(res, c);
    if (nm && strstr(nm, needle)) return 1;
  }
  for (size_t r = 0; r < perk_result_rows(res); r++)
    for (size_t c = 0; c < perk_result_cols(res); c++) {
      const PerkValue* v = perk_result_get(res, r, c);
      if (v == NULL) continue;
      const char* p = NULL;
      size_t n = 0;
      if (v->type == PERK_TEXT) {
        p = v->as.text.ptr;
        n = v->as.text.len;
      } else if (v->type == PERK_BLOB) {
        p = v->as.blob.ptr;
        n = v->as.blob.len;
      }
      for (size_t i = 0; p && n >= nl && i + nl <= n; i++)
        if (memcmp(p + i, needle, nl) == 0) return 1;
    }
  return 0;
}

/* ----------------------------------------------------------------- corpora */

// Strings that must never be accepted as a table or column name
static const char* const kBadIdents[] = {
    "",
    "'; DROP TABLE x;--",
    "\" OR 1=1",
    "' OR '1'='1",
    "x\"; DROP TABLE users; --",
    "1; DELETE FROM users",
    "users; --",
    "users;",
    "users--",
    "users#",
    "users/*",
    "/**/users",
    "us/**/ers",
    "--users",
    "`users`",
    "users`",
    "[users]",
    "[users",
    "users]",
    "users\"",
    "\"users\"",
    "\"\"",
    "'users'",
    "users'",
    "us ers",
    " users",
    "users ",
    "users\t",
    "users\n",
    "users\r",
    "users\v",
    "users\f",
    "us\xC3\xA9rs",
    "\xC2\xA0users",
    "users\xC2\xA0",
    "us\302\240ers",
    "\xEF\xBC\xA1users",
    "\xFF",
    "users\x7f",
    "\x01users",
    "us\x1b[0mers",
    "sqlite_master",
    "SQLITE_MASTER",
    "sqlite_x",
    "Sqlite_Sequence",
    "sqlite_",
    "main.users",
    "main.sqlite_master",
    "temp.sqlite_master",
    "users.id",
    "users)",
    "(users",
    "users(",
    "users,secret",
    "id,name",
    "*",
    "name=?",
    "?",
    "?1",
    ":a",
    "@a",
    "$a",
    "users%",
    "users\\",
    "1users",
    "0",
    "-1",
    "UNION SELECT 1",
    "id) UNION SELECT v FROM secret --",
    "ATTACH DATABASE ':memory:' AS z",
    "ATTACH DATABASE x AS z",
    "PRAGMA writable_schema=ON",
    "load_extension('x')",
    "load_extension(x)",
    "users OR 1=1",
    "users||secret",
    "users;DROP TABLE secret",
    "id IN secret",
    "like(?,?)",
};
#define N_BAD (sizeof(kBadIdents) / sizeof(kBadIdents[0]))

typedef struct {
  const char* where;
  size_t nargs;
} WhereCase;

// WHERE texts the grammar must refuse (MISUSE, BADNAME or an SQL error)
static const WhereCase kBadWheres[] = {
    {"1=1", 0},
    {"1", 0},
    {"'; DROP TABLE x;--", 0},
    {"\" OR 1=1", 0},
    {"' OR '1'='1", 0},
    {"id = 1; DELETE FROM users", 0},
    {"id = ?; DROP TABLE users", 1},
    {"id = ? ; DROP TABLE users", 1},
    {"id = ? --", 1},
    {"id = ? -- x", 1},
    {"id = ? /* x */", 1},
    {"/* x */ id = ?", 1},
    {"id = ? #", 1},
    {"id = ? OR 1=1", 1},
    {"id = ? OR '1'='1'", 1},
    {"id = ? OR \"\"=\"\"", 1},
    {"name = 'alice'", 0},
    {"name = \"alice\"", 0},
    {"name = `alice`", 0},
    {"name = [alice]", 0},
    {"name = x'41'", 0},
    {"id = 0x10", 0},
    {"id = 1", 0},
    {"id = 1.5", 0},
    {"id = -1", 0},
    {"id = +1", 0},
    {"name || 'x' = ?", 1},
    {"id IN secret", 0},
    {"id IN main.secret", 0},
    {"id NOT IN secret", 0},
    {"id IN pragma_table_info(?)", 1},
    {"id IN sentinel", 0},
    {"id IN", 0},
    {"id IN ?", 1},
    {"id IN (SELECT id FROM secret)", 0},
    {"name IN (SELECT v FROM secret)", 0},
    {"name = (SELECT v FROM secret)", 0},
    {"EXISTS (SELECT v FROM secret)", 0},
    {"id = ? OR EXISTS (SELECT 1)", 1},
    {"id = ? UNION SELECT v FROM secret", 1},
    {"id = ? ORDER BY id", 1},
    {"id = ? LIMIT 1", 1},
    {"id = ? COLLATE NOCASE", 1},
    {"name LIKE ? ESCAPE ?", 2},
    {"like(?,?)", 2},
    {"LIKE(?,?)", 2},
    {"glob(?,?)", 2},
    {"GLOB (?, ?)", 2},
    {"id = ? AND like(?,?)", 3},
    {"NOT like(?,?)", 2},
    {"name = ? OR glob(?,?)", 3},
    {"(like(?,?))", 2},
    {"ATTACH DATABASE ':memory:' AS z", 0},
    {"ATTACH DATABASE x AS z", 0},
    {"id = ? ATTACH DATABASE x AS z", 1},
    {"PRAGMA writable_schema=ON", 0},
    {"PRAGMA writable_schema = 1", 0},
    {"load_extension('x')", 0},
    {"load_extension(x)", 0},
    {"sqlite_version()", 0},
    {"randomblob(10) = ?", 1},
    {"abs(id) = ?", 1},
    {"abs (id) = ?", 1},
    {"id = ? AND (SELECT 1)", 1},
    {"sqlite_master.name = ?", 1},
    {"sqlite_master", 0},
    {"main.users.id = ?", 1},
    {"users.id = ?", 1},
    {"id = ?1", 0},
    {"id = ?1", 1},
    {"id = ?NNN", 0},
    {"id = :a", 0},
    {"id = @a", 0},
    {"id = $a", 0},
    {"id\v= ?", 1},
    {"id\f= ?", 1},
    {"id\xC2\xA0= ?", 1},
    {"id = ?\xC2\xA0", 1},
    {"\xEF\xBB\xBFid = ?", 1},
    {"id = ?\\", 1},
    {"id = ? \\' OR 1=1", 1},
    {"id = ?)", 1},
    {"(id = ?", 1},
    {"((id = ?)", 1},
    {"id = ?", 0},
    {"id = ?", 2},
    {"id = ? AND name = ?", 1},
    {"", 1},
    {"   ", 1},
    {"id ==== ?", 1},
    {"id = = ?", 1},
    {"id = ? AND", 1},
    {"AND id = ?", 1},
    {"id ! ?", 1},
    {"id ~ ?", 1},
    {"id & ?", 1},
    {"id + ?", 1},
    {"id % ?", 1},
    {"id = ? ;", 1},
    {"name, v = ?", 1},
    {"id = ? , id = ?", 2},
    {"DROP TABLE users", 0},
    {"DELETE FROM users", 0},
    {"id = ? OR DROP TABLE users", 1},
    {"id = ? ESCAPE '\\'", 1},
    {"CREATE TABLE z(a)", 0},
    {"VACUUM", 0},
    {"WITH t AS (SELECT 1) SELECT * FROM t", 0},
    {"RETURNING *", 0},
    {"id = ? RETURNING *", 1},
};
#define N_BADW (sizeof(kBadWheres) / sizeof(kBadWheres[0]))

typedef struct {
  const char* s;
  size_t n;
} Bytes;
#define LIT(l) {l, sizeof(l) - 1}

// Payloads stored and matched as values
static const Bytes kValues[] = {
    LIT("'; DROP TABLE x;--"),
    LIT("\" OR 1=1"),
    LIT("' OR '1'='1"),
    LIT("x\"; DROP TABLE users; --"),
    LIT("Robert'); DROP TABLE users;--"),
    LIT("1; DELETE FROM users"),
    LIT("users; --"),
    LIT("-- comment"),
    LIT("/* comment */"),
    LIT("`backtick`"),
    LIT("[bracket]"),
    LIT("%' UNION SELECT v FROM secret --"),
    LIT("' UNION SELECT token, 1 FROM secret --"),
    LIT("id IN secret"),
    LIT("like(?,?)"),
    LIT("?"),
    LIT("?1"),
    LIT(":name"),
    LIT("ATTACH DATABASE ':memory:' AS z"),
    LIT("PRAGMA writable_schema=ON"),
    LIT("load_extension('x')"),
    LIT("\\'; --"),
    LIT("%"),
    LIT("_"),
    LIT("*"),
    LIT("''"),
    LIT("\"\""),
    LIT("'"),
    LIT("\""),
    LIT(""),
    LIT(" "),
    LIT("NULL"),
    LIT("1x"),
    LIT("1e999"),
    LIT("sqlite_master"),
    LIT("us\xC3\xA9rs\xC2\xA0 \xE2\x80\xAE"),
    LIT("\xFF\xFE\xFD invalid utf8"),
    LIT("ab\0cd'; DROP TABLE users;--"),
    LIT("\0"),
    LIT("\0\0\0"),
    LIT("'\0'; DROP TABLE secret;--"),
    LIT("\x01\x02\x7f"),
};
#define N_VALUES (sizeof(kValues) / sizeof(kValues[0]))

/* ------------------------------------------------- hostile identifier tests */

/**
 * Feeds one invalid name to every function and every identifier position.
 */
static void hostile_ident(const char* p) {
  const PerkColumn good[] = {{"id", "INTEGER", PERK_COL_PK},
                             {"name", "TEXT", 0}};
  const PerkColumn bad_first[] = {{p, "TEXT", 0}, {"b", "TEXT", 0}};
  const PerkColumn bad_last[] = {{"a", "TEXT", 0}, {p, "TEXT", 0}};
  const PerkColumn bad_pk[] = {{"a", "INTEGER", PERK_COL_PK},
                               {p, "INTEGER", PERK_COL_PK | PERK_COL_UNIQUE}};
  const char* one[] = {p};
  const char* first[] = {p, "age"};
  const char* last[] = {"name", p};
  const char* name_only[] = {"name"};
  PerkValue v1[] = {perk_text("v")};
  PerkValue v2[] = {perk_text("v"), perk_int(1)};
  PerkValue id1 = perk_int(1);
  PerkResult* res = NULL;
  char buf[2048];

  rejected("create_table table", p, perk_create_table(g_db, p, good, 2), M_BAD);
  // An empty WHERE is documented as "match every row", so only direct-WHERE
  // checks of non-empty payloads are meaningful
  int direct_where = p[0] != '\0';
  rejected("create_table first col", p,
           perk_create_table(g_db, "newt", bad_first, 2), M_BAD);
  rejected("create_table last col", p,
           perk_create_table(g_db, "newt", bad_last, 2), M_BAD);
  rejected("create_table pk col", p, perk_create_table(g_db, "newt", bad_pk, 2),
           M_BAD);

  rejected("insert table", p, perk_insert(g_db, p, name_only, v1, 1, NULL),
           M_BAD);
  rejected("insert first col", p,
           perk_insert(g_db, "users", first, v2, 2, NULL), M_BAD);
  rejected("insert last col", p, perk_insert(g_db, "users", last, v2, 2, NULL),
           M_BAD);

  res = NULL;
  rejected_select("select table", p,
                  perk_select(g_db, p, NULL, 0, NULL, NULL, 0, &res), res,
                  M_BAD);
  res = NULL;
  rejected_select("select col", p,
                  perk_select(g_db, "users", one, 1, NULL, NULL, 0, &res), res,
                  M_BAD);
  res = NULL;
  rejected_select("select last col", p,
                  perk_select(g_db, "users", last, 2, NULL, NULL, 0, &res), res,
                  M_BAD);
  res = NULL;
  if (direct_where)
    rejected_select("select where word", p,
                    perk_select(g_db, "users", NULL, 0, p, NULL, 0, &res), res,
                    M_WHERE);
  snprintf(buf, sizeof(buf), "%s = ?", p);
  res = NULL;
  rejected_select("select where word = ?", p,
                  perk_select(g_db, "users", NULL, 0, buf, &id1, 1, &res), res,
                  M_WHERE);

  res = NULL;
  rejected_select("select_by_id table", p,
                  perk_select_by_id(g_db, p, NULL, 0, "id", &id1, &res), res,
                  M_BAD);
  res = NULL;
  rejected_select("select_by_id col", p,
                  perk_select_by_id(g_db, "users", one, 1, "id", &id1, &res),
                  res, M_BAD);
  res = NULL;
  rejected_select("select_by_id id_col", p,
                  perk_select_by_id(g_db, "users", NULL, 0, p, &id1, &res), res,
                  M_BAD);

  rejected("update table", p,
           perk_update(g_db, p, name_only, v1, 1, NULL, NULL, 0, NULL), M_BAD);
  rejected("update set col", p,
           perk_update(g_db, "users", one, v1, 1, NULL, NULL, 0, NULL), M_BAD);
  rejected("update set last col", p,
           perk_update(g_db, "users", last, v2, 2, NULL, NULL, 0, NULL), M_BAD);
  if (direct_where)
    rejected("update where", p,
             perk_update(g_db, "users", name_only, v1, 1, p, NULL, 0, NULL),
             M_WHERE);
  rejected("update where word = ?", p,
           perk_update(g_db, "users", name_only, v1, 1, buf, &id1, 1, NULL),
           M_WHERE);

  rejected("delete table", p, perk_delete(g_db, p, NULL, NULL, 0, NULL), M_BAD);
  if (direct_where)
    rejected("delete where", p, perk_delete(g_db, "users", p, NULL, 0, NULL),
             M_WHERE);
  rejected("delete where word = ?", p,
           perk_delete(g_db, "users", buf, &id1, 1, NULL), M_WHERE);
}

static void test_bad_identifiers(void) {
  fixture_up();
  for (size_t i = 0; i < N_BAD; i++) hostile_ident(kBadIdents[i]);
  hostile_ident(g_long65);
  hostile_ident(g_long1000);
  fixture_down();
}

/**
 * Names that bypass checks only by truncation: an embedded NUL ends the C
 * string, so the name is simply "users" and nothing after it is ever seen.
 */
static void test_embedded_nul_in_names(void) {
  fixture_up();
  char* base0 = snapshot(g_db, 0);
  const char* tbl = "users\0; DROP TABLE users;--";
  const char* cols[] = {"name\0\"; DROP TABLE users;--"};
  PerkValue v[] = {perk_text("nul-name")};
  int64_t rowid = 0;
  ASSERT_EQ_INT(perk_insert(g_db, tbl, cols, v, 1, &rowid), PERK_OK);
  PerkResult* res = NULL;
  ASSERT_EQ_INT(perk_select(g_db, tbl, cols, 1, NULL, NULL, 0, &res), PERK_OK);
  if (res) {
    ASSERT_EQ_INT(perk_result_rows(res), 4);
    perk_result_free(res);
  }
  PerkValue idv = perk_int(rowid);
  ASSERT_EQ_INT(perk_delete(g_db, tbl, "id = ?", &idv, 1, NULL), PERK_OK);
  // total_changes legitimately moved; compare everything except users, and
  // confirm users is back to its 3 seeded rows
  char* now = snapshot(g_db, 0);
  ASSERT_EQ_STR(now, base0);
  free(now);
  free(base0);
  PerkResult* all = NULL;
  ASSERT_EQ_INT(perk_select(g_db, "users", NULL, 0, NULL, NULL, 0, &all),
                PERK_OK);
  if (all) {
    ASSERT_EQ_INT(perk_result_rows(all), 3);
    perk_result_free(all);
  }
  fixture_down();
}

/* ------------------------------------------------------ hostile WHERE tests */

static void hostile_where(const WhereCase* w) {
  PerkValue args[4] = {perk_int(1), perk_int(1), perk_int(1), perk_int(1)};
  const char* name_only[] = {"name"};
  PerkValue nv[] = {perk_text("pwned")};
  PerkResult* res = NULL;
  const PerkValue* a = w->nargs ? args : NULL;
  size_t na = w->nargs > 4 ? 4 : w->nargs;

  rejected_select("select where", w->where,
                  perk_select(g_db, "users", NULL, 0, w->where, a, na, &res),
                  res, M_WHERE);
  rejected("update where", w->where,
           perk_update(g_db, "users", name_only, nv, 1, w->where, a, na, NULL),
           M_WHERE);
  rejected("delete where", w->where,
           perk_delete(g_db, "users", w->where, a, na, NULL), M_WHERE);
  // Same text against another table must not leak that table's rows either
  res = NULL;
  rejected_select("select secret where", w->where,
                  perk_select(g_db, "secret", NULL, 0, w->where, a, na, &res),
                  res, M_WHERE);
}

static void test_bad_wheres(void) {
  fixture_up();
  for (size_t i = 0; i < N_BADW; i++) {
    // The few entries that are legitimately valid would defeat the test, so
    // every entry in the table must be rejected.
    hostile_where(&kBadWheres[i]);
  }
  fixture_down();
}

/**
 * Sanity control: the tokenizer is not simply rejecting everything.
 */
static void expect_rows(const char* where, const PerkValue* args, size_t nargs,
                        size_t want) {
  PerkResult* res = NULL;
  PerkErr e = perk_select(g_db, "users", NULL, 0, where, args, nargs, &res);
  if (!ASSERT_EQ_INT(e, PERK_OK))
    printf("  where \"%s\": %s\n", where, perk_errmsg(g_db));
  if (res) {
    if (!ASSERT_EQ_INT(perk_result_rows(res), want))
      printf("  where \"%s\"\n", where);
    ASSERT_TRUE(!result_has(res, kToken));
    perk_result_free(res);
  }
}

static void test_valid_wheres(void) {
  fixture_up();
  PerkValue i1[] = {perk_int(1), perk_int(3)};
  PerkValue age[] = {perk_int(30), perk_text("alice")};
  PerkValue pat[] = {perk_text("a%")};
  PerkValue npat[] = {perk_text("a%")};
  PerkValue gl[] = {perk_text("b*")};
  PerkValue btw[] = {perk_int(24), perk_int(26)};
  PerkValue one[] = {perk_int(2)};
  PerkValue hostile_arg[] = {perk_text("1 OR 1=1")};
  expect_rows(NULL, NULL, 0, 3);
  expect_rows("", NULL, 0, 3);
  expect_rows("id = ?", one, 1, 1);
  expect_rows("id == ?", one, 1, 1);
  expect_rows("id != ?", one, 1, 2);
  expect_rows("id <> ?", one, 1, 2);
  expect_rows("id IN (?, ?)", i1, 2, 2);
  expect_rows("id NOT IN (?,?)", i1, 2, 1);
  expect_rows("id in (?,?)", i1, 2, 2);
  expect_rows("age >= ? AND (name = ? OR name IS NULL)", age, 2, 1);
  expect_rows("name LIKE ?", pat, 1, 1);
  expect_rows("name NOT LIKE ?", npat, 1, 2);
  expect_rows("name GLOB ?", gl, 1, 1);
  expect_rows("age BETWEEN ? AND ?", btw, 2, 1);
  expect_rows("data IS NULL", NULL, 0, 2);
  expect_rows("data IS NOT NULL", NULL, 0, 1);
  expect_rows("NOT id = ?", one, 1, 2);
  expect_rows("\tid\n=\r?  ", one, 1, 1);
  // A hostile string as a bound arg is just a value that matches nothing
  expect_rows("id = ?", hostile_arg, 1, 0);
  expect_rows("name = ?", hostile_arg, 1, 0);
  check_unchanged("valid wheres", "");
  fixture_down();
}

/* ---------------------------------------------------------- value payloads */

/**
 * Checks cell (row,col) of res is a text value equal to v.
 */
static void expect_text(const PerkResult* res, size_t row, size_t col,
                        const Bytes* v, const char* what) {
  const PerkValue* c = perk_result_get(res, row, col);
  if (!ASSERT_TRUE(c != NULL)) return;
  if (!ASSERT_EQ_INT(c->type, PERK_TEXT)) {
    printf("  %s\n", what);
    return;
  }
  if (!ASSERT_EQ_INT(c->as.text.len, v->n)) printf("  %s\n", what);
  if (c->as.text.len == v->n)
    if (!ASSERT_TRUE(memcmp(c->as.text.ptr, v->s, v->n) == 0))
      printf("  %s: bytes differ\n", what);
  // text is NUL terminated per header
  ASSERT_TRUE(c->as.text.ptr[c->as.text.len] == '\0');
}

static void expect_blob(const PerkResult* res, size_t row, size_t col,
                        const Bytes* v, const char* what) {
  const PerkValue* c = perk_result_get(res, row, col);
  if (!ASSERT_TRUE(c != NULL)) return;
  if (!ASSERT_EQ_INT(c->type, PERK_BLOB)) {
    printf("  %s\n", what);
    return;
  }
  if (!ASSERT_EQ_INT(c->as.blob.len, v->n)) printf("  %s\n", what);
  if (c->as.blob.len == v->n && v->n > 0)
    if (!ASSERT_TRUE(memcmp(c->as.blob.ptr, v->s, v->n) == 0))
      printf("  %s: bytes differ\n", what);
}

static PerkValue text_of(const Bytes* b) {
  PerkValue pv = {.type = PERK_TEXT, .as.text = {b->s, b->n}};
  return pv;
}

/**
 * Stores one payload via insert/update, reads it back byte-exact, matches it
 * with bound WHERE args, and verifies nothing else in the db was disturbed.
 */
static void value_roundtrip(const Bytes* v) {
  fixture_up();
  char* other_base = snapshot(g_db, 0);
  PerkValue tv = text_of(v);
  PerkValue bv = perk_blob(v->s, v->n);
  PerkResult* res = NULL;

  // 1. Read-only uses of the payload as a bound arg: matches nothing, no leak
  const char* wheres[] = {"name = ?",       "name LIKE ?",
                          "name GLOB ?",    "name <> ?",
                          "name IN (?, ?)", "name BETWEEN ? AND ?"};
  const size_t wn[] = {1, 1, 1, 1, 2, 2};
  for (size_t i = 0; i < 6; i++) {
    PerkValue args[2] = {tv, tv};
    res = NULL;
    PerkErr e =
        perk_select(g_db, "users", NULL, 0, wheres[i], args, wn[i], &res);
    if (!ASSERT_EQ_INT(e, PERK_OK))
      printf("  where %s payload \"%s\": %s\n", wheres[i], v->s,
             perk_errmsg(g_db));
    if (res) {
      ASSERT_TRUE(!result_has(res, kToken));
      ASSERT_TRUE(!result_has(res, kFlag));
      perk_result_free(res);
    }
  }
  res = NULL;
  PerkErr e = perk_select_by_id(g_db, "users", NULL, 0, "id", &tv, &res);
  if (!ASSERT_EQ_INT(e, PERK_ERR_NOT_FOUND)) printf("  payload \"%s\"\n", v->s);
  ASSERT_TRUE(res == NULL);
  e = perk_select_by_id(g_db, "users", NULL, 0, "name", &tv, &res);
  if (!ASSERT_EQ_INT(e, PERK_ERR_NOT_FOUND)) printf("  payload \"%s\"\n", v->s);
  {
    int64_t ch = 99;
    ASSERT_EQ_INT(perk_delete(g_db, "users", "name = ?", &tv, 1, &ch),
                  PERK_ERR_NOT_FOUND);
    ASSERT_EQ_INT(ch, 0);
    const char* sc[] = {"age"};
    PerkValue sv[] = {perk_int(1)};
    ch = 99;
    ASSERT_EQ_INT(
        perk_update(g_db, "users", sc, sv, 1, "name = ?", &tv, 1, &ch),
        PERK_ERR_NOT_FOUND);
    ASSERT_EQ_INT(ch, 0);
  }
  check_unchanged("value as bound arg", v->s);

  // 2. insert: text into name, blob into data, payload also as a new row in x
  const char* cols[] = {"name", "age", "data"};
  PerkValue vals[] = {tv, perk_int(7), bv};
  int64_t rowid = 0;
  e = perk_insert(g_db, "users", cols, vals, 3, &rowid);
  if (!ASSERT_EQ_INT(e, PERK_OK)) {
    printf("  insert payload \"%s\": %s\n", v->s, perk_errmsg(g_db));
    free(other_base);
    fixture_down();
    return;
  }
  ASSERT_EQ_INT(rowid, 4);

  // 3. read back by id, byte-exact
  PerkValue idv = perk_int(rowid);
  res = NULL;
  e = perk_select_by_id(g_db, "users", NULL, 0, "id", &idv, &res);
  ASSERT_EQ_INT(e, PERK_OK);
  if (res) {
    ASSERT_EQ_INT(perk_result_rows(res), 1);
    expect_text(res, 0, 1, v, "insert text readback");
    expect_blob(res, 0, 3, v, "insert blob readback");
    const PerkValue* age = perk_result_get(res, 0, 2);
    ASSERT_TRUE(age && age->type == PERK_INT && age->as.i == 7);
    perk_result_free(res);
  }

  // 4. use as a bound WHERE arg: matches exactly that row
  PerkValue wargs[] = {tv, bv};
  res = NULL;
  e = perk_select(g_db, "users", NULL, 0, "name = ? AND data = ?", wargs, 2,
                  &res);
  ASSERT_EQ_INT(e, PERK_OK);
  if (res) {
    if (ASSERT_EQ_INT(perk_result_rows(res), 1)) {
      const PerkValue* id = perk_result_get(res, 0, 0);
      ASSERT_TRUE(id && id->type == PERK_INT && id->as.i == rowid);
    }
    ASSERT_TRUE(!result_has(res, kToken));
    perk_result_free(res);
  }
  res = NULL;
  e = perk_select(g_db, "users", NULL, 0, "name = ?", &tv, 1, &res);
  ASSERT_EQ_INT(e, PERK_OK);
  if (res) {
    ASSERT_EQ_INT(perk_result_rows(res), 1);
    perk_result_free(res);
  }
  res = NULL;
  e = perk_select_by_id(g_db, "users", NULL, 0, "name", &tv, &res);
  ASSERT_EQ_INT(e, PERK_OK);
  if (res) {
    ASSERT_EQ_INT(perk_result_rows(res), 1);
    perk_result_free(res);
  }

  // 5. update row 2 (bob) to hold the payload; both rows now match
  const char* set[] = {"name", "age"};
  PerkValue setv[] = {tv, perk_int(99)};
  PerkValue bob = perk_text("bob");
  int64_t changed = 0;
  e = perk_update(g_db, "users", set, setv, 2, "name = ?", &bob, 1, &changed);
  ASSERT_EQ_INT(e, PERK_OK);
  ASSERT_EQ_INT(changed, 1);
  PerkValue two = perk_int(2);
  res = NULL;
  e = perk_select_by_id(g_db, "users", NULL, 0, "id", &two, &res);
  ASSERT_EQ_INT(e, PERK_OK);
  if (res) {
    expect_text(res, 0, 1, v, "update text readback");
    perk_result_free(res);
  }
  // update via payload as WHERE arg touches exactly the two payload rows
  const char* agecol[] = {"age"};
  PerkValue five[] = {perk_int(5)};
  changed = 0;
  e = perk_update(g_db, "users", agecol, five, 1, "name = ?", &tv, 1, &changed);
  ASSERT_EQ_INT(e, PERK_OK);
  ASSERT_EQ_INT(changed, 2);
  res = NULL;
  e = perk_select(g_db, "users", NULL, 0, "age = ?", five, 1, &res);
  ASSERT_EQ_INT(e, PERK_OK);
  if (res) {
    ASSERT_EQ_INT(perk_result_rows(res), 2);
    perk_result_free(res);
  }

  // 6. payload as the update value on a different table (x.v)
  const char* vcol[] = {"v"};
  PerkValue xv[] = {tv};
  e = perk_update(g_db, "x", vcol, xv, 1, NULL, NULL, 0, &changed);
  ASSERT_EQ_INT(e, PERK_OK);
  ASSERT_EQ_INT(changed, 1);
  PerkValue one = perk_int(1);
  res = NULL;
  e = perk_select_by_id(g_db, "x", NULL, 0, "id", &one, &res);
  ASSERT_EQ_INT(e, PERK_OK);
  if (res) {
    expect_text(res, 0, 1, v, "x readback");
    perk_result_free(res);
  }

  // 7. delete by payload removes exactly the 2 payload rows
  changed = 0;
  e = perk_delete(g_db, "users", "name = ?", &tv, 1, &changed);
  ASSERT_EQ_INT(e, PERK_OK);
  ASSERT_EQ_INT(changed, 2);
  ASSERT_EQ_INT(perk_delete(g_db, "users", "name = ?", &tv, 1, &changed),
                PERK_ERR_NOT_FOUND);
  res = NULL;
  e = perk_select(g_db, "users", NULL, 0, NULL, NULL, 0, &res);
  ASSERT_EQ_INT(e, PERK_OK);
  if (res) {
    ASSERT_EQ_INT(perk_result_rows(res), 2);  // alice, carol
    perk_result_free(res);
  }

  // 8. nothing outside users/x changed: schema, secret, sentinel
  // (x was deliberately modified in step 6, so restore it first)
  PerkValue xorig[] = {perk_text("xval")};
  ASSERT_EQ_INT(perk_update(g_db, "x", vcol, xorig, 1, NULL, NULL, 0, NULL),
                PERK_OK);
  char* now = snapshot(g_db, 0);
  ASSERT_EQ_STR(now, other_base);
  free(now);
  free(other_base);
  fixture_down();
}

static void test_value_payloads(void) {
  for (size_t i = 0; i < N_VALUES; i++) value_roundtrip(&kValues[i]);
}

/**
 * A very long hostile value, and values that look like the secret token query.
 */
static void test_long_value(void) {
  fixture_up();
  size_t n = 200000;
  char* big = malloc(n + 1);
  for (size_t i = 0; i < n; i++) big[i] = "'\";-"[i % 4];
  big[n] = '\0';
  const char* cols[] = {"name"};
  PerkValue v[] = {perk_text(big)};
  int64_t rowid = 0;
  ASSERT_EQ_INT(perk_insert(g_db, "users", cols, v, 1, &rowid), PERK_OK);
  PerkResult* res = NULL;
  ASSERT_EQ_INT(perk_select(g_db, "users", NULL, 0, "name = ?", v, 1, &res),
                PERK_OK);
  if (res) {
    ASSERT_EQ_INT(perk_result_rows(res), 1);
    const PerkValue* c = perk_result_get(res, 0, 1);
    ASSERT_TRUE(c && c->as.text.len == n &&
                memcmp(c->as.text.ptr, big, n) == 0);
    perk_result_free(res);
  }
  char* now = snapshot(g_db, 0);
  char* other = snapshot(g_db, 0);
  ASSERT_EQ_STR(now, other);
  free(now);
  free(other);
  free(big);
  fixture_down();
}

/**
 * Wrong arg counts are refused even when the args are hostile.
 */
static void test_arg_count_mismatch(void) {
  fixture_up();
  PerkValue hv[] = {perk_text("' OR '1'='1"), perk_text("x")};
  const char* nm[] = {"name"};
  PerkValue sv[] = {perk_text("zz")};
  PerkResult* res = NULL;
  rejected_select("select too many args", "",
                  perk_select(g_db, "users", NULL, 0, "name = ?", hv, 2, &res),
                  res, MASK(PERK_ERR_MISUSE));
  res = NULL;
  rejected_select(
      "select too few args", "",
      perk_select(g_db, "users", NULL, 0, "name = ? OR name = ?", hv, 1, &res),
      res, MASK(PERK_ERR_MISUSE));
  res = NULL;
  rejected_select("select args w/o where", "",
                  perk_select(g_db, "users", NULL, 0, NULL, hv, 1, &res), res,
                  MASK(PERK_ERR_MISUSE));
  rejected("update too many args", "",
           perk_update(g_db, "users", nm, sv, 1, "name = ?", hv, 2, NULL),
           MASK(PERK_ERR_MISUSE));
  rejected("delete too few args", "",
           perk_delete(g_db, "users", "name = ? AND age = ?", hv, 1, NULL),
           MASK(PERK_ERR_MISUSE));
  res = NULL;
  rejected_select(
      "select args NULL, nargs>0", "",
      perk_select(g_db, "users", NULL, 0, "name = ?", NULL, 1, &res), res,
      MASK(PERK_ERR_MISUSE));
  fixture_down();
}

/* ----------------------------------------------- create_table hostile types */

static void test_create_table_types(void) {
  fixture_up();
  static const char* const kTypes[] = {
      "TEXT); DROP TABLE users;--",
      "INTEGER PRIMARY KEY",
      "TEXT DEFAULT (1)",
      "TEXT; --",
      "TEXT, evil TEXT",
      "int ",
      " INT",
      "VARCHAR(10)",
      "",
      "'TEXT'",
      "\"TEXT\"",
      "TEXT\n",
      "INT\xC2\xA0",
      "TEXT REFERENCES secret(id)",
      "TEXT CHECK (1)",
      "TEXT AS (1)",
      "TEXT COLLATE NOCASE",
      "INTEGER)--",
  };
  for (size_t i = 0; i < sizeof(kTypes) / sizeof(kTypes[0]); i++) {
    PerkColumn cols[] = {{"a", kTypes[i], 0}};
    rejected("create_table type", kTypes[i],
             perk_create_table(g_db, "newt", cols, 1), M_BAD);
    PerkColumn cols2[] = {{"id", "INTEGER", PERK_COL_PK}, {"b", kTypes[i], 0}};
    rejected("create_table type (2nd col)", kTypes[i],
             perk_create_table(g_db, "newt", cols2, 2), M_BAD);
  }
  // unknown flag bits are refused, nothing created
  PerkColumn badflag[] = {{"a", "TEXT", 1 << 5}};
  rejected("create_table flags", "flags",
           perk_create_table(g_db, "newt", badflag, 1), MASK(PERK_ERR_MISUSE));
  PerkColumn negflag[] = {{"a", "TEXT", -1}};
  rejected("create_table neg flags", "flags",
           perk_create_table(g_db, "newt", negflag, 1), MASK(PERK_ERR_MISUSE));
  // existing names, any case, are EXISTS and leave the schema alone
  PerkColumn ok[] = {{"id", "INTEGER", PERK_COL_PK}};
  rejected("create_table dup", "users", perk_create_table(g_db, "users", ok, 1),
           MASK(PERK_ERR_EXISTS));
  rejected("create_table dup case", "USERS",
           perk_create_table(g_db, "USERS", ok, 1), MASK(PERK_ERR_EXISTS));
  rejected("create_table dup 2", "x", perk_create_table(g_db, "x", ok, 1),
           MASK(PERK_ERR_EXISTS));
  // duplicate column names are an SQL error, not a bypass
  PerkColumn dupcol[] = {{"a", "TEXT", 0}, {"A", "TEXT", 0}};
  rejected("create_table dup col", "a/A",
           perk_create_table(g_db, "newt", dupcol, 2), MASK(PERK_ERR_SQL));
  // lowercase / mixed-case whitelisted types are fine
  PerkColumn okc[] = {{"id", "integer", PERK_COL_PK},
                      {"t", "Text", 0},
                      {"n", NULL, 0},
                      {"b", "blob", PERK_COL_NOTNULL | PERK_COL_UNIQUE}};
  ASSERT_EQ_INT(perk_create_table(g_db, "ci_types", okc, 4), PERK_OK);
  fixture_down();
}

/* ------------------------------------------------- keywords and 64-char ids */

static void make_name(char* buf, size_t len, char fill) {
  memset(buf, fill, len);
  buf[len] = '\0';
}

static void test_boundary_names_via_api(void) {
  fixture_up();
  char n64[65], n63[64], c64[65];
  make_name(n64, 64, 'T');
  make_name(n63, 63, '_');
  make_name(c64, 64, 'c');
  PerkColumn cols[] = {{"id", "INTEGER", PERK_COL_PK}, {c64, "TEXT", 0}};
  ASSERT_EQ_INT(perk_create_table(g_db, n64, cols, 2), PERK_OK);
  ASSERT_EQ_INT(perk_create_table(g_db, n63, cols, 2), PERK_OK);

  const char* ic[] = {c64};
  PerkValue v[] = {perk_text("'; DROP TABLE users;--")};
  int64_t rowid = 0;
  ASSERT_EQ_INT(perk_insert(g_db, n64, ic, v, 1, &rowid), PERK_OK);
  char where[128];
  snprintf(where, sizeof(where), "%s = ?", c64);
  PerkResult* res = NULL;
  ASSERT_EQ_INT(perk_select(g_db, n64, ic, 1, where, v, 1, &res), PERK_OK);
  if (res) {
    ASSERT_EQ_INT(perk_result_rows(res), 1);
    ASSERT_EQ_STR(perk_result_colname(res, 0), c64);
    perk_result_free(res);
  }
  PerkValue idv = perk_int(rowid);
  res = NULL;
  ASSERT_EQ_INT(perk_select_by_id(g_db, n64, NULL, 0, "id", &idv, &res),
                PERK_OK);
  perk_result_free(res);
  int64_t ch = 0;
  PerkValue nv[] = {perk_text("n")};
  ASSERT_EQ_INT(perk_update(g_db, n64, ic, nv, 1, where, v, 1, &ch), PERK_OK);
  ASSERT_EQ_INT(ch, 1);
  PerkValue nv0 = perk_text("n");
  ASSERT_EQ_INT(perk_delete(g_db, n64, where, &nv0, 1, &ch), PERK_OK);

  // 65 characters is one too many for every position, including WHERE words
  char n65[66], where65[128];
  make_name(n65, 65, 'c');
  snprintf(where65, sizeof(where65), "%s = ?", n65);
  PerkColumn c65[] = {{n65, "TEXT", 0}};
  const char* i65[] = {n65};
  char* before = snapshot(g_db, 1);
  free(g_base);
  g_base = before;
  rejected("create_table 65 table", n65, perk_create_table(g_db, n65, cols, 2),
           M_BAD);
  rejected("create_table 65 col", n65, perk_create_table(g_db, "t65", c65, 1),
           M_BAD);
  rejected("insert 65 col", n65, perk_insert(g_db, n64, i65, v, 1, NULL),
           M_BAD);
  res = NULL;
  rejected_select("select 65 where", n65,
                  perk_select(g_db, n64, NULL, 0, where65, v, 1, &res), res,
                  M_BAD);
  // 64-char names starting with a digit or containing a dash are refused
  char dig[65];
  make_name(dig, 64, 'a');
  dig[0] = '9';
  rejected("create_table digit-first 64", dig,
           perk_create_table(g_db, dig, cols, 2), M_BAD);
  make_name(dig, 64, 'a');
  dig[63] = '-';
  rejected("create_table dash-last 64", dig,
           perk_create_table(g_db, dig, cols, 2), M_BAD);
  fixture_down();
}

static void test_keyword_identifiers(void) {
  fixture_up();
  static const char* const kKw[] = {"select", "drop",  "table", "from",
                                    "where",  "order", "group", "index",
                                    "values", "key",   "update"};
  PerkColumn cols[] = {{"from", "INTEGER", PERK_COL_PK},
                       {"where", "TEXT", 0},
                       {"order", "TEXT", 0},
                       {"null", "TEXT", 0}};
  for (size_t i = 0; i < sizeof(kKw) / sizeof(kKw[0]); i++) {
    PerkErr e = perk_create_table(g_db, kKw[i], cols, 4);
    if (!ASSERT_EQ_INT(e, PERK_OK)) {
      printf("  table %s: %s\n", kKw[i], perk_errmsg(g_db));
      continue;
    }
    const char* ic[] = {"where", "order", "null"};
    PerkValue v[] = {perk_text("1; DROP TABLE users"), perk_text("b"),
                     perk_text("c")};
    int64_t rowid = 0;
    ASSERT_EQ_INT(perk_insert(g_db, kKw[i], ic, v, 3, &rowid), PERK_OK);
    PerkValue id = perk_int(rowid);
    PerkResult* res = NULL;
    ASSERT_EQ_INT(perk_select_by_id(g_db, kKw[i], ic, 3, "from", &id, &res),
                  PERK_OK);
    perk_result_free(res);
    res = NULL;
    ASSERT_EQ_INT(perk_select(g_db, kKw[i], NULL, 0, "where = ? AND order = ?",
                              v, 2, &res),
                  PERK_OK);
    if (res) {
      ASSERT_EQ_INT(perk_result_rows(res), 1);
      perk_result_free(res);
    }
    int64_t ch = 0;
    PerkValue nv[] = {perk_text("zz")};
    const char* sc[] = {"order"};
    ASSERT_EQ_INT(perk_update(g_db, kKw[i], sc, nv, 1, "from = ?", &id, 1, &ch),
                  PERK_OK);
    ASSERT_EQ_INT(perk_delete(g_db, kKw[i], "from = ?", &id, 1, &ch), PERK_OK);
  }
  // users untouched by all of the above
  char* now = snapshot(g_db, 1);
  ASSERT_TRUE(strstr(now, "users") != NULL);
  PerkResult* res = NULL;
  ASSERT_EQ_INT(perk_select(g_db, "users", NULL, 0, NULL, NULL, 0, &res),
                PERK_OK);
  if (res) {
    ASSERT_EQ_INT(perk_result_rows(res), 3);
    perk_result_free(res);
  }
  free(now);
  fixture_down();
}

/* ------------------------------------------------ identifier regex conformance
 */

/**
 * Independent model of ^[A-Za-z_][A-Za-z0-9_]{0,63}$ plus the sqlite_ prefix
 * rule.
 */
static int model_valid(const unsigned char* s, size_t n) {
  if (n < 1 || n > 64) return 0;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = s[i];
    int alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
    int digit = c >= '0' && c <= '9';
    if (i == 0 ? !alpha : !(alpha || digit)) return 0;
  }
  if (n >= 7) {
    static const char pre[] = "sqlite_";
    int same = 1;
    for (size_t i = 0; i < 7; i++) {
      unsigned char c = s[i];
      if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
      if (c != (unsigned char)pre[i]) same = 0;
    }
    if (same) return 0;
  }
  return 1;
}

static void check_name_direct(const char* name) {
  int want = model_valid((const unsigned char*)name, strlen(name));
  int got = validate_identifier(name) != 0;
  if (!ASSERT_EQ_INT(got, want)) {
    printf("  name bytes:");
    for (const unsigned char* p = (const unsigned char*)name;
         *p && p < (const unsigned char*)name + 80; p++)
      printf(" %02x", *p);
    printf(" (len %zu)\n", strlen(name));
  }
}

static void test_regex_direct(void) {
  ASSERT_EQ_INT(validate_identifier(NULL), 0);
  ASSERT_EQ_INT(validate_identifier(""), 0);

  unsigned char buf[1100];
  for (int b = 1; b < 256; b++) {
    // as the first byte (alone, and followed by valid chars)
    buf[0] = (unsigned char)b;
    buf[1] = 0;
    check_name_direct((char*)buf);
    buf[1] = 'a';
    buf[2] = '1';
    buf[3] = 0;
    check_name_direct((char*)buf);
    // as a later byte (middle and last)
    buf[0] = 'a';
    buf[1] = (unsigned char)b;
    buf[2] = 'z';
    buf[3] = 0;
    check_name_direct((char*)buf);
    buf[0] = 'a';
    buf[1] = (unsigned char)b;
    buf[2] = 0;
    check_name_direct((char*)buf);
    // as the last byte of a 64-byte name, and of a 63-byte name
    memset(buf, 'a', 64);
    buf[63] = (unsigned char)b;
    buf[64] = 0;
    check_name_direct((char*)buf);
    memset(buf, 'a', 63);
    buf[62] = (unsigned char)b;
    buf[63] = 0;
    check_name_direct((char*)buf);
    // as the byte right after a 64-char name (65 chars: always invalid)
    memset(buf, 'a', 64);
    buf[64] = (unsigned char)b;
    buf[65] = 0;
    check_name_direct((char*)buf);
  }

  // lengths
  static const size_t lens[] = {1, 2, 7, 8, 62, 63, 64, 65, 66, 100, 1000};
  for (size_t i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
    size_t n = lens[i];
    memset(buf, 'a', n);
    buf[n] = 0;
    check_name_direct((char*)buf);
    ASSERT_EQ_INT(validate_identifier((char*)buf), n <= 64);
    memset(buf, '_', n);
    buf[n] = 0;
    check_name_direct((char*)buf);
    ASSERT_EQ_INT(validate_identifier((char*)buf), n <= 64);
    memset(buf, '9', n);
    buf[0] = 'Z';
    buf[n] = 0;
    check_name_direct((char*)buf);
    ASSERT_EQ_INT(validate_identifier((char*)buf), n <= 64);
    memset(buf, '9', n);
    buf[n] = 0;  // digit first
    check_name_direct((char*)buf);
    ASSERT_EQ_INT(validate_identifier((char*)buf), 0);
    memset(buf, 'a', n);
    buf[n - 1] = '-';
    buf[n] = 0;
    check_name_direct((char*)buf);
    if (n > 1) ASSERT_EQ_INT(validate_identifier((char*)buf), 0);
  }

  // sqlite_ prefix, any case, only as a prefix
  static const struct {
    const char* s;
    int ok;
  } pre[] = {
      {"sqlite_", 0},   {"sqlite_a", 0}, {"SQLITE_a", 0}, {"SqLiTe_x", 0},
      {"sqlite", 1},    {"sqlite1", 1},  {"sqlit_e", 1},  {"asqlite_x", 1},
      {"_sqlite_x", 1}, {"sqlite__", 0}, {"sqliteX_", 1},
  };
  for (size_t i = 0; i < sizeof(pre) / sizeof(pre[0]); i++) {
    ASSERT_EQ_INT(validate_identifier(pre[i].s), pre[i].ok);
    check_name_direct(pre[i].s);
  }
}

/**
 * Every byte through the public API: valid names reach SQLite (and fail there
 * with "no such table/column", PERK_ERR_SQL), invalid ones are BADNAME.
 */
static void test_regex_via_api(void) {
  fixture_up();
  PerkValue id1 = perk_int(1);
  PerkValue v1[] = {perk_text("v")};
  for (int b = 1; b < 256; b++) {
    char t2[3] = {'q', (char)b, 0};       // table "q?"
    char t3[4] = {'q', (char)b, 'q', 0};  // table "q?q"
    char t1[2] = {(char)b, 0};
    int valid_t2 = model_valid((unsigned char*)t2, 2);
    int valid_t3 = model_valid((unsigned char*)t3, 3);
    // single-byte names could hit the real table "x", so use them as columns
    int valid_t1 = model_valid((unsigned char*)t1, 1);
    // a single-byte column name never exists in users
    const char* names[] = {t2, t3};
    int valids[] = {valid_t2, valid_t3};
    for (int k = 0; k < 2; k++) {
      PerkResult* res = NULL;
      unsigned want = valids[k] ? MASK(PERK_ERR_SQL) : M_BAD;
      rejected_select("select table", names[k],
                      perk_select(g_db, names[k], NULL, 0, NULL, NULL, 0, &res),
                      res, want);
      rejected("delete table", names[k],
               perk_delete(g_db, names[k], NULL, NULL, 0, NULL), want);
      rejected("insert table", names[k],
               perk_insert(g_db, names[k], (const char*[]){"v"}, v1, 1, NULL),
               want);
      rejected("update table", names[k],
               perk_update(g_db, names[k], (const char*[]){"v"}, v1, 1, NULL,
                           NULL, 0, NULL),
               want);
      const char* col[] = {names[k]};
      res = NULL;
      rejected_select("select col", names[k],
                      perk_select(g_db, "users", col, 1, NULL, NULL, 0, &res),
                      res, want);
      res = NULL;
      rejected_select(
          "select_by_id id_col", names[k],
          perk_select_by_id(g_db, "users", NULL, 0, names[k], &id1, &res), res,
          want);
      rejected("insert col", names[k],
               perk_insert(g_db, "users", col, v1, 1, NULL), want);
      rejected("update set col", names[k],
               perk_update(g_db, "users", col, v1, 1, NULL, NULL, 0, NULL),
               want);
    }
    {
      const char* col[] = {t1};
      PerkResult* res = NULL;
      rejected_select("select 1-byte col", t1,
                      perk_select(g_db, "users", col, 1, NULL, NULL, 0, &res),
                      res, valid_t1 ? MASK(PERK_ERR_SQL) : M_BAD);
    }
  }
  fixture_down();
}

/* --------------------------------------------------- runtime hardening */

static int exec_rc(sqlite3* c, const char* sql) {
  return sqlite3_exec(c, sql, NULL, NULL, NULL);
}

static void check_connection_hardening(PerkDB* db) {
  sqlite3* c = db->conn;
  int r = -1;
  ASSERT_EQ_INT(sqlite3_db_config(c, SQLITE_DBCONFIG_DEFENSIVE, -1, &r),
                SQLITE_OK);
  ASSERT_EQ_INT(r, 1);
  r = -1;
  ASSERT_EQ_INT(
      sqlite3_db_config(c, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, -1, &r),
      SQLITE_OK);
  ASSERT_EQ_INT(r, 0);
  r = -1;
  ASSERT_EQ_INT(sqlite3_db_config(c, SQLITE_DBCONFIG_TRUSTED_SCHEMA, -1, &r),
                SQLITE_OK);
  ASSERT_EQ_INT(r, 0);
  r = -1;
  ASSERT_EQ_INT(sqlite3_db_config(c, SQLITE_DBCONFIG_DQS_DML, -1, &r),
                SQLITE_OK);
  ASSERT_EQ_INT(r, 0);
  r = -1;
  ASSERT_EQ_INT(sqlite3_db_config(c, SQLITE_DBCONFIG_DQS_DDL, -1, &r),
                SQLITE_OK);
  ASSERT_EQ_INT(r, 0);
  ASSERT_EQ_INT(sqlite3_limit(c, SQLITE_LIMIT_ATTACHED, -1), 0);
}

static void test_runtime_hardening(void) {
  fixture_up();
  check_connection_hardening(g_db);
  sqlite3* c = g_db->conn;

  // ATTACH is impossible even with raw SQL on the handle
  ASSERT_TRUE(exec_rc(c, "ATTACH DATABASE ':memory:' AS z") != SQLITE_OK);
  ASSERT_TRUE(
      exec_rc(c, "ATTACH DATABASE 'build/test_hardening_att.db' AS z") !=
      SQLITE_OK);
  // extension loading is impossible
  ASSERT_TRUE(exec_rc(c, "SELECT load_extension('x')") != SQLITE_OK);
  ASSERT_TRUE(exec_rc(c, "SELECT load_extension('x', 'y')") != SQLITE_OK);
  // double-quoted strings are identifiers only (no silent string literal)
  ASSERT_TRUE(exec_rc(c, "SELECT \"no_such_column_literal\"") != SQLITE_OK);
  ASSERT_TRUE(exec_rc(c, "SELECT * FROM users WHERE name = \"alice\"") !=
              SQLITE_OK);
  // writable_schema cannot be used to rewrite sqlite_master
  exec_rc(c, "PRAGMA writable_schema=ON");
  ASSERT_TRUE(
      exec_rc(c, "UPDATE sqlite_master SET name='pwned' WHERE name='secret'") !=
      SQLITE_OK);
  ASSERT_TRUE(exec_rc(c, "DELETE FROM sqlite_master WHERE name='secret'") !=
              SQLITE_OK);
  check_unchanged("raw hardening probes", "");

  // The wrapper is still fully functional on a hardened handle
  PerkValue id1 = perk_int(1);
  PerkResult* res = NULL;
  ASSERT_EQ_INT(perk_select_by_id(g_db, "secret", NULL, 0, "id", &id1, &res),
                PERK_OK);
  perk_result_free(res);
  fixture_down();
}

static void rm_db(const char* path) {
  char p[256];
  remove(path);
  snprintf(p, sizeof(p), "%s-wal", path);
  remove(p);
  snprintf(p, sizeof(p), "%s-shm", path);
  remove(p);
  snprintf(p, sizeof(p), "%s-journal", path);
  remove(p);
}

static void test_file_db_wal_and_reopen(void) {
  const char* path = "build/test_hardening.db";
  rm_db(path);
  PerkDB* db = NULL;
  if (!ASSERT_EQ_INT(perk_open(path, &db), PERK_OK)) {
    printf("  %s\n", perk_errmsg(NULL));
    return;
  }
  check_connection_hardening(db);

  // WAL really is on, and the wrapper works under it
  sqlite3_stmt* st = NULL;
  ASSERT_EQ_INT(
      sqlite3_prepare_v2(db->conn, "PRAGMA journal_mode", -1, &st, NULL),
      SQLITE_OK);
  if (st) {
    ASSERT_EQ_INT(sqlite3_step(st), SQLITE_ROW);
    ASSERT_EQ_STR((const char*)sqlite3_column_text(st, 0), "wal");
    sqlite3_finalize(st);
  }
  PerkColumn cols[] = {{"id", "INTEGER", PERK_COL_PK}, {"v", "TEXT", 0}};
  ASSERT_EQ_INT(perk_create_table(db, "t", cols, 2), PERK_OK);
  const char* ic[] = {"v"};
  PerkValue pv[] = {perk_text("'; DROP TABLE t;--")};
  int64_t rowid = 0;
  ASSERT_EQ_INT(perk_insert(db, "t", ic, pv, 1, &rowid), PERK_OK);
  ASSERT_EQ_INT(perk_begin(db), PERK_OK);
  PerkValue pv2[] = {perk_text("\" OR 1=1")};
  ASSERT_EQ_INT(perk_insert(db, "t", ic, pv2, 1, NULL), PERK_OK);
  ASSERT_EQ_INT(perk_commit(db), PERK_OK);
  perk_close(db);

  // Reopen: hardened again, data intact, tables still there
  db = NULL;
  ASSERT_EQ_INT(perk_open(path, &db), PERK_OK);
  if (db) {
    check_connection_hardening(db);
    PerkResult* res = NULL;
    ASSERT_EQ_INT(perk_select(db, "t", NULL, 0, "v = ?", pv, 1, &res), PERK_OK);
    if (res) {
      ASSERT_EQ_INT(perk_result_rows(res), 1);
      perk_result_free(res);
    }
    res = NULL;
    ASSERT_EQ_INT(perk_select(db, "t", NULL, 0, NULL, NULL, 0, &res), PERK_OK);
    if (res) {
      ASSERT_EQ_INT(perk_result_rows(res), 2);
      perk_result_free(res);
    }
    perk_close(db);
  }
  rm_db(path);
}

/**
 * Opening hostile paths: the path is only ever a filename, never SQL. A path
 * that looks like SQL must not execute anything and must not crash.
 */
static void test_open_path_is_not_sql(void) {
  PerkDB* db = NULL;
  PerkErr e = perk_open("/nonexistent-dir-xyz/'; DROP TABLE users;--.db", &db);
  ASSERT_EQ_INT(e, PERK_ERR_OPEN);
  ASSERT_TRUE(db == NULL);
  ASSERT_TRUE(strlen(perk_errmsg(NULL)) > 0);
  ASSERT_EQ_INT(perk_open(NULL, &db), PERK_ERR_MISUSE);
  ASSERT_EQ_INT(perk_open(":memory:", NULL), PERK_ERR_MISUSE);
}

/* ------------------------------------------------------------------- main */

int main(void) {
  make_name(g_long65, 65, 'a');
  make_name(g_long1000, 1000, 'a');

  test_regex_direct();
  test_regex_via_api();
  test_bad_identifiers();
  test_embedded_nul_in_names();
  test_bad_wheres();
  test_valid_wheres();
  test_value_payloads();
  test_long_value();
  test_arg_count_mismatch();
  test_create_table_types();
  test_boundary_names_via_api();
  test_keyword_identifiers();
  test_runtime_hardening();
  test_file_db_wal_and_reopen();
  test_open_path_is_not_sql();
  return harness_summary();
}
