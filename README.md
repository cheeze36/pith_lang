### **Pith Language: A Technical Overview**

Pith is an currently experintal imperative typed scripting language that blends syntax from C-family languages with Python-like structural conventions. It is executed by a tree-walking interpreter written in C, which processes a source file through a classic pipeline: **Tokenizer -> Parser (AST Builder) -> Interpreter**.

#### **1. Execution Model**

*   **Interpreter**: The core of the language is a C-based interpreter (`interpreter.c`) that recursively evaluates an Abstract Syntax Tree (AST). It supports two modes:
    1.  **File Mode**: Executes a `.pith` script file.
    2.  **REPL Mode**: An interactive Read-Eval-Print Loop is initiated when the interpreter is run without arguments.
*   **Compilation Pipeline**:
    1.  **Tokenizer (`tokenizer.c`)**: Scans the source text and converts it into a stream of tokens (e.g., identifiers, keywords, operators). It supports both single-line (`#`) and multi-line (`###...###`) comments.
    2.  **Parser (`parser.c`)**: Consumes the token stream to build an AST, which is a hierarchical tree representation of the code's structure.
    3.  **Interpreter (`interpreter.c`)**: Traverses the AST, executing statements and evaluating expressions.

#### **2. Syntax and Structure**

*   **C-Style Expressions**: Statements, declarations, and expressions are terminated and structured in a way that is familiar to C/Java developers (e.g., `int x = 10;`, `if (x > 5)`).
*   **Indentation-Based Blocks**: Like Python, Pith uses indentation to define the scope of blocks for control flow statements (`if`, `for`, `while`, etc.), functions, and classes. A block is introduced by a colon (`:`).
*   **Scoping**: Pith uses lexical (block) scoping. Variables declared within a block are not accessible outside of it.

#### **3. Data Types and Memory Management**

The language's type system is defined in `value.h` and is built around a `Value` tagged union.

*   **Primitive Types**:
    *   `int`: 32-bit signed integer.
    *   `float`: 32-bit floating-point number.
    *   `bool`: `true` or `false`.
    *   `string`: A sequence of characters.
    *   `void`: Represents the absence of a value, primarily for function return types.

*   **Reference Types (Heap-Allocated)**: These are managed by a Garbage Collector.
    *   `list<T>`: A dynamic, ordered collection.
    *   `T[size]`: A fixed-size array.
    *   `map<string, V>`: A hash map with string keys.
    *   `class`: User-defined types with fields and methods.
    *   `module`: A namespace for imported code.

*   **Garbage Collection (`gc.c`)**:
    *   **Algorithm**: Pith uses a **Mark-and-Sweep** garbage collector.
    *   **Mechanism**: All heap-allocated objects (`List`, `HashMap`, `Func`, `PithClass`, etc.) are prefixed with an `ObjHeader`. This header contains a type identifier, a `is_marked` flag for the GC cycle, and a pointer to form a linked list of all allocated objects.
    *   **Trigger**: The GC is not currently run automatically during execution. A full collection cycle is triggered explicitly at the end of the program's execution to prevent memory leaks.

#### **4. Key Language Features**

*   **Control Flow**: Supports a comprehensive set of control flow statements: `if/elif/else`, `while`, `do-while`, `foreach`, C-style `for`, and `switch` (with fall-through behavior).
*   **Functions**: Defined with the `define` keyword, requiring explicit type annotations for parameters and return values.
*   **Classes**: A simple object-oriented model is supported.
    *   Declared with the `class` keyword.
    *   Fields are declared at the top of the class body.
    *   A special `init` method serves as the constructor.
    *   Instances are created with the `new` keyword, and members are accessed via the `.` operator.
*   **Module System**:
    *   The `import "foo"` statement loads code from `stdlib/foo.pith` or a local `foo.pith` file.
    *   The imported content is encapsulated within a `module` object, providing a namespace (e.g., `math.sqrt(16)`).

*   **Standard Library**: A small but functional standard library provides built-in capabilities:
    *   **Global Functions**: `print()`, `input()`, `clock()`.
    *   **Native Methods**: `string` and `list` types have built-in methods like `len()`, `append()`, and `trim()`.
    *   **Modules**:
        *   `math`: Mathematical functions (`sqrt`, `sin`, `abs`, etc.).
        *   `io`: File I/O (`read_file`, `write_file`).
        *   `sys`: System operations (`exit`).

#### **5. Testing and Debugging**

*   **Test Suite**: The project includes a test suite (`tests/`) where `.pith` files are executed and their `stdout` is compared against `.expected` files. A batch script (`run_test.bat`) automates this process.
*   **Debugging Traces**: The interpreter is instrumented with an extensive set of compile-time debug flags (in `debug.h`) that can trace tokenizer, parser, and interpreter execution, providing deep insight into the runtime behavior for debugging purposes.
