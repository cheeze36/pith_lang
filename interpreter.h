/**
 * @file interpreter.h
 * @brief This file contains the core logic for interpreting and executing the Pith language.
 * It defines the functions for evaluating AST nodes, managing environments, and handling runtime errors.
 */

#ifndef PITH_INTERPRETER_H
#define PITH_INTERPRETER_H

#include "parser.h"
#include "value.h"
#include "common.h" // Include the new common header for error reporting

// The main function to interpret and execute the AST
void interpret(ASTNode *root);

// Utility function to read a file's content
char* read_file_content(const char* filename);

// Expose the global environment and setup functions for the REPL.
extern Env* global_env;
extern HashMap* native_string_methods;
extern HashMap* native_list_methods;
extern HashMap* native_module_funcs;

void define_all_natives_in_env(Env **env_ptr);
void register_all_native_methods();
void register_all_native_modules();

// Forward Declarations for interpreter functions
Value eval(ASTNode *node, Env *env);
Value exec(ASTNode *node, Env **env_ptr);
Value exec_block(ASTNode *node, Env **env_ptr);
void exec_module(ASTNode *root, Env **env_ptr);

#endif //PITH_INTERPRETER_H
