# Benchmarks

Small, deterministic Pith programs used to measure interpreter performance.
See `../PERFORMANCE.md` for methodology and results.

Run one with:

```bash
/usr/bin/time -f "%e" ./build/pith_lang benchmarks/bench_loop.pith
```

| File             | What it stresses                                                        | Expected output |
|------------------|------------------------------------------------------------------------|-----------------|
| `bench_loop.pith`| Integer binary ops and variable access in a tight `while` loop (3M iterations). Exercises the literal-caching and operator-dispatch hot paths. | `8999994000000` |
| `bench_fib.pith` | Recursive calls, environment setup, and comparison ops via `fib(30)`.   | `832040`        |

`bench_fib.pith` also serves as the regression test for the recursion /
GC-rooting bug fixed in `gc.c` and `interpreter.c` (see `TODO.md`): it recurses
deeply enough to trigger GC cycles while many call frames are live.
