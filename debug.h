// #ifndef UNTITLED1_DEBUG_H
// #define UNTITLED1_DEBUG_H
//
// /*
//  * This file centralizes all debug flags for the Pith interpreter.
//  * By uncommenting or commenting these lines, you can control the
//  * verbosity of the interpreter's output for different modules.
//  */
//
// // --- Deep Dive Debugging ---
// // Uncomment the following lines to enable extremely detailed, step-by-step
// // tracing of the parser and interpreter. This is useful for hunting down
// // complex bugs related to AST construction and execution flow.
// #define DEBUG_DEEP_DIVE_PARSER
// #define DEBUG_DEEP_DIVE_INTERP
//
// // --- General Parser Tracing ---
// // Traces the entry and exit of major parsing functions.
//  #define DEBUG_TRACE_PARSER
//
// // --- Detailed Parser Tracing ---
// // Provides more granular details within parsing functions, like argument parsing.
//  #define DEBUG_TRACE_PARSER_DETAIL
//
// // --- General Execution Tracing ---
// // Traces the execution of each AST node.
//  #define DEBUG_TRACE_EXECUTION
//
// // --- Environment Tracing ---
// // Logs when variables are defined or accessed in environments.
//  #define DEBUG_TRACE_ENVIRONMENT
//
// // --- Advanced Environment Tracing ---
// // Provides more detailed information about environment linking and function calls.
//  #define DEBUG_TRACE_ENVIRONMENT_ADV
//
// // --- Memory Management Tracing ---
// // Logs memory allocation and copying for Pith values.
//  #define DEBUG_TRACE_MEMORY
//
// // --- Advanced Memory Tracing ---
// // Provides memory addresses for allocated AST nodes and their components.
//  #define DEBUG_TRACE_ADVANCED_MEMORY
//
// // --- Function Definition Tracing ---
// // Logs details when a Pith function is defined.
//  #define DEBUG_TRACE_FUNCTION_DEFINING
//
// // --- Native Function Tracing ---
// // Traces the execution of native C functions.
//  #define DEBUG_TRACE_NATIVE
//
// // --- Module Import Tracing ---
// // Traces the process of importing modules.
//  #define DEBUG_TRACE_IMPORT
//
// // --- Tokenizer Tracing ---
// // Traces the execution of the tokenizer, showing each character as it is processed.
// #define DEBUG_TRACE_TOKENIZER
//
// #endif //UNTITLED1_DEBUG_H
