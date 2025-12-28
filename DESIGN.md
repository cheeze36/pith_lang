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
String literals are delimited with double-quotes (`"...")`). Common escape sequences are recognized and translated by the tokenizer: `\n`, `\t`, `\\`, `\"`, `\r`.

### 1.3. Blocks and Indentation
Pith uses indentation to define code blocks. A block is started with a colon (`:`) and a newline, followed by an indented section. A block ends when the indentation returns to the previous level.

```pith
if (x > 5):
    print("x is greater than 5")
print("Outside the if-block")
```

Note: the tokenizer emits `INDENT` / `DEDENT` tokens for block handling. The parser accepts `pass` as a no-op statement which is commonly used inside empty class bodies.

## 2. Data Types

### 2.1. Primitive Types
Pith currently exposes the following primitive types (value semantics):

- `int`: 32-bit signed integer
- `float`: 32-bit floating-point number
- `bool`: boolean (`true` / `false`)
- `string`: heap-allocated C string
- `void`: absence of a value (used for function return type or non-value)

### 2.2. Reference / Heap Types
Reference (heap-allocated) types are managed by the runtime and passed by reference (assigning or passing copies the reference):

- `list<T>`: dynamic, ordered collection. The syntax `list<int> my_list = [...]` is used; at present, element type enforcement is not enforced at compile-time. Runtime enforcement for `list<T>` operations is on the TODO list.
- `T[size]`: fixed-size arrays (e.g., `int[3] arr`) created with a compile-time size expression.
- `map<string, V>`: hashmap keyed by strings, with a value type `V`. Keys must be strings.
- `class`: user-defined class definitions with fields and methods.
- `module`: namespaces created by `import`.

Notes and current limitations:
- `list<T>` element type enforcement is not yet implemented — you can currently append any `Value` to a list created with a `list<T>` annotation. See TODO for plans to enforce runtime checks (or to add a static type checker).
- `map<K, V>` requires `K` to be `string` in the current implementation.

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

- Arithmetic: `+`, `-`, `*`, `/`, `%` (mod), `^` (power)
- Comparison: `==`, `!=`, `<`, `>`, `<=`, `>=`
- Logical: `and`, `or`, `!` (not)

Type behavior is strict: arithmetic expects numeric operands; comparisons require type-compatible operands (strings compare by content, ints/floats by numeric value).

## 5. Control Flow

- `if / elif / else` with indentation blocks.
- `while`, `do : ... while (cond)`
- `foreach (T x in collection):` iterates lists/arrays.
- C-style `for (init; cond; inc):` supported and creates its own loop scope.
- `switch(expr): case ...` supports fall-through unless `break` is used.
- `break`, `continue`, and `pass` behave as expected.

## 6. Built-in IO and Stdlib

- Global natives: `print()`, `input()`, `clock()`, `isinstance()`
- Modules in `stdlib/` are importable with `import "name"`. The interpreter will first try `stdlib/name.pith` then `name.pith` in the working directory.

Native modules provided:
- `math`: `sqrt`, `sin`, `cos`, `tan`, `abs`, `pow`, `floor`, `ceil`, `log`
- `io`: `read_file(path)`, `write_file(path, content)` (note: `read_file` returns `void` on failure)
- `sys`: `exit(code)`
- `str`: `replace`, `startswith`, `endswith`, `contains`, `trim`, `upper`, `lower`, `split`, `len`
- `list` native methods: `len`, `append`, `join`, `pop`, `remove`, `insert`, `clear`

Note: native methods are implemented in C and available via field access on string/list values (e.g., `"a,b".split(",")`, `my_list.append(1)`).

## 9. Classes

- Declared with `class Name:`. Fields are declared at the top of the body, methods with `define`.
- `init` is treated as the constructor and is invoked by `new Name(...)` during instance creation; fields are initialized to `void` by default.
- Inheritance is supported with `class Child extends Parent:` — parent methods and fields are copied to the child class at definition time.
- `isinstance(obj, Class)` is provided as a native function which walks the instance's class chain to check membership.
- `this` is available inside methods as the instance receiver.

## 10. REPL and Execution

- Running `pith` with no arguments starts the REPL. `pith -i script.pith` runs the script then drops into REPL with the script's environment preserved.
- The REPL supports multi-line statements (blocks) and prints the value of expressions automatically.
- Runtime errors are reported via `report_error` and typically terminate execution when running a file; in the REPL they are shown without exiting the session.

## 11. Debugging

- `debug.h` contains compile-time flags to trace tokenizer, parser, interpreter, environment ops, memory events, native calls, and module imports. Enable these for deep tracing during development.

## 12. Memory Management

- Pith uses a mark-and-sweep GC for heap-managed types (lists, maps, functions, modules, classes, instances, bound methods, env nodes).
- Objects are allocated via `allocate_obj` which attaches an `ObjHeader` used by the GC.
- The interpreter uses a temporary root stack (via `gc_push_root` / `gc_pop_root`) to protect temporaries on the C stack during allocations and evaluation.

---

This specification is intended to track actual implemented behavior. For language-design decisions that would change syntax or typing behavior, please review and approve the proposed syntax changes in the TODO section.
