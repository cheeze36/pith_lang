/**
 * @file main.c
 * @brief Entry point for the Pith language interpreter.
 *
 * This file contains the main function which handles command-line arguments.
 * It can launch the REPL (Read-Eval-Print Loop) or execute a script file.
 * It also handles the initialization and cleanup of the interpreter and garbage collector.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tokenizer.h"
#include "parser.h"
#include "interpreter.h"
#include "debug.h"
#include "repl.h"
#include "gc.h" // Include GC
#include "sys_module.h" // sys_set_args() for sys.args()

// To enable debug traces, uncomment the desired flags in debug.h
// or define them here before the includes.

/**
 * @brief Main entry point.
 *
 * Usage:
 *   pith              - Start REPL
 *   pith script.pith  - Execute script
 *   pith script.pith a b - Execute script with arguments (see sys.args())
 *   pith -i script.pith - Execute script and then drop into REPL
 *
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return Exit code (0 for success, non-zero for error).
 */
int main(int argc, char *argv[])
{
    // Everything the interpreter does runs below this point on the C stack, so
    // the stack guard measures from here (see common.h).
    stack_guard_init();

    // --- REPL Mode (No arguments) ---
    if (argc == 1)
    {
        start_repl(0);
        free_all_objects(); // Clean up GC objects
        return 0;
    }

    int interactive = 0;
    char *filename = NULL;

    // Parse arguments: interpreter flags, then the script; everything after the
    // script belongs to the script (see sys.args()).
    int script_index = argc;
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-i") == 0)
        {
            interactive = 1;
        }
        else
        {
            filename = argv[i];
            script_index = i;
            break;
        }
    }
    if (filename)
        sys_set_args(filename, argc - script_index - 1, argv + script_index + 1);

    if (filename == NULL)
    {
        // If only -i was provided, just start REPL
        if (interactive)
        {
            start_repl(0);
            free_all_objects();
            return 0;
        }
        fprintf(stderr, "Usage: %s [-i] [script.pith [script arguments...]]\n", argv[0]);
        return 1;
    }

    // --- File Mode ---
    char *source = read_file_content(filename);
    if (!source)
    {
        fprintf(stderr, "Error: Could not read file '%s'.\n", filename);
        return 1;
    }

    // Tokenize
    TokenizerState tokenizer_state;
    // Provide error context for better messages
    set_error_context(source, filename);
    tokenize(source, &tokenizer_state);

    // Parse
    ParserState parser_state = {&tokenizer_state, 0};
    ASTNode *ast_root = parse_program(&parser_state);

    // Interpret
    interpret(ast_root);

    // The tokens are no longer needed (AST nodes copy their text).
    free_tokens(&tokenizer_state);

    // If interactive mode was requested, start REPL after script execution
    if (interactive)
    {
        // Drop into REPL, preserving the environment populated by the script
        start_repl(1);
    }

    // Free the script's syntax tree and source only now: functions and classes
    // it defined point into the tree, and the REPL above may still call them.
    free_ast(ast_root);
    free(source);

    // Final cleanup
    free_all_objects(); // Clean up GC objects

    return 0;
}
