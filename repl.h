/**
 * @file repl.h
 * @brief Header file for the Read-Eval-Print Loop (REPL).
 *
 * Declares the entry point for the interactive shell.
 */

#ifndef PITH_REPL_H
#define PITH_REPL_H

/**
 * @brief Starts the interactive REPL for the Pith interpreter.
 *
 * This function enters a loop that reads user input, handles multi-line statements,
 * tokenizes, parses, executes the code, and prints results.
 * It handles errors and signals (like Ctrl+C) gracefully.
 *
 * @param interactive_mode If true, indicates the REPL was started after running a script
 *                         (preserving the script's environment).
 */
void start_repl(int interactive_mode);

#endif //PITH_REPL_H
