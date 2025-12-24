# Pith Language Project - TODO List

This document tracks the major features, bug fixes, and improvements for the Pith language.

## 1. High Priority & Core Features

-   **[x] Garbage Collector (GC):** Implemented a Mark-and-Sweep garbage collector.
    -   **Details:**
        1.  Defined `ObjHeader` and `ObjType` for all heap objects.
        2.  Implemented `allocate_obj` to track allocations.
        3.  Implemented `mark` phase starting from `global_env` and native registries.
        4.  Implemented `sweep` phase to free unmarked objects.
        5.  Added `free_all_objects()` for cleanup at exit.
    -   **Note:** Automatic collection during execution is currently disabled to avoid issues with C-stack roots. It can be enabled once a shadow stack or handle system is in place.

-   **[x] Multi-line Comments:** A common feature for documenting larger blocks of code.
    -   **Action:** Update the tokenizer (`tokenizer.c`) to recognize and ignore Pith-style multi-line comments (`### ... ###`).

-   **[x] Verify `input()` Function:** The native `input()` function has been tested via the `examples/library.pith` program.

## 2. Standard Library Expansion

-   **[x] `math` Module:** Added more native functions to the `math` module.
    -   **Functions added:** `abs`, `pow` (in Pith), `sin`, `cos`, `tan`, `floor`, `ceil`, `log` (native).

-   **[x] `io` and `sys` Modules:** Created new modules for file and system operations.
    -   **`io` functions:** `read_file(path)`, `write_file(path, content)`.
    -   **`sys` functions:** `exit(code)`.

-   **[ ] `string` and `list` Methods:** Implement the remaining methods listed in the design document.
    -   **`string`:** `upper()`, `lower()`, `replace()`, `startswith()`, `endswith()`, `contains()`.
    -   **`list`:** `pop()`, `remove(index)`, `insert(index, value)`, `clear()`.

## 3. Language Polish and Refinements

-   **[ ] Advanced File Handling:** Implement a more powerful, stream-based file API.
    -   **Action:** Introduce a `File` object type.
    -   **Functions:** `io.open(path, mode)`, `file.read()`, `file.readline()`, `file.write()`, `file.close()`.

-   **[ ] Runtime Type Enforcement for Lists:**
    -   **Action:** Currently, `list<int>` doesn't prevent appending a string. Update the `list_add` function and list literal creation logic in `interpreter.c` to perform runtime type checks based on a stored `value_type` in the `List` struct.

-   **[ ] Improved Error Reporting:**
    -   **Action:** Enhance the `report_error` function to optionally print the line of source code where the error occurred, providing more context to the user.

-   **[ ] AST Memory Management:**
    -   **Action:** The `main.c` file has a `TODO` for freeing AST nodes. Implement a recursive `free_ast(ASTNode* node)` function to walk the entire AST and free all nodes and their associated values after execution is complete.
