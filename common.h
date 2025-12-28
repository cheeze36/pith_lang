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

/**
 * @brief Prints the line of code associated with an error.
 * @param line The line number to print.
 */
void print_error_context(int line);

#endif //PITH_COMMON_H
