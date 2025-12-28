### Pith Language: A Technical Overview

Pith is an experimental imperative statically-typed scripting language implemented as a tree-walking interpreter in C.

This README summarizes how Pith currently works, how to build/run tests, and links to the design notes in `DESIGN.md`.

Key points
- Interpreter pipeline: Tokenizer -> Parser (AST) -> Interpreter (tree-walk).
- Execution modes: file execution (`pith [filename]`), REPL (no args), and interactive file mode (`pith -i script.pith`).
- Language aims to stay statically typed at the surface (explicit type annotations), while some type enforcement (e.g., lists) is currently runtime-lax and slated for improvement.

Building and running
- This project uses CMake; open in CLion or build from the command-line with CMake. Typical CMake out-of-source build directory used here is `cmake-build-debug` (created by CLion).
- After building you should have `cmake-build-debug\pith_lang.exe` on Windows.

Running tests
- The `tests/` folder contains `.pith` files and `.expected` outputs. The repository includes a Windows batch script `run_test.bat` that executes each `.pith` with the built interpreter and compares stdout against the `.expected` files.
- Usage (PowerShell / cmd):

```powershell
.\run_test.bat
```

- `run_test.bat` expects the test binary at `cmake-build-debug\pith_lang.exe`. It prints PASS/FAIL per test and a summary at the end.

Standard library and modules
- `stdlib/` contains built-in Pith libraries (e.g., `math.pith`, `io.pith`, `str.pith`, etc.).
- Native modules are registered at runtime and exposed as module objects (e.g., `import "math"; math.sqrt(16)`).

Debugging
- For development, `debug.h` contains compile-time flags for detailed tracing: tokenizer, parser, interpreter, environment, GC events, native calls, and more. Toggle the flags in `debug.h` and recompile to get detailed logs.

Design and further details
- For a deeper, implementation-accurate specification see `DESIGN.md`.

Contributing & Roadmap
- The `TODO.md` file lists current priorities, known limitations, and the roadmap for new features and bug fixes. If you plan to change syntax or typing rules, please coordinate so we keep the language's static typing goals consistent.

Contact
- For questions about design choices (especially syntax or typing behavior) leave an issue or start a discussion 
