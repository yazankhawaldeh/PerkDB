# node-gyp build for the PerkDB Node addon (build/Release/perkdb.node).
# The C library and SQLite are compiled from the same sources as the Makefile.
# Warnings are left non-fatal here: the Makefile's -Werror is for development
# builds only and must not break `npm install` on other compilers.
{
  "variables": {
    "perkdb_version": "<!(node -p \"require('./package.json').version\")"
  },
  "targets": [
    {
      # SQLite amalgamation, built like the Makefile's SQLITE_CFLAGS
      # (-DSQLITE_DQS=0, compiler-default C dialect, no warning flags).
      "target_name": "perkdb_sqlite",
      "type": "static_library",
      "sources": ["sqlite3-src/sqlite3.c"],
      "defines": ["SQLITE_DQS=0"],
      "cflags": ["-w"],
      "xcode_settings": {
        "MACOSX_DEPLOYMENT_TARGET": "11.0",
        "OTHER_CFLAGS": ["-w"]
      },
      "conditions": [
        ["OS!='win' and OS!='mac'", {
          # Linked into a shared module, so it must be position independent
          "cflags": ["-fPIC"]
        }]
      ]
    },
    {
      "target_name": "perkdb",
      "dependencies": ["perkdb_sqlite"],
      "sources": [
        "perkdb.c",
        "perk_crud.c",
        "perk_result.c",
        "perk_tx.c",
        "perk_util.c",
        "perk_where.c",
        "addon/perkdb_addon.cc"
      ],
      "include_dirs": [
        "<!(node -p \"require('node-addon-api').include_dir\")",
        "."
      ],
      "defines": [
        "SQLITE_DQS=0",
        "NAPI_VERSION=8",
        "NAPI_CPP_EXCEPTIONS",
        "PERKDB_ADDON_VERSION=\"<(perkdb_version)\""
      ],
      "cflags!": ["-fno-exceptions"],
      "cflags_cc!": ["-fno-exceptions"],
      "cflags_c": ["-std=c11"],
      "cflags_cc": ["-std=c++17", "-fexceptions"],
      "xcode_settings": {
        "GCC_C_LANGUAGE_STANDARD": "c11",
        "CLANG_CXX_LANGUAGE_STANDARD": "c++17",
        "CLANG_CXX_LIBRARY": "libc++",
        "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
        "MACOSX_DEPLOYMENT_TARGET": "11.0"
      },
      "conditions": [
        ["OS=='linux'", {
          "libraries": ["-lpthread", "-lm", "-ldl"]
        }]
      ]
    }
  ]
}
