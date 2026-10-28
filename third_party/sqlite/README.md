# SQLite (vendored)

The `sqlite` module (`lib/sqlite_module.c`) embeds SQLite from its official
single-file "amalgamation", so Pith needs no SQLite installation on any
platform and the build works offline.

| | |
|---|---|
| Version | 3.54.1 |
| Source | https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip |
| SHA3-256 of the zip | `92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30` (matches sqlite.org/download.html) |
| Files kept | `sqlite3.c`, `sqlite3.h` (unmodified) |
| License | Public domain (https://www.sqlite.org/copyright.html) |

Compile-time options are set in `CMakeLists.txt` (target `pith_sqlite`):

- `SQLITE_THREADSAFE=0` - the interpreter is single-threaded, so SQLite's
  internal locking is unnecessary overhead.
- `SQLITE_OMIT_LOAD_EXTENSION` - no loading of native extension libraries from
  SQL (removes the dependency on `dlopen` and a way to run arbitrary code).
- `SQLITE_DQS=0` - "double-quoted strings" are always identifiers, never string
  literals, as the SQLite documentation recommends.

## Upgrading

1. Download the newest "sqlite-amalgamation-*.zip" from
   https://www.sqlite.org/download.html and check its SHA3-256 against the one
   listed on that page.
2. Replace `sqlite3.c` and `sqlite3.h` here with the ones from the zip.
3. Update the table above, rebuild, and run `./run_tests.sh`.
