/**
 * @file common.h
 * @brief Common utilities and type definitions.
 *
 * This file contains shared definitions used across the Pith project,
 * primarily focusing on the error reporting system.
 */

#ifndef PITH_COMMON_H
#define PITH_COMMON_H

#include <stdarg.h>

// --- Error Handling ---

/**
 * @brief Function pointer type for error reporting.
 *
 * Allows different parts of the program (e.g., REPL vs CLI) to handle errors differently.
 *
 * @param line The line number where the error occurred.
 * @param format The printf-style format string.
 * @param ... Variable arguments.
 */
typedef void (*error_reporter_t)(int line, const char *format, ...);

/**
 * @brief Sets the global error reporter function.
 * @param reporter A function pointer to the new error reporter.
 */
void set_error_reporter(error_reporter_t reporter);

/**
 * @brief Reports a runtime or parsing error.
 *
 * Uses the currently configured error reporter.
 *
 * @param line The line number where the error occurred.
 * @param format The error message format string.
 * @param ... Variable arguments for the format string.
 */
void report_error(int line, const char *format, ...);

// --- Error Context Helpers ---

/**
 * @brief Sets the source code context for error reporting.
 *
 * Allows error messages to display the offending line of code.
 *
 * @param source The source code string.
 * @param filename The name of the source file.
 */
void set_error_context(const char *source, const char *filename);

// --- C Stack Guard ---
//
// Pith functions, nested expressions and nested data are all processed by
// recursive C functions, so a runaway recursion in a Pith program (or a
// 100,000-deep nested expression) would otherwise overflow the C stack and
// crash the interpreter with a segmentation fault. Instead, the interpreter
// measures how much C stack is in use and raises an ordinary, rescuable Pith
// error before it runs out.

/**
 * @brief Records where the C stack begins. Call once, first thing in main().
 *
 * Also determines how much stack Pith may use: the operating system's stack
 * limit (getrlimit on POSIX; on Windows the 8 MB requested at link time, see
 * CMakeLists.txt), minus a safety margin for native code that runs between checks.
 */
void stack_guard_init(void);

/**
 * @brief Raises "Stack overflow" as a Pith error if the C stack is nearly full.
 *
 * Cheap (an address comparison); called by eval(), the parser and other
 * recursive code. Does nothing until stack_guard_init() has run.
 *
 * @param line Source line to report (0 if unknown).
 */
void check_stack_space(int line);

/**
 * @brief Prints the line of code associated with an error.
 * @param line The line number to print.
 */
void print_error_context(int line);

#endif //PITH_COMMON_H
