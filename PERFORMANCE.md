# Pith Performance Notes

This document records the performance work done on the Pith interpreter, how to
measure it, and the roadmap of remaining optimizations. Pith is a tree-walking
interpreter, so the hot path is `eval()`/`exec()` in `interpreter.c` and the
environment and GC machinery they lean on.

## How to benchmark

The `benchmarks/` directory holds small, deterministic Pith programs that stress
the interpreter's hot paths. See `benchmarks/README.md` for what each one covers.

Build an optimized binary and time a benchmark:

```bash
cmake -B build -S .          # defaults to a Release (-O2) build
cmake --build build
/usr/bin/time -f "%e" ./build/pith_lang benchmarks/bench_loop.pith
```

To compare against an unoptimized build:

```bash
cmake -B build_O0 -S . -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS_DEBUG="-O0"
cmake --build build_O0
```

Always confirm a change keeps the benchmark's printed output identical before
trusting a timing difference.

## Completed optimizations

### 1. Optimized build by default (`CMakeLists.txt`)
CMake builds with no optimizer (`-O0`) unless told otherwise. The build now
defaults to a `Release` configuration with `-O2 -DNDEBUG`. On `bench_loop.pith`
this alone roughly halved wall-clock time (~0.31s -> ~0.16s on the development
machine). A debug build is still available with `-DCMAKE_BUILD_TYPE=Debug`.

### 2. Literals parsed once, at parse time (`parser.c`, `interpreter.c`)
Previously `eval()` ran `atoi()`/`atof()`/`strcmp()` on the literal's source text
on *every* evaluation — so a literal inside a loop was re-parsed every iteration.
`ASTNode` now carries precomputed `int_value` / `float_value` fields
(see `parser.h`), populated once in `create_node()`. `eval()` reads the cached
value directly for `AST_INT_LITERAL`, `AST_FLOAT_LITERAL`, and `AST_BOOL_LITERAL`.

### 3. Operators dispatched by code, not by string (`parser.c`, `interpreter.c`)
Binary and unary operators were matched with a chain of up to ~12 `strcmp()`
calls per evaluation. The operator string is now resolved once to an `OpCode`
enum (see `parser.h`) in `create_node()`, and `eval()` dispatches with a
`switch` on `node->op` for both `AST_BINARY_OP` and `AST_UNARY_OP`.

## Remaining opportunities (roadmap)

These are larger, structural changes, ordered by expected payoff. They are
tracked in `TODO.md` under "Performance".

1. **Variable resolution to slots.** `Env` is a linked list where each binding
   is its own GC-allocated object with a `strdup`'d name (`value.h`,
   `env_define`/`env_get` in `interpreter.c`). Every lookup is an O(n) linear
   scan with `strcmp`. Resolving variables to `(depth, slot)` indices in a
   parse-time pass turns lookups into array indexing.

2. **Shared / interned strings.** `value_copy()` deep-copies strings on every
   variable read and assignment, so reading a string N times allocates N copies.
   Immutable, GC-managed (ideally interned) string objects would remove most of
   this churn.

3. **Instance fields in flat slots.** Instance fields live in a string-keyed
   `HashMap`, so every `obj.field` hashes and `strcmp`s. The class already knows
   its field layout; a `Value[]` indexed by resolved field slot removes the hash.

4. **Call frames.** The call path builds the argument scope by appending one GC
   object per argument and walking to the tail of the closure chain. A single
   pre-sized frame object removes both costs. (Arguments are already evaluated
   into a stack buffer for calls with up to 8 arguments, and are GC-rooted.)

5. **HashMap resizing.** `bucket_count` is fixed at 16 with no rehash, so large
   maps degrade to O(n) chains. Grow and rehash past a load factor (~0.75).

Done since this list was written: maps grow and rehash past a 3/4 load factor
(item 5); integer `^` uses exact exponentiation by squaring; and GC marking in
`gc.c` is iterative (a gray stack), so deeply nested data can no longer
overflow the C stack.
