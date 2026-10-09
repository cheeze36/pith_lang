# Pith Language Project - TODO List

This document tracks the major features, bug fixes, and improvements for the Pith language. Items are grouped by priority and include concrete next steps.

## 0. Project goals
- Keep Pith statically-typed at the surface: explicit type annotations for variables, function parameters, and return types remain required.
- Improve runtime robustness and add optional static checks in future (design decisions below require your input).

## 1. High Priority & Core Features (Must-Have)

- [x] Garbage Collector (GC) — implemented (Mark-and-Sweep)
- [x] Multi-line comments (`### ... ###`) and single-line comments `#` (standardized to `#`)
- [x] Core stdlib: `math`, `io`, `sys`, `str`, `list` methods implemented in C
- [x] `isinstance()` native helper and class/instance semantics
- [x] Tests: A comprehensive test-suite under `tests/` with expected outputs; `run_test.bat` for Windows test runs

### 1.1. Bug fixes that were just completed
- [x] Parser mis-parsing `pass` inside class bodies (fixed) — prevented creation of NULL field names.

### 1.2. Known correctness bugs
- [x] **Recursion collected active call frames.** Deep recursion (e.g. `fib(20)`
  and above) failed with a spurious `Undefined variable` error. Root cause: a
  call frame's `Env` chain lived only in a C local, so once recursion crossed the
  GC threshold the collector swept the live frame's bindings. Fixed by registering
  active environments as GC roots: `gc_push_env_root`/`gc_pop_env_root` (see
  `gc.c`), called in `exec_block` and at every call site in `interpreter.c`, and
  `gc_reset_roots` on the REPL's longjmp error-recovery path. Covered by
  `benchmarks/bench_fib.pith`.
  - Note: a narrower rooting gap remains — heap values already evaluated into the
    `args[]` array are not rooted while the remaining arguments are evaluated.
    Not yet observed to crash; close it when the call path is reworked into a
    single pre-sized frame (see Performance).

## 1.5. Performance

Completed and planned performance work is documented in detail in `PERFORMANCE.md`.
Benchmarks live in `benchmarks/`.

- [x] Build with `-O2` by default (CMake defaulted to `-O0`; ~2x on `bench_loop`)
- [x] Parse int/float/bool literals once at parse time, cache on the AST node
      (previously `atoi`/`atof`/`strcmp` ran on every evaluation)
- [x] Resolve operators to an `OpCode` enum at parse time; dispatch binary/unary
      ops with a `switch` instead of a `strcmp` chain
- [ ] Resolve variables to `(depth, slot)` indices; replace the per-binding
      linked-list `Env` with slot-based scopes (removes O(n) `strcmp` lookups,
      also fixes the recursion bug above)
- [ ] Immutable / interned strings so `value_copy` stops deep-copying on every read
- [ ] Store instance fields in flat slots instead of a string-keyed hashmap
- [ ] Use a single pre-sized call frame instead of one GC object per argument
- [ ] Resize/rehash hashmaps past a load factor (currently fixed at 16 buckets)

## 2. Medium Priority (Important improvements)

- [x] Runtime enforcement for `list<T>` element types (implemented)
  - Option A (runtime): store `value_type` in `List` and check on `append`, `insert`, and literal construction. Report a runtime type error on mismatch.
  - Option B (static): add a small static type checker that runs before interpretation and rejects incorrect list usage.
  - Question for you: Which option do you prefer? (You said you want to stay statically typed at the surface — do you want to enforce list element types at runtime first or add a static checker?)

- [x] Strengthen string handling and escapes
  - Properly support common escape sequences (`\n`, `\t`, `\\`, `\"`) in the tokenizer and parser.
  - Decide and document Unicode handling strategy (UTF-8 friendly operations vs full Unicode support).

- [ ] File API improvements
  - Add `io.open(path, mode)` returning a `File` object, `file.read()`, `file.readline()`, `file.write()`, `file.close()`.

- [ ] Better error messages and source-location reporting
  - Improve `report_error` to show source line and point to the offending token.

- [ ] GC stress tests and potential bug-hunt
  - Tests that create many objects, closures, and long-lived references to validate GC correctness.

## 3. Low Priority & Nice-to-Have

- [ ] Module system improvements
  - Add module search path configuration, package-like directories, and better error reporting on import failure.

- [ ] More stdlib (as-needed)
  - Add `datetime`, `os` wrappers, richer `str`/`list` utilities.

- [ ] Optional static type checker (longer term)
  - A separate `pithc` tool that performs static checks and emits warnings/errors before running the interpreter.

- [ ] CI integration (Windows runner)
  - Add a GitHub Actions workflow that builds on Windows and runs `run_test.bat` to validate regressions automatically.

## 4. Tests & QA
- [x] Existing test-suite covers arithmetic, control flow, classes, inheritance, stdlib functions, IO, lists, maps, and many features.
- [x] Add regression test that explicitly asserts `pass` inside classes does not create fields
- [ ] Add tests for error conditions: undefined variable, type mismatch, index out of bounds, division by zero
- [ ] Add tests to exercise GC (create many lists/instances and force collection)

## 5. Implementation & Code Hygiene
- [ ] Replace direct `realloc()` uses with a safe helper that preserves the old pointer on failure (address clang-tidy warnings)
- [ ] Audit memory ownership and `value_copy()` semantics to avoid double-free on some code paths
- [ ] Ensure `free_ast()` is always invoked after program execution to free parser allocations

