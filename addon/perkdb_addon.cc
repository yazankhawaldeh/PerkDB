// N-API binding for PerkDB. Exposes the public C API (perkdb.h) to JavaScript
// as a Database class plus a few module-level helpers.
//
// Every argument is converted to C values (which may run user JS such as
// getters) BEFORE the database handle is read, so a getter that closes the
// database can't leave a C call holding a dangling PerkDB*.

#include <napi.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "../perkdb.h"
#include "../sqlite3-src/sqlite3.h"

#ifndef PERKDB_ADDON_VERSION
#define PERKDB_ADDON_VERSION "0.1.0"
#endif

namespace {

// Per-environment state (one per main thread / worker), freed by N-API.
struct AddonData {
  Napi::FunctionReference database_ctor;
  Napi::FunctionReference error_class;
};

// Largest integer a JS number holds exactly (Number.MAX_SAFE_INTEGER).
constexpr int64_t kMaxSafeInteger = 9007199254740991LL;

// Error code used when a method is called after close().
constexpr const char* kClosedCode = "PERK_ERR_CLOSED";

// Rough native memory held by an open connection, reported to V8 so that
// garbage collection (and so the GC close) runs sooner for unclosed handles.
constexpr int64_t kConnectionMemoryHint = 64 * 1024;

/*
 * Maps a PerkErr to its enum name.
 * @param err the status returned by a perk_* function
 * @return the enum name, e.g. "PERK_ERR_NOT_FOUND", or "PERK_ERR_UNKNOWN"
 */
const char* ErrCodeName(PerkErr err) {
  switch (err) {
    case PERK_OK:
      return "PERK_OK";
    case PERK_ERR_OPEN:
      return "PERK_ERR_OPEN";
    case PERK_ERR_SQL:
      return "PERK_ERR_SQL";
    case PERK_ERR_BUSY:
      return "PERK_ERR_BUSY";
    case PERK_ERR_BADNAME:
      return "PERK_ERR_BADNAME";
    case PERK_ERR_NOMEM:
      return "PERK_ERR_NOMEM";
    case PERK_ERR_MISUSE:
      return "PERK_ERR_MISUSE";
    case PERK_ERR_EXISTS:
      return "PERK_ERR_EXISTS";
    case PERK_ERR_NOT_FOUND:
      return "PERK_ERR_NOT_FOUND";
  }
  return "PERK_ERR_UNKNOWN";
}

/*
 * Throws a PerkError (or a plain Error if index.js did not register the
 * class) carrying code and errno properties.
 * @param env the N-API environment
 * @param code string code, e.g. "PERK_ERR_SQL"
 * @param errnum numeric PerkErr value
 * @param message error message; the code is used if it is empty
 */
[[noreturn]] void ThrowPerkError(Napi::Env env, const char* code, int errnum,
                                 const std::string& message) {
  const std::string text = message.empty() ? std::string(code) : message;
  Napi::String js_code = Napi::String::New(env, code);
  Napi::Number js_errno = Napi::Number::New(env, errnum);
  AddonData* data = env.GetInstanceData<AddonData>();
  Napi::Object error;
  if (data != nullptr && !data->error_class.IsEmpty()) {
    error = data->error_class.New(
        {Napi::String::New(env, text), js_code, js_errno});
  } else {
    error = Napi::Error::New(env, text).Value();
    error.Set("code", js_code);
    error.Set("errno", js_errno);
  }
  throw Napi::Error(env, error);
}

/*
 * Throws if a perk_* call failed, using perk_errmsg() as the message.
 * @param env the N-API environment
 * @param err the status returned by the C call
 * @param db the handle the call used (NULL for perk_open)
 */
void Check(Napi::Env env, PerkErr err, PerkDB* db) {
  if (err == PERK_OK) return;
  const char* msg = perk_errmsg(db);
  ThrowPerkError(env, ErrCodeName(err), static_cast<int>(err),
                 msg != nullptr ? msg : "");
}

/*
 * Throws a TypeError with a Node-style code property.
 * @param env the N-API environment
 * @param message error message
 * @param code e.g. "ERR_INVALID_ARG_TYPE" (default) or "ERR_INVALID_ARG_VALUE"
 */
[[noreturn]] void ThrowTypeError(Napi::Env env, const std::string& message,
                                 const char* code = "ERR_INVALID_ARG_TYPE") {
  Napi::Error error = Napi::TypeError::New(env, message);
  error.Value().Set("code", Napi::String::New(env, code));
  throw error;
}

/*
 * Throws a RangeError with code ERR_OUT_OF_RANGE.
 * @param env the N-API environment
 * @param message error message
 */
[[noreturn]] void ThrowRangeError(Napi::Env env, const std::string& message) {
  Napi::Error error = Napi::RangeError::New(env, message);
  error.Value().Set("code", Napi::String::New(env, "ERR_OUT_OF_RANGE"));
  throw error;
}

/*
 * Reads a JS string that is passed to C as a NUL-terminated string.
 * @param env the N-API environment
 * @param value the argument
 * @param what argument name for error messages
 * @return the UTF-8 string; throws TypeError if value is not a string or
 *         contains a NUL byte (C would silently truncate it)
 */
std::string GetCString(Napi::Env env, Napi::Value value,
                       const std::string& what) {
  if (!value.IsString()) ThrowTypeError(env, what + " must be a string");
  std::string s = value.As<Napi::String>().Utf8Value();
  if (s.find('\0') != std::string::npos) {
    ThrowTypeError(env, what + " must not contain NUL characters",
                   "ERR_INVALID_ARG_VALUE");
  }
  return s;
}

/*
 * @param value any JS value
 * @return true if value is null or undefined
 */
bool IsNullish(Napi::Value value) {
  return value.IsNull() || value.IsUndefined();
}

/*
 * @param value any JS value
 * @return true for a plain object usable as a column -> value map (not an
 *         array, typed array, buffer or function)
 */
bool IsRecord(Napi::Value value) {
  return value.IsObject() && !value.IsArray() && !value.IsTypedArray() &&
         !value.IsArrayBuffer() && !value.IsFunction();
}

/*
 * Lists an object's own enumerable string keys (integer keys as strings).
 * @param env the N-API environment
 * @param obj the object
 * @return the keys in property order
 */
Napi::Array OwnKeys(Napi::Env env, Napi::Object obj) {
  napi_value keys;
  napi_status status = napi_get_all_property_names(
      env, obj, napi_key_own_only,
      static_cast<napi_key_filter>(napi_key_enumerable | napi_key_skip_symbols),
      napi_key_numbers_to_strings, &keys);
  if (status != napi_ok) throw Napi::Error::New(env);
  return Napi::Array(env, keys);
}

// Owns the column names passed to C as a const char* array.
class NameList {
 public:
  /*
   * Appends a name.
   * @param name the column name
   */
  void Add(std::string name) { names_.push_back(std::move(name)); }

  /*
   * Appends every string in a JS array.
   * @param env the N-API environment
   * @param array JS array of strings
   * @param what argument name for error messages
   */
  void AddAll(Napi::Env env, Napi::Array array, const std::string& what) {
    for (uint32_t i = 0; i < array.Length(); i++) {
      Add(GetCString(env, array.Get(i), what + "[" + std::to_string(i) + "]"));
    }
  }

  /*
   * @return a pointer array valid until the list changes, or NULL if empty
   */
  const char** Data() {
    ptrs_.clear();
    for (const std::string& n : names_) ptrs_.push_back(n.c_str());
    return ptrs_.empty() ? nullptr : ptrs_.data();
  }

  /*
   * @return the number of names
   */
  size_t Size() const { return names_.size(); }

 private:
  std::deque<std::string> names_;
  std::vector<const char*> ptrs_;
};

// Owns a PerkValue array and copies of every text/blob it points to, so no
// pointer into JS memory is held across calls that may run user code.
class ValueList {
 public:
  /*
   * Converts a JS value and appends it.
   * number -> PERK_INT if integer-valued and within int64, else PERK_REAL;
   * bigint -> PERK_INT (RangeError outside int64); boolean -> PERK_INT 0/1;
   * string -> PERK_TEXT (UTF-8, embedded NULs kept); Buffer/Uint8Array ->
   * PERK_BLOB; null/undefined -> PERK_NULL; anything else -> TypeError.
   * @param env the N-API environment
   * @param value the JS value
   * @param what argument name for error messages
   */
  void Add(Napi::Env env, Napi::Value value, const std::string& what) {
    switch (value.Type()) {
      case napi_undefined:
      case napi_null:
        vals_.push_back(perk_null());
        return;
      case napi_boolean:
        vals_.push_back(perk_int(value.As<Napi::Boolean>().Value() ? 1 : 0));
        return;
      case napi_number:
        vals_.push_back(FromNumber(value.As<Napi::Number>().DoubleValue()));
        return;
      case napi_bigint: {
        bool lossless = false;
        int64_t i = value.As<Napi::BigInt>().Int64Value(&lossless);
        if (!lossless)
          ThrowRangeError(env, what + " is outside the int64 range");
        vals_.push_back(perk_int(i));
        return;
      }
      case napi_string: {
        const std::string& s =
            store_.emplace_back(value.As<Napi::String>().Utf8Value());
        PerkValue pv = perk_null();
        pv.type = PERK_TEXT;
        pv.as.text.ptr = s.c_str();
        pv.as.text.len = s.size();
        vals_.push_back(pv);
        return;
      }
      default:
        break;
    }
    if (value.IsTypedArray()) {
      Napi::TypedArray typed = value.As<Napi::TypedArray>();
      if (typed.TypedArrayType() == napi_uint8_array) {
        Napi::Uint8Array bytes = value.As<Napi::Uint8Array>();
        const char* src = reinterpret_cast<const char*>(bytes.Data());
        const std::string& copy =
            store_.emplace_back(src, src == nullptr ? 0 : bytes.ByteLength());
        vals_.push_back(perk_blob(copy.data(), copy.size()));
        return;
      }
    }
    ThrowTypeError(env, what +
                            " must be a number, bigint, boolean, string, "
                            "Buffer, Uint8Array, null or undefined");
  }

  /*
   * Appends every element of a JS array.
   * @param env the N-API environment
   * @param array the values
   * @param what argument name for error messages
   */
  void AddAll(Napi::Env env, Napi::Array array, const std::string& what) {
    for (uint32_t i = 0; i < array.Length(); i++) {
      Add(env, array.Get(i), what + "[" + std::to_string(i) + "]");
    }
  }

  /*
   * @return the values, or NULL if there are none
   */
  const PerkValue* Data() const {
    return vals_.empty() ? nullptr : vals_.data();
  }

  /*
   * @return the number of values
   */
  size_t Size() const { return vals_.size(); }

 private:
  /*
   * @param d a JS number
   * @return PERK_INT for integer-valued numbers that fit int64, else PERK_REAL
   */
  static PerkValue FromNumber(double d) {
    // 2^63 is exact as a double; the int64 range is [-2^63, 2^63)
    const double kTwo63 = 9223372036854775808.0;
    if (std::isfinite(d) && std::trunc(d) == d && d >= -kTwo63 && d < kTwo63) {
      return perk_int(static_cast<int64_t>(d));
    }
    return perk_real(d);
  }

  std::vector<PerkValue> vals_;
  std::deque<std::string> store_;
};

// Frees a PerkResult when it goes out of scope (including on exceptions).
struct ResultDeleter {
  void operator()(PerkResult* res) const { perk_result_free(res); }
};
using ResultPtr = std::unique_ptr<PerkResult, ResultDeleter>;

/*
 * Converts an int64 to a JS number if it is a safe integer, else a BigInt.
 * @param env the N-API environment
 * @param v the value
 * @return number or BigInt
 */
Napi::Value IntToJs(Napi::Env env, int64_t v) {
  if (v >= -kMaxSafeInteger && v <= kMaxSafeInteger) {
    return Napi::Number::New(env, static_cast<double>(v));
  }
  return Napi::BigInt::New(env, v);
}

/*
 * Converts one result cell to JS: INTEGER -> number/BigInt, REAL -> number,
 * TEXT -> string, BLOB -> Buffer (copied), NULL -> null.
 * @param env the N-API environment
 * @param v the cell from perk_result_get(), may be NULL
 * @return the JS value
 */
Napi::Value CellToJs(Napi::Env env, const PerkValue* v) {
  if (v == nullptr) return env.Null();
  switch (v->type) {
    case PERK_INT:
      return IntToJs(env, v->as.i);
    case PERK_REAL:
      return Napi::Number::New(env, v->as.r);
    case PERK_TEXT:
      return Napi::String::New(env, v->as.text.ptr, v->as.text.len);
    case PERK_BLOB:
      if (v->as.blob.len == 0 || v->as.blob.ptr == nullptr) {
        return Napi::Buffer<uint8_t>::New(env, 0);
      }
      return Napi::Buffer<uint8_t>::Copy(
          env, static_cast<const uint8_t*>(v->as.blob.ptr), v->as.blob.len);
    case PERK_NULL:
      break;
  }
  return env.Null();
}

/*
 * Converts one result row into a JS object. Properties are defined (not
 * assigned), so names such as "__proto__" are ordinary data properties and
 * no setter on Object.prototype runs.
 * @param env the N-API environment
 * @param res the result
 * @param row row index
 * @param names one JS string per column
 * @return the row object
 */
Napi::Object RowToJs(Napi::Env env, const PerkResult* res, size_t row,
                     const std::vector<napi_value>& names) {
  Napi::Object obj = Napi::Object::New(env);
  std::vector<napi_property_descriptor> props(names.size());
  for (size_t c = 0; c < names.size(); c++) {
    props[c] =
        napi_property_descriptor{nullptr,
                                 names[c],
                                 nullptr,
                                 nullptr,
                                 nullptr,
                                 CellToJs(env, perk_result_get(res, row, c)),
                                 napi_default_jsproperty,
                                 nullptr};
  }
  if (!props.empty()) {
    napi_status status =
        napi_define_properties(env, obj, props.size(), props.data());
    if (status != napi_ok) throw Napi::Error::New(env);
  }
  return obj;
}

/*
 * Converts a whole result into an array of row objects.
 * @param env the N-API environment
 * @param res the result (not freed here)
 * @param max_rows stop after this many rows
 * @return array of row objects keyed by column name
 */
Napi::Array ResultToRows(Napi::Env env, const PerkResult* res,
                         size_t max_rows) {
  size_t nrows = perk_result_rows(res);
  if (nrows > max_rows) nrows = max_rows;
  if (nrows > UINT32_MAX) ThrowRangeError(env, "result has too many rows");
  const size_t ncols = perk_result_cols(res);
  std::vector<napi_value> names(ncols);
  for (size_t c = 0; c < ncols; c++) {
    const char* name = perk_result_colname(res, c);
    names[c] = Napi::String::New(env, name != nullptr ? name : "");
  }
  Napi::Array rows = Napi::Array::New(env, nrows);
  for (size_t r = 0; r < nrows; r++) {
    Napi::HandleScope scope(env);
    rows.Set(static_cast<uint32_t>(r), RowToJs(env, res, r, names));
  }
  return rows;
}

/*
 * Reads an optional boolean property.
 * @param env the N-API environment
 * @param obj the object
 * @param key property name
 * @param what object name for error messages
 * @return the value, false if undefined; TypeError for non-booleans
 */
bool GetFlag(Napi::Env env, Napi::Object obj, const char* key,
             const std::string& what) {
  Napi::Value v = obj.Get(key);
  if (v.IsUndefined()) return false;
  if (!v.IsBoolean())
    ThrowTypeError(env, what + "." + key + " must be a boolean");
  return v.As<Napi::Boolean>().Value();
}

/*
 * Reads an optional array argument.
 * @param env the N-API environment
 * @param value the argument (undefined/null mean "none")
 * @param what argument name for error messages
 * @return the array, or an empty array
 */
Napi::Array OptionalArray(Napi::Env env, Napi::Value value,
                          const std::string& what) {
  if (IsNullish(value)) return Napi::Array::New(env, 0);
  if (!value.IsArray()) ThrowTypeError(env, what + " must be an array");
  return value.As<Napi::Array>();
}

/*
 * Reads an optional WHERE string.
 * @param env the N-API environment
 * @param value the argument (undefined/null mean "every row")
 * @param out receives the string
 * @return true if a string was given, false for null/undefined
 */
bool OptionalWhere(Napi::Env env, Napi::Value value, std::string* out) {
  if (IsNullish(value)) return false;
  *out = GetCString(env, value, "where");
  return true;
}

/*
 * Reads an optional options object.
 * @param env the N-API environment
 * @param value the argument
 * @param what argument name for error messages
 * @return the object (an empty one for undefined/null)
 */
Napi::Object OptionalOptions(Napi::Env env, Napi::Value value,
                             const std::string& what) {
  if (IsNullish(value)) return Napi::Object::New(env);
  if (!IsRecord(value)) ThrowTypeError(env, what + " must be an object");
  return value.As<Napi::Object>();
}

/*
 * Splits a column -> value object into names and values.
 * @param env the N-API environment
 * @param value the object
 * @param what argument name for error messages
 * @param names receives the keys
 * @param vals receives the values
 */
void RecordToLists(Napi::Env env, Napi::Value value, const std::string& what,
                   NameList* names, ValueList* vals) {
  if (!IsRecord(value)) ThrowTypeError(env, what + " must be an object");
  Napi::Object obj = value.As<Napi::Object>();
  Napi::Array keys = OwnKeys(env, obj);
  for (uint32_t i = 0; i < keys.Length(); i++) {
    Napi::Value key = keys.Get(i);
    std::string name = GetCString(env, key, what + " key");
    vals->Add(env, obj.Get(key), what + "." + name);
    names->Add(std::move(name));
  }
}

// JS wrapper around one PerkDB connection. The handle is closed by close() or,
// failing that, when the object is garbage-collected.
class Database : public Napi::ObjectWrap<Database> {
 public:
  /*
   * Defines the Database class.
   * @param env the N-API environment
   * @return the constructor
   */
  static Napi::Function Define(Napi::Env env) {
    return DefineClass(
        env, "Database",
        {
            StaticMethod("open", &Database::Open),
            InstanceAccessor("isOpen", &Database::IsOpen, nullptr),
            InstanceMethod("close", &Database::Close),
            InstanceMethod("errmsg", &Database::Errmsg),
            InstanceMethod("createTable", &Database::CreateTable),
            InstanceMethod("insert", &Database::Insert),
            InstanceMethod("select", &Database::Select),
            InstanceMethod("selectById", &Database::SelectById),
            InstanceMethod("update", &Database::Update),
            InstanceMethod("delete", &Database::Delete),
            InstanceMethod("begin", &Database::Begin),
            InstanceMethod("commit", &Database::Commit),
            InstanceMethod("rollback", &Database::Rollback),
        });
  }

  /*
   * new Database(path): opens path with perk_open().
   * @param info info[0] is the file path (":memory:" for an in-memory db)
   */
  explicit Database(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<Database>(info) {
    Napi::Env env = info.Env();
    const std::string path = GetCString(env, info[0], "path");
    PerkDB* db = nullptr;
    Check(env, perk_open(path.c_str(), &db), nullptr);
    db_ = db;
    env_ = env;
    Napi::MemoryManagement::AdjustExternalMemory(env, kConnectionMemoryHint);
  }

  /*
   * Closes the handle if close() was never called (garbage collection).
   */
  ~Database() override { CloseHandle(); }

 private:
  /*
   * Closes the handle once; later calls do nothing.
   */
  void CloseHandle() {
    if (db_ != nullptr) {
      perk_close(db_);
      db_ = nullptr;
      napi_adjust_external_memory(env_, -kConnectionMemoryHint, nullptr);
    }
  }

  /*
   * Returns the open handle. Call only after every argument is converted.
   * @param env the N-API environment
   * @return the handle; throws PerkError PERK_ERR_CLOSED after close()
   */
  PerkDB* Handle(Napi::Env env) const {
    if (db_ == nullptr) {
      ThrowPerkError(env, kClosedCode, static_cast<int>(PERK_ERR_MISUSE),
                     "the database is closed");
    }
    return db_;
  }

  /*
   * Database.open(path): same as new Database(path).
   * @param info info[0] is the file path
   * @return the new Database
   */
  static Napi::Value Open(const Napi::CallbackInfo& info) {
    AddonData* data = info.Env().GetInstanceData<AddonData>();
    return data->database_ctor.New({info[0]});
  }

  /*
   * isOpen getter.
   * @param info unused
   * @return false after close()
   */
  Napi::Value IsOpen(const Napi::CallbackInfo& info) {
    return Napi::Boolean::New(info.Env(), db_ != nullptr);
  }

  /*
   * close(): perk_close(). Idempotent.
   * @param info unused
   * @return undefined
   */
  Napi::Value Close(const Napi::CallbackInfo& info) {
    CloseHandle();
    return info.Env().Undefined();
  }

  /*
   * errmsg(): perk_errmsg() for this connection.
   * @param info unused
   * @return the last error message (may be stale or empty)
   */
  Napi::Value Errmsg(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    const char* msg = perk_errmsg(Handle(env));
    return Napi::String::New(env, msg != nullptr ? msg : "");
  }

  /*
   * createTable(name, columns): perk_create_table().
   * @param info info[0] table name; info[1] array of {name, type?, primaryKey?,
   *        notNull?, unique?, flags?}
   * @return undefined
   */
  Napi::Value CreateTable(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    const std::string table = GetCString(env, info[0], "table");
    if (!info[1].IsArray()) ThrowTypeError(env, "columns must be an array");
    Napi::Array arr = info[1].As<Napi::Array>();
    std::deque<std::string> strings;
    std::vector<PerkColumn> cols;
    for (uint32_t i = 0; i < arr.Length(); i++) {
      const std::string what = "columns[" + std::to_string(i) + "]";
      Napi::Value item = arr.Get(i);
      if (!IsRecord(item)) ThrowTypeError(env, what + " must be an object");
      Napi::Object col = item.As<Napi::Object>();
      PerkColumn pc;
      pc.name =
          strings.emplace_back(GetCString(env, col.Get("name"), what + ".name"))
              .c_str();
      Napi::Value type = col.Get("type");
      pc.type =
          IsNullish(type)
              ? nullptr
              : strings.emplace_back(GetCString(env, type, what + ".type"))
                    .c_str();
      pc.flags = 0;
      if (GetFlag(env, col, "primaryKey", what)) pc.flags |= PERK_COL_PK;
      if (GetFlag(env, col, "notNull", what)) pc.flags |= PERK_COL_NOTNULL;
      if (GetFlag(env, col, "unique", what)) pc.flags |= PERK_COL_UNIQUE;
      Napi::Value flags = col.Get("flags");
      if (!flags.IsUndefined()) {
        if (!flags.IsNumber())
          ThrowTypeError(env, what + ".flags must be a number");
        pc.flags |= flags.As<Napi::Number>().Int32Value();
      }
      cols.push_back(pc);
    }
    PerkDB* db = Handle(env);
    Check(env,
          perk_create_table(db, table.c_str(),
                            cols.empty() ? nullptr : cols.data(), cols.size()),
          db);
    return env.Undefined();
  }

  /*
   * insert(table, values) or insert(table, columns, values): perk_insert().
   * @param info info[0] table; info[1] a column -> value object, or an array of
   *        column names with info[2] the matching array of values
   * @return the new rowid (number, or BigInt above 2^53 - 1)
   */
  Napi::Value Insert(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    const std::string table = GetCString(env, info[0], "table");
    NameList names;
    ValueList vals;
    if (info[1].IsArray()) {
      if (!info[2].IsArray()) ThrowTypeError(env, "values must be an array");
      Napi::Array cols = info[1].As<Napi::Array>();
      Napi::Array values = info[2].As<Napi::Array>();
      if (cols.Length() != values.Length()) {
        ThrowTypeError(env, "columns and values must have the same length",
                       "ERR_INVALID_ARG_VALUE");
      }
      names.AddAll(env, cols, "columns");
      vals.AddAll(env, values, "values");
    } else {
      RecordToLists(env, info[1], "values", &names, &vals);
    }
    PerkDB* db = Handle(env);
    int64_t rowid = 0;
    Check(env,
          perk_insert(db, table.c_str(), names.Data(), vals.Data(), vals.Size(),
                      &rowid),
          db);
    return IntToJs(env, rowid);
  }

  /*
   * select(table, {columns?, where?, args?}): perk_select().
   * @param info info[0] table; info[1] optional options
   * @return array of row objects (empty if nothing matches)
   */
  Napi::Value Select(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    const std::string table = GetCString(env, info[0], "table");
    Napi::Object opts = OptionalOptions(env, info[1], "options");
    NameList cols;
    cols.AddAll(env, OptionalArray(env, opts.Get("columns"), "options.columns"),
                "options.columns");
    std::string where;
    const bool has_where = OptionalWhere(env, opts.Get("where"), &where);
    ValueList args;
    args.AddAll(env, OptionalArray(env, opts.Get("args"), "options.args"),
                "options.args");
    PerkDB* db = Handle(env);
    PerkResult* raw = nullptr;
    PerkErr err = perk_select(db, table.c_str(), cols.Data(), cols.Size(),
                              has_where ? where.c_str() : nullptr, args.Data(),
                              args.Size(), &raw);
    ResultPtr res(raw);
    Check(env, err, db);
    return ResultToRows(env, res.get(), SIZE_MAX);
  }

  /*
   * selectById(table, idColumn, id, {columns?}): perk_select_by_id().
   * @param info info[0] table; info[1] key column; info[2] key value;
   *        info[3] optional options
   * @return the first matching row object; throws PERK_ERR_NOT_FOUND if none
   */
  Napi::Value SelectById(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    const std::string table = GetCString(env, info[0], "table");
    const std::string id_col = GetCString(env, info[1], "idColumn");
    ValueList id;
    id.Add(env, info[2], "id");
    Napi::Object opts = OptionalOptions(env, info[3], "options");
    NameList cols;
    cols.AddAll(env, OptionalArray(env, opts.Get("columns"), "options.columns"),
                "options.columns");
    PerkDB* db = Handle(env);
    PerkResult* raw = nullptr;
    PerkErr err = perk_select_by_id(db, table.c_str(), cols.Data(), cols.Size(),
                                    id_col.c_str(), id.Data(), &raw);
    ResultPtr res(raw);
    Check(env, err, db);
    Napi::Array rows = ResultToRows(env, res.get(), 1);
    if (rows.Length() == 0) {
      // perk_select_by_id promises at least one row on PERK_OK
      ThrowPerkError(env, ErrCodeName(PERK_ERR_NOT_FOUND),
                     static_cast<int>(PERK_ERR_NOT_FOUND), "no row found");
    }
    return rows.Get(static_cast<uint32_t>(0));
  }

  /*
   * update(table, set, where?, args?): perk_update().
   * @param info info[0] table; info[1] column -> new value object; info[2]
   *        optional WHERE (null/undefined updates every row); info[3] args
   * @return number of rows changed; throws PERK_ERR_NOT_FOUND if none matched
   */
  Napi::Value Update(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    const std::string table = GetCString(env, info[0], "table");
    NameList set_cols;
    ValueList set_vals;
    RecordToLists(env, info[1], "set", &set_cols, &set_vals);
    std::string where;
    const bool has_where = OptionalWhere(env, info[2], &where);
    ValueList args;
    args.AddAll(env, OptionalArray(env, info[3], "args"), "args");
    PerkDB* db = Handle(env);
    int64_t changed = 0;
    Check(env,
          perk_update(db, table.c_str(), set_cols.Data(), set_vals.Data(),
                      set_vals.Size(), has_where ? where.c_str() : nullptr,
                      args.Data(), args.Size(), &changed),
          db);
    return IntToJs(env, changed);
  }

  /*
   * delete(table, where?, args?): perk_delete().
   * @param info info[0] table; info[1] optional WHERE (null/undefined deletes
   *        every row); info[2] args
   * @return number of rows deleted; throws PERK_ERR_NOT_FOUND if none matched
   */
  Napi::Value Delete(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    const std::string table = GetCString(env, info[0], "table");
    std::string where;
    const bool has_where = OptionalWhere(env, info[1], &where);
    ValueList args;
    args.AddAll(env, OptionalArray(env, info[2], "args"), "args");
    PerkDB* db = Handle(env);
    int64_t changed = 0;
    Check(env,
          perk_delete(db, table.c_str(), has_where ? where.c_str() : nullptr,
                      args.Data(), args.Size(), &changed),
          db);
    return IntToJs(env, changed);
  }

  /*
   * begin(): perk_begin() (BEGIN IMMEDIATE).
   * @param info unused
   * @return undefined
   */
  Napi::Value Begin(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    PerkDB* db = Handle(env);
    Check(env, perk_begin(db), db);
    return env.Undefined();
  }

  /*
   * commit(): perk_commit().
   * @param info unused
   * @return undefined
   */
  Napi::Value Commit(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    PerkDB* db = Handle(env);
    Check(env, perk_commit(db), db);
    return env.Undefined();
  }

  /*
   * rollback(): perk_rollback().
   * @param info unused
   * @return undefined
   */
  Napi::Value Rollback(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    PerkDB* db = Handle(env);
    Check(env, perk_rollback(db), db);
    return env.Undefined();
  }

  PerkDB* db_ = nullptr;
  napi_env env_ = nullptr;
};

/*
 * Reports the addon version.
 * @param info call arguments (none are used)
 * @return the addon version string, e.g. "0.1.0"
 */
Napi::Value Version(const Napi::CallbackInfo& info) {
  return Napi::String::New(info.Env(), PERKDB_ADDON_VERSION);
}

/*
 * Reports the version of the SQLite amalgamation linked into the addon.
 * @param info call arguments (none are used)
 * @return the SQLite library version string, e.g. "3.50.2"
 */
Napi::Value SqliteVersion(const Napi::CallbackInfo& info) {
  return Napi::String::New(info.Env(), sqlite3_libversion());
}

/*
 * openErrmsg(): perk_errmsg(NULL), the reason the last perk_open() on this
 * thread failed.
 * @param info call arguments (none are used)
 * @return the message (empty if no open has failed)
 */
Napi::Value OpenErrmsg(const Napi::CallbackInfo& info) {
  const char* msg = perk_errmsg(nullptr);
  return Napi::String::New(info.Env(), msg != nullptr ? msg : "");
}

/*
 * Registers the error class thrown for PerkErr failures (called by index.js).
 * It is constructed as new ErrorClass(message, code, errno).
 * @param info info[0] the class (a function)
 * @return undefined
 */
Napi::Value SetErrorClass(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!info[0].IsFunction())
    ThrowTypeError(env, "errorClass must be a function");
  AddonData* data = env.GetInstanceData<AddonData>();
  data->error_class = Napi::Persistent(info[0].As<Napi::Function>());
  return env.Undefined();
}

/*
 * Builds the constants object (PerkErr, PerkColFlags and PerkType values).
 * @param env the N-API environment
 * @return a frozen object of name -> number
 */
Napi::Object Constants(Napi::Env env) {
  Napi::Object c = Napi::Object::New(env);
  const PerkErr errs[] = {
      PERK_OK,         PERK_ERR_OPEN,    PERK_ERR_SQL,
      PERK_ERR_BUSY,   PERK_ERR_BADNAME, PERK_ERR_NOMEM,
      PERK_ERR_MISUSE, PERK_ERR_EXISTS,  PERK_ERR_NOT_FOUND};
  for (PerkErr e : errs) c.Set(ErrCodeName(e), static_cast<int>(e));
  c.Set("PERK_COL_PK", static_cast<int>(PERK_COL_PK));
  c.Set("PERK_COL_NOTNULL", static_cast<int>(PERK_COL_NOTNULL));
  c.Set("PERK_COL_UNIQUE", static_cast<int>(PERK_COL_UNIQUE));
  c.Set("PERK_NULL", static_cast<int>(PERK_NULL));
  c.Set("PERK_INT", static_cast<int>(PERK_INT));
  c.Set("PERK_REAL", static_cast<int>(PERK_REAL));
  c.Set("PERK_TEXT", static_cast<int>(PERK_TEXT));
  c.Set("PERK_BLOB", static_cast<int>(PERK_BLOB));
  c.Freeze();
  return c;
}

/*
 * Registers the addon's exports.
 * @param env the N-API environment
 * @param exports the module's exports object
 * @return the populated exports object
 */
Napi::Object Init(Napi::Env env, Napi::Object exports) {
  AddonData* data = new AddonData();
  env.SetInstanceData<AddonData>(data);
  Napi::Function database = Database::Define(env);
  data->database_ctor = Napi::Persistent(database);

  exports.Set("Database", database);
  exports.Set("constants", Constants(env));
  exports.Set("openErrmsg", Napi::Function::New(env, OpenErrmsg, "openErrmsg"));
  exports.Set("setErrorClass",
              Napi::Function::New(env, SetErrorClass, "setErrorClass"));
  exports.Set("version", Napi::Function::New(env, Version, "version"));
  exports.Set("sqliteVersion",
              Napi::Function::New(env, SqliteVersion, "sqliteVersion"));
  exports.Set("loaded", Napi::Boolean::New(env, true));
  return exports;
}

}  // namespace

NODE_API_MODULE(perkdb, Init)
