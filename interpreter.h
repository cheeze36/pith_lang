/**
 * @file interpreter.h
 * @brief Header file for the Pith language interpreter.
 *
 * Defines the public interface for the interpreter, including the main entry point `interpret`,
 * environment management, and native function registration.
 */

#ifndef PITH_INTERPRETER_H
#define PITH_INTERPRETER_H

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
 * @brief Registry for native module functions (e.g., math.sqrt, io.read_file).
 */
extern HashMap *native_module_funcs;

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

#endif //PITH_INTERPRETER_H
