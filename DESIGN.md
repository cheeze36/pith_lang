# Pith Language Specification

This document serves as the official specification for the Pith programming language.

## 1. General Syntax

### 1.1. Comments
Pith supports both single-line and multi-line comments.

**Single-line comments** start with the `#` symbol. All text from the `#` to the end of the line is ignored by the tokenizer.

```pith
# This is a full-line comment.
int x = 10 # This is also a valid comment.
```

**Multi-line comments** are enclosed in `###` markers. Everything between the opening `###` and the closing `###` is ignored, spanning multiple lines if necessary.

```pith
###
This is a multi-line comment.
It can span multiple lines.
###
print("Hello") ### This is also a valid multi-line comment ###
```

### 1.2. Blocks and Indentation
Pith uses indentation to define code blocks. A block is started with a colon (`:`) and a newline, followed by an indented section of code. The block ends when the indentation returns to the previous level.

```pith
if (x > 5):
    print("x is greater than 5") # This is inside the if-block
print("This is outside the if-block")
```

## 2. Data Types

### 2.1. Primitive Types
Pith has a set of primitive data types that are passed by value.

- `int`: A 32-bit signed integer.
- `float`: A 32-bit floating-point number.
- `bool`: A boolean value, either `true` or `false`.
- `string`: A sequence of characters.
- `void`: A special type representing the absence of a value, used for function return types. Variables cannot be declared as `void`.

### 2.2. Reference Types
These types are handled by reference (or pointer). Assigning them or passing them to functions copies the reference, not the underlying data.

- `list<T>`: A dynamic, ordered collection of elements of type `T`. **Note: Type enforcement for list elements is not yet implemented.**
- `T[size]`: A fixed-size array of `size` elements of type `T`.
- `map<K, V>`: A collection of key-value pairs. Keys (`K`) must be strings, and values (`V`) are strongly typed.
- `class`: A user-defined data structure containing fields and methods.
- `module`: A container for functions and variables loaded via the `import` statement.

## 3. Variables and Scope
Variables are declared with their type. Pith uses lexical (or block) scoping. A variable declared inside a block (like a loop or `if` statement) is only accessible within that block.

```pith
int x = 10 # Global scope

if (x == 10):
    int y = 20 # Local to the if-block
    print(y)

# print(y) # This would cause an "Undefined variable" error.
```

## 4. Operators

- **Arithmetic**: `+`, `-`, `*`, `/`, `%` (modulo), `^` (power)
- **Comparison**: `==`, `!=`, `<`, `>`, `<=`, `>=`
- **Logical**: `and`, `or`, `!` (not)

## 5. Control Flow

### 5.1. `if / elif / else`
Pith supports standard conditional statements.

```pith
if (x < 10):
    print("x is small")
elif (x < 100):
    print("x is medium")
else:
    print("x is large")
```

### 5.2. Loops

#### `while` Loop
Executes a block as long as a condition is true.
```pith
int i = 0
while (i < 3):
    print(i)
    i = i + 1
```

#### `do-while` Loop
Executes a block once, and then continues as long as a condition is true.
```pith
int j = 0
do:
    print(j)
    j = j + 1
while (j < 3)
```

#### `foreach` Loop
Iterates over the elements of a list or array.
```pith
list<int> my_list = [10, 20, 30]
foreach (int item in my_list):
    print(item)
```

#### C-Style `for` Loop
A traditional `for` loop with an initializer, a condition, and an increment expression. The initializer creates a new scope for the loop variable.
```pith
for (int i = 0; i < 5; i = i + 1):
    print(i)
```

### 5.3. `switch` Statement
Evaluates an expression and executes the block of the first matching `case`. If no `break` statement is used, execution will "fall through" to subsequent cases.

```pith
switch(x):
    case 1:
        print("one")
        break
    case 2:
    case 3:
        print("two or three")
        break
    default:
        print("other")
```

### 5.4. Control Flow Keywords
- `break`: Immediately exits the innermost loop (`while`, `do-while`, `for`, `foreach`) or `switch` statement.
- `continue`: Skips the rest of the current loop iteration and proceeds to the next one.
- `pass`: A null operation. It does nothing and can be used as a placeholder.

## 6. Collections

### 6.1. Dynamic Lists
A resizeable, ordered collection.
- **Declaration**: `list<type> name = [value1, value2, ...]`

### 6.2. Fixed-Size Arrays
An array with a size known at compile time.
- **Declaration**: `type[size] name`

### 6.3. Hashmaps
A strongly-typed collection of key-value pairs. Keys must be strings.
- **Declaration**: `map<string, type> name = {key1: value1, ...}`

## 7. Functions
Functions are defined using the `define` keyword. Return types and parameter types are required.

```pith
define int add(int a, int b):
    return a + b

define void say_hello():
    print("Hello!")
```

## 8. Built-in IO

### 8.1. `print` Statement
The `print` statement evaluates one or more expressions, converts them to strings, and prints them to the standard output, followed by a newline. Multiple arguments are separated by a single space.

```pith
print("Hello, world!")
print("The value of x is:", 10)
```

### 8.2. `input()` Function
The `input()` function is a built-in native function that reads a line of text from the standard input and returns it as a string. It can optionally take a string argument to use as a prompt.

```pith
string name = input("Enter your name: ")
print("Hello,", name)
```

## 9. Classes
Classes group data (fields) and functions that operate on that data (methods).

- **Declaration**: Use the `class` keyword.
- **Fields**: Must be declared with their type at the top of the class body.
- **Constructor**: A special method named `init` is called when a new instance is created.
- **Methods**: Functions defined within a class.
- **Instantiation**: Use the `new` keyword.
- **Access**: Use the `.` operator to access fields and methods. The `this` keyword refers to the current instance.

```pith
class Counter:
    int count

    define init():
        this.count = 0

    define void increment():
        this.count = this.count + 1

Counter c = new Counter()
c.increment()
```

## 10. Modules and Standard Library

### 10.1. `import` System
Pith uses a namespaced module system. `import "foo"` loads the code from `stdlib/foo.pith` (or `foo.pith`) into a `module` object named `foo`.

```pith
import "math"
print(math.sqrt(16)) # Access sqrt from the math module
```

### 10.2. Global Native Functions
A small number of native functions are available in the global scope without an import.
- `clock()`: Returns the number of seconds since the program started.

### 10.3. Native Methods
Some built-in types have native methods that can be called with the `.` operator.

- **`string` Methods**:
    - `len()`: Returns the length of the string.
    - `trim()`: Returns a new string with leading/trailing whitespace removed.
    - `split(delimiter)`: Returns a `list<string>` split by the delimiter.
    - `upper()`: (Not Yet Implemented)
    - `lower()`: (Not Yet Implemented)
    - `replace(find, replace)`: (Not Yet Implemented)
    - `startswith(prefix)`: (Not Yet Implemented)
    - `endswith(suffix)`: (Not Yet Implemented)
    - `contains(substring)`: (Not Yet Implemented)

- **`list` Methods**:
    - `len()`: Returns the number of elements in the list.
    - `append(item)`: Adds an item to the end of a dynamic list.
    - `join(delimiter)`: Joins the list of strings into a single string.
    - `pop()`: (Not Yet Implemented)
    - `remove(index)`: (Not Yet Implemented)
    - `insert(index, value)`: (Not Yet Implemented)
    - `clear()`: (Not Yet Implemented)

### 10.4. Standard Modules
- **`math`**: Provides mathematical functions.
    - `sqrt(n)`
    - `sin(n)`
    - `cos(n)`
    - `tan(n)`
    - `abs(n)`
    - `pow(base, exp)`
    - `floor(n)`
    - `ceil(n)`
    - `log(n)`
- **`io`**: Provides basic file input/output operations.
    - `read_file(path)`: Reads an entire file into a string. Returns `void` if the file cannot be read.
    - `write_file(path, content)`: Writes a string to a file, overwriting it. Returns `true` on success and `false` on failure.
- **`sys`**: Provides access to system-specific functions.
    - `exit(code)`: Terminates the program with the given integer exit code.

## 11. The REPL (Read-Eval-Print Loop)
Running the interpreter with no arguments starts the interactive REPL.

- **Multi-line Input**: The REPL automatically detects incomplete statements (like those ending in `:`) and allows for multi-line input. A blank line finishes the block.
- **Error Handling**: Runtime errors are caught and displayed without terminating the REPL session.
- **Expression Printing**: If a line of input evaluates to an expression, the result is automatically printed.

## 12. Debugging System
The interpreter includes a debugging system controlled by flags in `debug.h`. To use it, uncomment the desired flag in `debug.h` and recompile.

### 12.1. `DEBUG_TRACE_TOKENIZER`
Traces the execution of the tokenizer, showing each character as it is processed. Useful for debugging syntax errors at the lowest level.
- `[TOKENIZER]`: Shows the character being processed.

### 12.2. `DEBUG_DEEP_DIVE_PARSER`
Traces the creation and linking of Abstract Syntax Tree (AST) nodes during the parsing phase.
- `[DDP_CREATE]`: Logs the creation of a new AST node, its type, value, and memory address.
- `[DDP_LINK]`: Shows how a child node is linked to a parent node, visualizing the tree structure.

### 12.3. `DEBUG_DEEP_DIVE_INTERP`
Traces the execution of the interpreter as it walks the AST. This is the most common and useful flag for debugging runtime logic.
- `[DDI_EVAL]`: Shows when the `eval` function is entered for a specific AST node.
- `[DDI_EXEC]`: Shows when the `exec` function is entered for a specific AST node.
- `[DDI_ENV]`: Traces environment operations: defining, assigning, and getting variables.
- `[DDI_BLOCK]`: Logs when the interpreter enters or exits a new block scope.
- `[DDI_BINOP]`: Details the evaluation of a binary operation, showing the left and right operands and the result.
- `[DDI_IMPORT]`: Logs the start of a module import.
- `[DDI_MODULE_ACCESS]`: Logs when a member of a module is accessed.
- `[DDI_CLASS_DEF]`: Logs when a class definition is being processed.
- `[DDI_CLASS_METHOD]`: Logs when a method is being attached to a class.
- `[DDI_NATIVE_METHOD]`: Logs when a native C method (like `len()` or `append()`) is called.
- `[DDI_ARRAY_INIT]`: Logs the initialization of a fixed-size array.
- `[DDI_ASSIGN_INDEX]`: Logs an assignment to a list, array, or hashmap index.
- `[DDI_FOR_LOOP]`: Traces the execution of a C-style `for` loop's components (initializer, condition, increment).
- `[DDI_FOREACH_LOOP]`: Traces the iteration of a `foreach` loop, showing the loop variable being defined.
- `[DDI_REPL]`: Provides logs specific to the REPL's execution, such as the buffer being executed or the decision to print an expression.

## 13. Memory Management
Pith uses a Mark-and-Sweep Garbage Collector (GC) to manage heap-allocated memory.

### 13.1. Managed Objects
The following types are managed by the GC:
- `List`
- `HashMap`
- `Func` (Function closures)
- `Module`
- `PithClass`
- `PithInstance`
- `BoundMethod`
- `StructDef`
- `StructInstance`
- `Env` (Environment scopes)

### 13.2. Object Header
Every heap-allocated object is prefixed with an `ObjHeader` structure containing:
- `type`: The type of the object (used for traversal).
- `is_marked`: A flag used during the mark phase.
- `next`: A pointer to the next object in the global linked list of all allocated objects.

### 13.3. Garbage Collection Cycle
The GC cycle consists of two phases:
1.  **Mark Phase**: The GC starts from a set of "roots" (currently the global environment and native registries) and recursively marks all reachable objects by setting their `is_marked` flag to true.
2.  **Sweep Phase**: The GC iterates through the global list of all objects. Any object with `is_marked` set to false is considered unreachable and is freed. Objects with `is_marked` set to true are kept, and their flag is reset for the next cycle.

### 13.4. Current Limitations
-   **Automatic Collection**: Currently, the GC does not run automatically during execution to avoid collecting active objects referenced only by the C stack (which are not yet tracked as roots).
-   **Cleanup**: A full GC cycle is triggered at the end of the program execution to free all allocated memory, preventing memory leaks.
