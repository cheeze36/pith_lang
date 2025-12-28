# Pith Language Coding Guidelines

This document outlines the coding standards and conventions for the Pith programming language project. Adhering to these guidelines ensures consistency, readability, and maintainability of the codebase.

## 1. General Philosophy

*   **Clarity over Cleverness**: Write code that is easy to read and understand. Avoid obscure C tricks unless absolutely necessary for performance.
*   **Consistency**: Follow the existing style of the project. If you edit a file, stick to the style used in that file.
*   **Documentation**: Public APIs and complex logic must be documented.

## 2. Naming Conventions

We use specific casing styles to distinguish between different kinds of identifiers.

| Entity | Style | Example |
| :--- | :--- | :--- |
| **Variables** | `snake_case` | `current_token`, `line_num`, `node` |
| **Functions** | `snake_case` | `parse_expression`, `gc_collect`, `create_node` |
| **Structs/Unions** | `PascalCase` | `ASTNode`, `ParserState`, `Value` |
| **Typedefs** | `PascalCase` | `TokenizerState`, `ObjHeader` |
| **Enums** | `PascalCase` | `TokenType`, `ASTNodeType` |
| **Enum Members** | `UPPER_SNAKE_CASE` | `TOKEN_IDENTIFIER`, `AST_BINARY_OP` |
| **Macros/Constants** | `UPPER_SNAKE_CASE` | `MAX_TEMP_ROOTS`, `DEBUG_TRACE_MEMORY` |
| **Global Variables** | `snake_case` | `global_env`, `native_string_methods` |

### Specific Rules
*   **Prefixes**: Use prefixes for "methods" acting on specific structs (pseudo-classes).
    *   Example: `list_add`, `env_define`, `hashmap_get`.
*   **Boolean**: Boolean variables should often start with `is_` or `has_` (e.g., `is_marked`, `is_fixed`).

## 3. Formatting

*   **Indentation**: Use **4 spaces**. Do not use tabs.
*   **Line Length**: Aim for a maximum of 100-120 characters per line.
*   **Braces**: Use the BSD (Allman) brace style. Opening braces go on the next line.
    ```c
    if (condition)
    {
        // code
    }
    else
    {
        // code
    }

    void function_name()
    {
        // code
    }
    ```
*   **Pointers**: The asterisk binds to the variable name, not the type (though the project currently has mixed usage, prefer `Type *var`).
    *   *Note: The existing code mostly uses `Type* var` or `Type *var`. Consistency within a file is key.*
*   **Switch Statements**: Indent `case` labels.
    ```c
    switch (type) {
        case VAL_INT:
            // code
            break;
        default:
            break;
    }
    ```

## 4. Comments and Documentation

We use Doxygen-style comments for documentation.

### File Headers
Every `.c` and `.h` file should start with a block comment describing its purpose.
```c
/**
 * @file filename.c
 * @brief Brief description of the file.
 *
 * Detailed description...
 */
```

### Function Documentation
Public functions (especially in headers) must be documented.
```c
/**
 * @brief Short description of what the function does.
 *
 * @param name Description of the parameter.
 * @return Description of the return value.
 */
int my_function(int name);
```

### Inline Comments
Use `//` for inline comments to explain *why* something is done, not *what* is done (unless the code is complex).
```c
// Good: Check if we need to trigger GC before allocation
if (bytes_allocated > threshold) ...

// Bad: Increment i
i++;
```

## 5. File Organization

*   **Header Files (`.h`)**:
    *   Must use include guards: `#ifndef PITH_FILENAME_H`, `#define PITH_FILENAME_H`, `#endif`.
    *   Should contain typedefs, struct definitions, and function prototypes.
    *   Avoid defining variables in headers (use `extern`).
*   **Source Files (`.c`)**:
    *   Include standard libraries first, then local headers.
    *   Order: Includes -> Macros -> Global Variables -> Forward Declarations -> Implementation.

## 6. Memory Management

The project uses a mix of manual memory management and a Garbage Collector (GC).

*   **Manual Management**:
    *   Used for internal parser/tokenizer structures (e.g., `ASTNode`, `Token` arrays).
    *   Use `malloc`, `realloc`, `calloc`, and `free`.
    *   Always check for `NULL` after allocation (or use a wrapper that exits on failure).
*   **Garbage Collection**:
    *   Used for runtime values (`Value`, `List`, `HashMap`, `PithInstance`).
    *   Use `allocate_obj(size, type)` instead of `malloc`.
    *   **Crucial**: If you allocate a GC object but haven't linked it to a root (like a variable in `env`) yet, you **must** push it to the temporary root stack using `gc_push_root()` to prevent it from being collected during subsequent allocations. Remember to `gc_pop_root()` afterwards.

## 7. Error Handling

*   **Runtime Errors**: Use `report_error(line, format, ...)` defined in `common.h`.
    *   In the REPL, this uses `longjmp` to recover.
    *   In file mode, this exits the program.
*   **Internal Errors**: For unrecoverable internal states (e.g., malloc failure), print to `stderr` and `exit(1)`.

## 8. Debugging

*   Use the macros defined in `debug.h` to guard debug print statements.
    ```c
    #ifdef DEBUG_TRACE_EXECUTION
    printf("[EXEC] ...");
    #endif
    ```
*   Do not leave raw `printf` statements in the code meant for production logic; wrap them in debug flags.

## 9. Type System

*   The `Value` struct (in `value.h`) is the core tagged union for dynamic typing.
*   Always check `Value.type` before accessing the union members.
*   Use `get_value_type_name(type)` for user-friendly type strings in error messages.
