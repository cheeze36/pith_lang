#ifndef PITH_COMMON_H
#define PITH_COMMON_H

#include <stdarg.h>

/**
 * @file common.h
 * @brief This file contains common utilities and type definitions shared across
 * the entire Pith project, such as the error reporting system.
 */

// --- Error Handling ---
// Define a function pointer type for the error reporter.
// This allows different parts of the program (like the REPL) to substitute
// their own non-crashing error handlers.
typedef void (*error_reporter_t)(int line, const char *format, ...);

/**
 * @brief Sets the global function to be used for reporting errors.
 * @param reporter A function pointer to the new error reporter.
 */
void set_error_reporter(error_reporter_t reporter);

/**
 * @brief Reports a runtime or parsing error using the currently set reporter.
 * @param line The line number where the error occurred.
 * @param format The error message format string.
 * @param ... Variable arguments for the format string.
 */
void report_error(int line, const char *format, ...);


#endif //PITH_COMMON_H
