// N-API entry point for the PerkDB Node addon. This story only builds and loads
// the addon; the CRUD functions are exposed in a later story.

#include <napi.h>

#include "../sqlite3-src/sqlite3.h"

#ifndef PERKDB_ADDON_VERSION
#define PERKDB_ADDON_VERSION "0.1.0"
#endif

namespace {

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
 * Registers the addon's exports.
 * @param env the N-API environment
 * @param exports the module's exports object
 * @return the populated exports object
 */
Napi::Object Init(Napi::Env env, Napi::Object exports) {
  exports.Set("version", Napi::Function::New(env, Version, "version"));
  exports.Set("sqliteVersion",
              Napi::Function::New(env, SqliteVersion, "sqliteVersion"));
  exports.Set("loaded", Napi::Boolean::New(env, true));
  return exports;
}

}  // namespace

NODE_API_MODULE(perkdb, Init)
