### Pith Language: A Technical Overview

Pith is an experimental imperative statically-typed scripting language implemented as a tree-walking interpreter in C.

This README summarizes how Pith currently works, how to build/run tests, and links to the design notes in `DESIGN.md`.

Key points
- Interpreter pipeline: Tokenizer -> Parser (AST) -> Interpreter (tree-walk).
- Execution modes: file execution (`pith [filename]`), REPL (no args), and interactive file mode (`pith -i script.pith`).
- Strings are immutable byte strings; the built-in `buffer` type is their mutable counterpart for building text or binary data efficiently (`buffer out = buffer(); out.append("..."); string s = out.to_string()`). See `pith_buffer.h`.
- Declared types are enforced at runtime (variables, parameters, return values, fields, list/map elements) with no implicit conversions except int -> float. See DESIGN.md section 2.3.

Building and running
- This project uses CMake; open in CLion or build from the command-line with CMake. Typical CMake out-of-source build directory used here is `cmake-build-debug` (created by CLion).
- After building you should have `cmake-build-debug\pith_lang.exe` on Windows.

Build requirements
- The interpreter itself has **no external dependencies**: all you need is a C99
  compiler and CMake (>= 3.28). It links only against the C math library (libm).
- Plain command-line build:

  ```sh
  cmake -S . -B build
  cmake --build build -j
  ```

- The optional `gui` module is the **only** part with extra requirements, and it
  is **off by default** — a normal build never fetches raylib or needs any
  graphics libraries. See "Optional: GUI module" below and `docs/GUI.md`.

Optional: GUI module
- The `gui` module (raylib + raygui) is strictly opt-in via `-DPITH_ENABLE_GUI=ON`.
  It is never built, fetched, or required unless you ask for it.
- On Linux (X11 backend, the default — also runs on Wayland via XWayland) it needs
  these system development packages. On Debian/Ubuntu:

  ```sh
  sudo apt-get install libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl1-mesa-dev
  ```

- Then build with the module enabled (raylib is downloaded automatically on first
  configure; raygui is vendored in `third_party/`):

  ```sh
  cmake -DPITH_ENABLE_GUI=ON -S . -B build
  cmake --build build -j
  ./build/pith_lang examples/gui_demo.pith
  ```

- Native Wayland (no X11) and the full API reference are documented in `docs/GUI.md`.

Running tests
- The `tests/` folder contains `.pith` files and `.expected` outputs. The repository includes a Windows batch script `run_test.bat` that executes each `.pith` with the built interpreter and compares stdout against the `.expected` files.
- Usage (PowerShell / cmd):

```powershell
.\run_test.bat
```

- `run_test.bat` expects the test binary at `cmake-build-debug\pith_lang.exe`. It prints PASS/FAIL per test and a summary at the end.

Standard library and modules
- `stdlib/` contains built-in Pith libraries (e.g., `math.pith`, `io.pith`, `str.pith`, etc.).
- `lib/` contains the native library modules written in C (`io`, `sys`, `time`, `http`, `json`, `regex`, `encoding`, `crypto`, `sqlite`, `template`, and the optional `gui`; `sha256.c` holds the hash algorithms). `third_party/` holds vendored code (raygui, SQLite) with its version and source recorded.. Modules can define native types (like `Regex`) with `native_object.h`. Each is a `*_module.c` / `.h` pair (`gui.c` / `gui.h` for the GUI) with a `<name>_register_module()` entry point called from `register_all_native_modules()` in `interpreter.c`.
- Native modules are registered at runtime and exposed as module objects (e.g., `import "math"; math.sqrt(16)`). The smaller built-in modules (`math`, `random`, `integer`) are still implemented inside `interpreter.c`.
- The `time` module provides `sleep(seconds)`, `sleep_ms(ms)`, `now()`, `millis()`, `perf()` and `format(fmt [, timestamp])`. See `lib/time_module.h`.
- The `http` module is an HTTP/1.1 server: routing with path parameters (`route`, `serve`, `stop`, `param`), static files (`static`), keep-alive, and a non-blocking event loop so slow clients never stall others. A low-level `accept`/`respond` loop is also available, plus `header`, `parse_query`, `url_decode` and `url_encode`. See `lib/http_module.h` and `examples/web_server.pith`; end-to-end tests live in `tests/http/`.
- The `io` module handles files, directories and paths; `sys` gives script arguments (`pith_lang app.pith arg1 arg2`), environment variables, the working directory and the platform. See `lib/io_module.h` and `lib/sys_module.h`.
- The `regex` module provides regular expressions with a linear-time engine (safe against catastrophic backtracking): `Regex r = regex.compile(r"(\d+)-(\d+)")`, then `r.find(text)`, `r.find_all`, `r.replace`, `r.split`. Write patterns as raw strings (`r"..."`). See `lib/regex_module.h`.
- `examples/blog/` is a complete multi-user blog (accounts, posts, comments, search, JSON API, RSS) built with `http`, `sqlite`, `crypto`, `template` and more; its end-to-end tests (`tests/blog/`) run with `./run_tests.sh`. See `examples/blog/README.md`.
- The `template` module renders HTML templates: `Template page = template.load("views/users.html")`, `page.render({"users": users})`. Handlebars-style tags (`{{name}}`, `{{#if age < 18}}`, `{{#each users as user}}`, `{{> header}}`), auto-escaping, typed Pith helper functions, and errors that point at the template line. See `lib/template_module.h`.
- The `sqlite` module embeds an SQL database: `Database db = sqlite.open("app.db")`, `db.execute(sql, params)`, `db.query(sql, params)` returning a list of row maps, transactions, and typed values in both directions. SQLite itself is vendored (`third_party/sqlite/`), so nothing needs installing. See `lib/sqlite_module.h`.
- The `crypto` module provides secure random tokens, SHA-256, HMAC-SHA256, timing-safe comparison and password hashing (PBKDF2-HMAC-SHA256, 600,000 iterations); `encoding` converts bytes to and from base64 and hex. See `lib/crypto_module.h` and `lib/encoding_module.h`.
- The `json` module provides `encode(value)` and `decode(text)`. See `lib/json_module.h`.
- An optional graphical UI module (`gui`, built on raylib + raygui) is available as an opt-in build. See `docs/GUI.md`.

Debugging
- For development, `debug.h` contains compile-time flags for detailed tracing: tokenizer, parser, interpreter, environment, GC events, native calls, and more. Toggle the flags in `debug.h` and recompile to get detailed logs.

Performance
- `PERFORMANCE.md` documents completed interpreter optimizations, how to benchmark, and the roadmap of remaining work. Benchmark programs live in `benchmarks/`.

Design and further details
- For a deeper, implementation-accurate specification see `DESIGN.md`.

Contributing & Roadmap
- The `TODO.md` file lists current priorities, known limitations, and the roadmap for new features and bug fixes. If you plan to change syntax or typing rules, please coordinate so we keep the language's static typing goals consistent.

Contact
- For questions about design choices (especially syntax or typing behavior) leave an issue or start a discussion 
