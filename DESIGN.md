# Pith Language Specification

This document is the working specification for the Pith programming language. It describes the syntax, core types, runtime model, and notable implementation behaviors as of the current implementation.

## 1. General Syntax

### 1.1. Comments
Pith supports both single-line and multi-line comments.

- Single-line comments: `#` (standardized). All text from the comment marker to the end of the line is ignored by the tokenizer.

```pith
# This is a full-line comment
int x = 10 # trailing comment
```

- Multi-line comments are enclosed in `###` markers. Everything between the opening `###` and the closing `###` is ignored by the tokenizer (can span multiple lines).

```pith
###
This is a multi-line comment.
It can span multiple lines.
###
print("Hello") ### inline multi-line comment ###
```

### 1.2. Strings and escape sequences
String literals are delimited with double-quotes (`"..."`). The tokenizer translates these escape sequences; any other escape is a syntax error:

| Escape | Meaning |
| :--- | :--- |
| `\n` `\t` `\r` | newline, tab, carriage return |
| `\0` | NUL byte |
| `\a` `\b` `\f` `\v` | bell, backspace, form feed, vertical tab |
| `\e` | escape (27), for terminal colour codes |
| `\\` `\"` `\'` | backslash, double quote, single quote |
| `\xHH` | the byte with hex value `HH` (exactly two digits) |
| `\u{H...}` | the Unicode code point `H...` (1-6 hex digits), encoded as UTF-8 |

**Raw strings** keep every character exactly as written (no escapes), which suits regex patterns and Windows paths. The value is an ordinary `string`:

```pith
string number = r"\d+\.\d+"          # backslashes are literal
string path   = r"C:\Users\pith"
string quoted = r#"say "hi" \d"#       # add #s to include a " (r##"..."## for "# inside)
```

A `string` is an **immutable sequence of bytes**. It can hold any data, including NUL bytes, so binary content (files read with `io.read_file`, HTTP bodies) works. Text is conventionally UTF-8.

```pith
string s = "héllo"
s.len()       # 6  - bytes ("é" is two bytes in UTF-8)
s.count()     # 5  - characters (UTF-8 code points)
s[0]          # "h"   - one byte; negative indexes count from the end (s[-1] is "o")
s[1:3]        # "é"   - bytes 1..2; either bound may be omitted: s[:2], s[2:]
s[-3:]        # "llo" - negative bounds count from the end; out-of-range bounds are clamped
```

- Indexing and slicing count bytes, like `len()`. An out-of-range index is an error; slice bounds are clamped.
- Strings cannot be modified (`s[0] = "x"` is an error); build a new string with slicing and `+`.
- `split("")` splits into characters (UTF-8 code points), matching `count()`.
- `s.repeat(n)` repeats a string (`"-".repeat(40)`).
- **Building long strings:** because strings are immutable, `result = result + piece` in a loop copies the whole string every time (quadratic time for long results). Collect the pieces in a `list<string>` and call `pieces.join("")` once, or use `repeat()`.
- Map keys are text: a key containing a NUL byte is truncated at the NUL.

### 1.3. Blocks and Indentation
Pith uses indentation to define code blocks. A block is started with a colon (`:`) and a newline, followed by an indented section. A block ends when the indentation returns to the previous level.

**Line continuation:** while a `(`, `[` or `{` is open, line breaks are ignored, so long calls, lists, maps and conditions can span several lines (indentation inside them does not matter):

```pith
print(hex_is(crypto.sha256("abc"),
             "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"))
map<string, int> ages = {
    "ann": 31,
    "bob": 42
}
```

```pith
if (x > 5):
    print("x is greater than 5")
print("Outside the if-block")
```

Note: the tokenizer emits `INDENT` / `DEDENT` tokens for block handling. The parser accepts `pass` as a no-op statement which is commonly used inside empty class bodies.

## 2. Data Types

### 2.1. Primitive Types
Pith currently exposes the following primitive types (value semantics):

- `int`: 64-bit signed integer. Arithmetic is checked: a result that does not fit raises "Integer overflow" instead of wrapping. Integer `^` is exact (repeated squaring); a negative exponent needs a float base.
- `float`: 64-bit IEEE double. Literals may use exponents (`1e300`, `2.5E-3`).
- `bool`: boolean (`true` / `false`)
- `string`: immutable, garbage-collected byte string with a stored length (see 1.2)
- `void`: absence of a value (used for function return type or non-value)

### 2.2. Reference / Heap Types
Reference (heap-allocated) types are managed by the runtime and passed by reference (assigning or passing copies the reference):

- `list<T>`: dynamic, ordered collection, e.g. `list<int> nums = [1, 2]`. Bare `list` accepts any elements.
- `T[size]` / `T[]`: a fixed-size list of `size` zero values (e.g. `int[3] arr`), or an empty growable one.
- `map<string, V>`: hashmap keyed by strings with values of type `V`. Bare `map` accepts any values. Maps remember insertion order.
- Class names (e.g. `Point p = Point(1, 2)`): instances of that class or a subclass.
- Native types provided by modules (e.g. `Regex`, `Match` from `regex`): C-backed objects that behave like class instances: declared by name, methods via `.`, nullable (`void`), compared by identity, freed by the GC. See `native_object.h`.
- `module`: namespaces created by `import`.

### 2.3. Type enforcement
Types are checked at runtime wherever a value is stored under a declared type: variable declarations and assignments, function arguments and return values, class fields, list elements (`append`, `insert`, `xs[i] = v`), map values, and `foreach` variables. A mismatch is a (rescuable) error, e.g. `Type mismatch: variable 'n' expects int, got string.`

- **No implicit conversions**, with one exception: an `int` is accepted where a `float` is expected and widens to a float (`float f = 5`, `2.5 * n`). A `float` never silently becomes an `int`, and nothing ever becomes a string.
- **Explicit conversions:** `str(x)` (int, float, bool, string -> text), `int(x)` (float truncates toward zero; strings must be a whole integer like `"-42"`), `float(x)` (int or numeric string). Invalid input such as `int("abc")` raises an error. (`integer.toString` / `integer.fromString` remain for compatibility.)
- **`void` ("no object") is allowed only in class-typed variables and fields**, e.g. `Node next` starts as `void`; test with `next == void`. All other types can never hold void.
- **Zero values:** a declaration without `=` gets `0`, `0.0`, `""`, `false`, an empty list/map, or `void` for class types. Class fields start at the zero value of their declared type.
- **Containers:** an untyped list/map (a literal, `json.decode(...)`, ...) is checked element by element when first stored under a typed name and then adopts that element type. A list that already has a different element type is rejected (`list<string> s = ints` is an error).
- **Functions:** parameters and return types are optional; when written they are enforced, and so is the number of arguments. A function declared with a non-void return type must return a value; a `void` function must not.
- **Classes:** assigning to an undeclared field (`obj.missing = 1`) is an error.
- **Maps:** reading a missing key (`m["nope"]`) is an error; use `m.has(k)` or `m.get(k, default)`.
## 3. Variables and Scope

- Variables are declared with an explicit type: `int x = 10`. Pith uses lexical (block) scoping. Variables declared inside a block are not visible outside it.

```pith
int x = 10
if (x == 10):
    int y = 20
    print(y)
# print(y) would be an undefined-variable error
```

## 4. Operators

- Arithmetic: `+`, `-`, `*`, `/`, `%` (mod), `^` (power) on numbers. `int` with `int` gives `int` (`/` is integer division); any `float` operand gives a `float`.
- `+` on two strings concatenates. Mixing a string with a number is an error: use `"n=" + str(n)`.
- Comparison: `<`, `>`, `<=`, `>=` on numbers, or on two strings (byte-wise lexicographic order).
- Equality: `==`, `!=` compare numbers by value, strings by content, bools by value, and lists/maps/instances by identity. Anything compared with `void` is equal only to `void`. Comparing other mismatched types (`1 == "1"`) is an error rather than silently `false`.
- Logical: `and`, `or`, `!` take bools only. `and`/`or` short-circuit: the right side is evaluated only if the left side does not decide the result, so `s.len() > 0 and s[0] == "/"` is safe.

## 5. Control Flow

- `if / elif / else` with indentation blocks.
- `while`, `do : ... while (cond)`
- Conditions (`if`, `elif`, `while`, `for`, `do-while`) must be `bool`; `if (1):` is an error.
- `foreach (T x in collection):` iterates a list/array, or the keys of a map (in insertion order).
- C-style `for (init; cond; inc):` supported and creates its own loop scope.
- `switch(expr): case ...` supports fall-through unless `break` is used.
- `break`, `continue`, and `pass` behave as expected.

## 6. Built-in IO and Stdlib

- Global natives: `print()`, `input()`, `clock()`, `isinstance()`, `str()`, `int()`, `float()`
- `import "name"` loads the native module `name` (if there is one) together with `name.pith`, searched for in: the standard library directory (the `PITH_STDLIB` environment variable, or the source tree's `stdlib/` recorded at build time), then the running script's directory, then the current directory. Importing a module that does not exist is an error, and so is using a member a module does not have (`Module 'math' has no member 'nope'.`).
- Native type names (`Regex`, `Match`, ...) are reserved: a `class` with the same name is an error.
- Recursion depth is limited by the C stack (about 8,000 nested calls with the usual 8 MB stack); going deeper raises a rescuable "Stack overflow" error instead of crashing.

Native modules provided:
- `math`: `sqrt`, `sin`, `cos`, `tan`, `abs`, `pow`, `floor`, `ceil`, `log`, `min`, `max`, `round`, `clamp`, `PI`, `E`
- `random`: `seed`, `int`, `float`, `choice`, `shuffle`
- `io`: files (`read_file`, `read_lines`, `write_file`, `append_file`, `copy_file`, `rename`, `remove`), queries (`exists`, `is_file`, `is_dir`, `file_size`, `modified_time`), directories (`list_dir`, `make_dir`, `temp_dir`) and paths (`join_path`, `basename`, `dirname`, `extension`, `absolute_path`). Failures raise rescuable errors with the OS reason. See `lib/io_module.h`.
- `sys`: `args`, `script`, `env(name [, default])`, `has_env`, `set_env`, `cwd`, `chdir`, `platform`, `exit`. Script arguments follow the script name: `pith_lang app.pith 8080`. See `lib/sys_module.h`.
- `time`: `sleep`, `sleep_ms`, `now`, `millis`, `perf`, `format` (see `lib/time_module.h`)
- `http`: `listen`, `route`, `static`, `serve`, `stop`, `accept`, `respond`, `header`, `param`, `close`, `parse_query`, `url_decode`, `url_encode` (see `lib/http_module.h`)
- `json`: `encode`, `decode` (see `lib/json_module.h`)
- `encoding`: `base64_encode`, `base64_decode`, `base64url_encode`, `base64url_decode`, `hex_encode`, `hex_decode` (see `lib/encoding_module.h`)
- `crypto`: secure randomness (`random_bytes`, `random_token`, `random_int`), `sha256`, `hmac_sha256`, `timing_safe_equal`, passwords (`hash_password`, `verify_password`), `pbkdf2_sha256`. Results are raw bytes; use `encoding` to print/store them. See `lib/crypto_module.h`.
- `regex`: `compile(pattern)` -> `Regex` (`test`, `find` -> `Match` or void, `find_all`, `replace`, `split`), `escape`. Linear-time engine (no backreferences/lookaround). See `lib/regex_module.h`.
- `integer`: `toString`, `fromString`

Native methods (called on values):
- string: `len` (bytes), `count` (characters), `trim`, `upper`, `lower`, `split`, `startswith`, `endswith`, `contains`, `find`, `replace`, `repeat`
- list: `len`, `append`, `join`, `pop`, `remove`, `insert`, `clear`
- map: `len`, `has(key)`, `get(key, default)`, `keys()`, `values()`, `remove(key)`

## 9. Classes

- Declared with `class Name:`. Fields are declared at the top of the body, methods with `define`.
- `init` is treated as the constructor and is invoked by `new Name(...)` during instance creation; fields are initialized to `void` by default.
- Inheritance is supported with `class Child extends Parent:` — parent methods and fields are copied to the child class at definition time.
- `isinstance(obj, Class)` is provided as a native function which walks the instance's class chain to check membership.
- `this` is available inside methods as the instance receiver.

## 10. REPL and Execution

- Running `pith` with no arguments starts the REPL. `pith -i script.pith` runs the script then drops into REPL with the script's environment preserved.
- The REPL supports multi-line statements (blocks) and prints the value of expressions automatically.
- Runtime errors are reported via `report_error` and terminate execution when running a file, unless they are rescued (see below); in the REPL they are shown without exiting the session.

### Error handling: try / rescue / else / finally

```pith
try:
    int ratio = total / count
rescue e:                # e is the error message (string); `rescue:` ignores it
    print("failed: " + e)
else:                    # only if the body raised nothing
    print("ok")
finally:                 # always runs
    print("done")

raise "something went wrong"   # raise your own error (string message)
```

- Every runtime error is rescuable: errors from the interpreter (`Division by zero.`, type errors, undefined variables), from native modules, and from `raise`.
- A `try` needs at least one `rescue` or `finally`; `else` requires `rescue`.
- `finally` runs on success, after a rescued error, before an unrescued error propagates, and on `return`/`break`/`continue`. If `finally` itself returns/breaks/continues, that wins and any pending error is discarded.
- Errors raised inside `rescue` or `else` are not caught by the same `try`; they propagate after its `finally`. Re-raise with `raise e`.
- The rescue variable is scoped to the rescue block.
- Implementation: each active `try` pushes a `TryFrame` (a `jmp_buf`); `report_error` longjmps to the innermost one, which restores the GC root stacks and the current environment (see `exec_try` in interpreter.c).

## 11. Debugging

- `debug.h` contains compile-time flags to trace tokenizer, parser, interpreter, environment ops, memory events, native calls, and module imports. Enable these for deep tracing during development.

## 12. Memory Management

- Pith uses a mark-and-sweep GC for heap-managed types (strings, lists, maps, functions, modules, classes, instances, bound methods, env nodes).
- Objects are allocated via `allocate_obj`, which zeroes them and attaches an `ObjHeader` used by the GC.
- **Rooting rule:** any heap value held only in a C local must be on the temporary root stack across any call that may allocate. Use `gc_push_value(v)` / `gc_pop_root()`, or record `gc_temp_root_depth()` and release with `gc_pop_roots_to(depth)`. The interpreter already roots binary-operator operands, call arguments and callees, index/slice subjects, assignment values, `foreach` collections and `switch` subjects, so native functions may assume their arguments stay alive; natives that allocate several objects must root their own intermediates (see `native_string_split`).
- Debug aid: build with `-DDEBUG_STRESS_GC` (see `debug.h`) to collect on every allocation; combined with AddressSanitizer, a missing root shows up immediately as a use-after-free.

---

This specification is intended to track actual implemented behavior. For language-design decisions that would change syntax or typing behavior, please review and approve the proposed syntax changes in the TODO section.
