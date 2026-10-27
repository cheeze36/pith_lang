/**
 * @file interpreter.h
 * @brief Header file for the Pith language interpreter.
 *
 * Defines the public interface for the interpreter, including the main entry point `interpret`,
 * environment management, and native function registration.
 */

#ifndef PITH_INTERPRETER_H
#define PITH_INTERPRETER_H

#include <stddef.h>
#include "parser.h"
#include "value.h"
#include "common.h" // Include the new common header for error reporting

/**
 * @brief Interprets and executes the given Abstract Syntax Tree (AST).
 *
 * This is the main entry point for running a Pith program.
 *
 * @param root The root node of the AST.
 */
void interpret(ASTNode *root);

/**
 * @brief Reads the entire content of a file into a string.
 *
 * @param filename The path to the file.
 * @return A dynamically allocated string containing the file content, or NULL on failure.
 */
char *read_file_content(const char *filename);

/**
 * @brief Reads an entire file, preserving every byte (binary-safe).
 * @param filename The path to the file.
 * @param length_out If non-NULL, receives the number of bytes read.
 * @return A malloc'd buffer (bytes plus a terminating NUL), or NULL on failure.
 */
char *read_file_bytes(const char *filename, long *length_out);

// --- Global State ---

/**
 * @brief The global environment containing global variables and functions.
 */
extern Env *global_env;

/**
 * @brief Registry for native string methods (e.g., split, trim).
 */
extern HashMap *native_string_methods;

/**
 * @brief Registry for native list methods (e.g., append, pop).
 */
extern HashMap *native_list_methods;

/**
 * @brief Registry of native methods for maps (len, has, get, keys, values, remove).
 */
extern HashMap *native_map_methods;

/**
 * @brief Registry for native module functions (e.g., math.sqrt, io.read_file).
 */
extern HashMap *native_module_funcs;

// --- Native Registration Helpers ---
//
// These helpers are defined in interpreter.c but are exposed here so that
// native modules living in their own translation units (e.g. the optional
// `gui` module in lib/gui.c) can build their own function tables using the exact
// same machinery as the built-in math/io/sys modules.

/**
 * @brief Creates a new, empty hashmap.
 * @param key_type The value type used for keys (modules use VAL_STRING).
 * @param value_type The value type stored (module tables use VAL_NATIVE_FN).
 * @return A newly allocated, GC-managed hashmap.
 */
HashMap *hashmap_create(ValueType key_type, ValueType value_type);

/**
 * @brief Inserts or updates an entry in a hashmap.
 * @param map The target hashmap.
 * @param key The (string) key to set.
 * @param value The value to store.
 * @param line_num Source line for error reporting (0 when not applicable).
 */
void hashmap_set(HashMap *map, const char *key, Value value, int line_num);

/**
 * @brief Looks up an entry in a hashmap.
 * @param map The hashmap to search.
 * @param key The (string) key to find.
 * @return The stored value, or a VAL_VOID value if the key is absent.
 */
Value hashmap_get(HashMap *map, const char *key);

/**
 * @brief Tests whether a hashmap contains a key (even if its value is void).
 * @param map The hashmap to search.
 * @param key The (string) key to find.
 * @return 1 if present, 0 otherwise.
 */
int hashmap_has(HashMap *map, const char *key);

/**
 * @brief Appends an item to a list, growing it as needed (no type check).
 * @param list The list (must not be fixed-size).
 * @param item The item to append.
 */
void list_add(List *list, Value item);

/**
 * @brief Removes a key from a hashmap.
 * @return 1 if the key was present, 0 otherwise.
 */
int hashmap_remove(HashMap *map, const char *key);

/**
 * @brief Returns a hashmap's entries in insertion order.
 * @return A malloc'd array of map->count entries; the caller frees it.
 */
MapEntry **hashmap_entries_in_order(HashMap *map);

/**
 * @brief Returns the user-facing name of a value type ("int", "string", ...).
 * @param type The value type.
 * @return A static string.
 */
const char *get_value_type_name(ValueType type);

/**
 * @brief Registers a C function as a named entry in a module/method table.
 * @param method_map The table to add the function to.
 * @param name The name the function will be exposed under (e.g. "button").
 * @param function The C implementation to call.
 */
void register_native_method(HashMap *method_map, const char *name, NativeFn function);

// --- Initialization Functions ---

/**
 * @brief Defines all built-in native functions (clock, input, etc.) in the given environment.
 * @param env_ptr Pointer to the environment pointer.
 */
void define_all_natives_in_env(Env **env_ptr);

/**
 * @brief Registers all native methods for built-in types (string, list).
 */
void register_all_native_methods();

/**
 * @brief Registers all native modules (math, io, sys).
 */
void register_all_native_modules();

// --- Interpreter Core Functions ---

/**
 * @brief Evaluates an expression node.
 * @param node The AST node to evaluate.
 * @param env The current environment.
 * @return The result of the evaluation.
 */
Value eval(ASTNode *node, Env *env);

/**
 * @brief Executes a statement node.
 * @param node The AST node to execute.
 * @param env_ptr Pointer to the current environment pointer (may be modified).
 * @return The result of execution (e.g., return value, break signal).
 */
Value exec(ASTNode *node, Env **env_ptr);

/**
 * @brief Executes a block of statements in a new scope.
 * @param node The block AST node.
 * @param env_ptr Pointer to the current environment pointer.
 * @return The result of the block execution.
 */
Value exec_block(ASTNode *node, Env **env_ptr);

/**
 * @brief Executes a module (a list of statements) in a given environment.
 * @param root The root AST node of the module.
 * @param env_ptr Pointer to the module's environment pointer.
 */
void exec_module(ASTNode *root, Env **env_ptr);

// --- Error Context ---

/**
 * @brief Sets the current line number for runtime error reporting in native functions.
 * @param line The line number.
 */
void set_exec_error_line(int line);

/**
 * @brief Gets the current line number for runtime error reporting.
 * @return The line number.
 */
int get_exec_error_line();

/**
 * @brief Discards all active try handlers.
 *
 * For the REPL's error-recovery path: its longjmp (e.g. on Ctrl+C) can unwind
 * past try statements without popping their handler frames.
 */
void reset_try_frames(void);

/**
 * @brief Calls a Pith function (or native function) from C.
 *
 * Arity and declared parameter/return types are checked as for a call written
 * in Pith. Errors are reported normally (they reach any active `try`).
 *
 * @param callee The function value.
 * @param args Arguments; heap values among them must be GC-rooted by the caller.
 * @param arg_count Number of arguments.
 * @param line Line to blame in error messages (0 if none).
 * @return The function's result.
 */
Value interpreter_call(Value callee, Value *args, int arg_count, int line);

/**
 * @brief Like interpreter_call(), but catches any error the call raises.
 *
 * For native code that runs Pith callbacks and must keep going when one fails
 * (e.g. the http server's route handlers).
 *
 * @param result Receives the result when the call succeeds.
 * @param error Receives the error message when it fails.
 * @param error_size Size of @p error.
 * @return 0 on success, 1 if the callee raised an error.
 */
int interpreter_call_protected(Value callee, Value *args, int arg_count, Value *result, char *error,
                               size_t error_size);

#endif //PITH_INTERPRETER_H
