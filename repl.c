/**
 * @file repl.c
 * @brief Implementation of the Read-Eval-Print Loop (REPL).
 *
 * This file handles the interactive shell for the Pith language.
 * It reads user input, handles multi-line statements, tokenizes, parses,
 * and executes the code, printing the result if applicable.
 * It also manages error recovery and signal handling (Ctrl+C).
 */

#include "repl.h"
#include "interpreter.h"
#include "parser.h"
#include "tokenizer.h"
#include "debug.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <ctype.h>
#include <signal.h>

// --- Global REPL State ---
// Used for error recovery to jump back to the main loop
static jmp_buf repl_error_jmp;
// Flag to indicate if SIGINT (Ctrl+C) was received
static volatile sig_atomic_t sigint_flag = 0;

// Track resources for cleanup on error
static TokenizerState *current_tokenizer_state = NULL;
static ASTNode *current_ast_root = NULL;
static char *current_code_buffer = NULL;

/**
 * @brief Custom error reporting function for the REPL.
 *
 * Instead of exiting the program, this function cleans up resources
 * and uses longjmp to return control to the main REPL loop.
 *
 * @param line The line number where the error occurred.
 * @param format The printf-style format string for the error message.
 * @param ... Arguments for the format string.
 */
void repl_report_error(int line, const char *format, ...)
{
    fprintf(stderr, "[line %d] Error: ", line);
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr, "\n");
    // Print source context if available
    print_error_context(line);

    // Cleanup before jumping
    if (current_tokenizer_state)
    {
        free_tokens(current_tokenizer_state);
        current_tokenizer_state = NULL;
    }
    if (current_ast_root)
    {
        free_ast(current_ast_root);
        current_ast_root = NULL;
    }

    longjmp(repl_error_jmp, 1);
}

/**
 * @brief Signal handler for SIGINT (Ctrl+C).
 *
 * Sets a flag that is checked in the main loop.
 *
 * @param sig The signal number.
 */
void handle_sigint(int sig)
{
    sigint_flag = 1;
}

/**
 * @brief Checks if a line of code is likely incomplete.
 *
 * Heuristically determines if the user has more to type (e.g., open parentheses,
 * trailing colon for a block).
 *
 * @param line The line of code to check.
 * @return 1 if the line is incomplete, 0 otherwise.
 */
int is_line_incomplete(const char *line)
{
    int len = strlen(line);
    if (len == 0)
        return 0;

    int open_parens = 0;
    int open_brackets = 0;
    int open_braces = 0;

    for (int i = 0; i < len; i++)
    {
        char c = line[i];
        if (c == '(')
            open_parens++;
        else if (c == ')')
            open_parens--;
        else if (c == '[')
            open_brackets++;
        else if (c == ']')
            open_brackets--;
        else if (c == '{')
            open_braces++;
        else
            if (c == '}')
                open_braces--;
    }

    // Check for trailing colon which indicates a block start
    int has_trailing_colon = 0;
    for (int i = len - 1; i >= 0; i--)
    {
        if (!isspace((unsigned char) line[i]))
        {
            if (line[i] == ':')
                has_trailing_colon = 1;
            break;
        }
    }

    if (open_parens > 0 || open_brackets > 0 || open_braces > 0 || has_trailing_colon)
    {
        return 1;
    }

    // Check if a block was started in previous lines (heuristic)
    int block_started = 0;
    char *copy = strdup(line);
    if (!copy)
        return 0; // Out of memory
    char *line_tok = strtok(copy, "\n");
    while (line_tok != NULL)
    {
        int tlen = strlen(line_tok);
        for (int i = tlen - 1; i >= 0; i--)
        {
            if (!isspace((unsigned char) line_tok[i]))
            {
                if (line_tok[i] == ':')
                    block_started = 1;
                break;
            }
        }
        line_tok = strtok(NULL, "\n");
    }
    free(copy);

    // If a block was started, wait for a double newline to terminate
    if (block_started)
    {
        if (len >= 2 && line[len - 1] == '\n' && line[len - 2] == '\n')
            return 0;
        if (len >= 4 && line[len - 1] == '\n' && line[len - 2] == '\r' && line[len - 3] == '\n' && line[len - 4] ==
            '\r')
            return 0;
        return 1;
    }

    return 0;
}

/**
 * @brief Checks if an AST node represents an expression.
 *
 * Used to decide whether to print the result of an evaluation in the REPL.
 *
 * @param type The AST node type.
 * @return 1 if it is an expression node, 0 otherwise.
 */
int is_expression_node(ASTNodeType type)
{
    switch (type)
    {
        case AST_INT_LITERAL:
        case AST_FLOAT_LITERAL:
        case AST_STRING_LITERAL:
        case AST_BOOL_LITERAL:
        case AST_VAR_REF:
        case AST_BINARY_OP:
        case AST_UNARY_OP:
        case AST_FUNC_CALL:
        case AST_NEW_EXPR:
        case AST_FIELD_ACCESS:
        case AST_INDEX_ACCESS:
        case AST_LIST_LITERAL:
        case AST_HASHMAP_LITERAL:
            return 1;
        default:
            return 0;
    }
}

/**
 * @brief Starts the Read-Eval-Print Loop.
 *
 * @param interactive_mode Flag to indicate if running in interactive mode (currently unused but good for future).
 */
void start_repl(int interactive_mode)
{
    printf("Pith REPL v0.2\n");
    printf("Type 'exit' to quit.\n");

    // Initialize global environment if not already done
    if (!global_env)
    {
        define_all_natives_in_env(&global_env);
        register_all_native_methods();
        register_all_native_modules();
    }

    // Set up error handling
    set_error_reporter(repl_report_error);
    signal(SIGINT, handle_sigint);

    char line_buffer[1024];
    size_t code_buffer_size = 0;

    while (1)
    {
        // Error recovery point
        if (setjmp(repl_error_jmp))
        {
            // Error occurred or interrupt happened
            if (current_code_buffer)
            {
                free(current_code_buffer);
                current_code_buffer = NULL;
            }
            code_buffer_size = 0;
            printf("\n");
            sigint_flag = 0; // Reset flag
            continue;
        }

        if (sigint_flag)
        {
            printf("\nKeyboardInterrupt\n");
            longjmp(repl_error_jmp, 1);
        }

        printf("pith > ");

        if (!fgets(line_buffer, sizeof(line_buffer), stdin))
        {
            if (sigint_flag)
            {
                // Interrupted during fgets
                printf("\nKeyboardInterrupt\n");
                longjmp(repl_error_jmp, 1);
            }
            break; // EOF
        }

        // Check for exit command
        char *trimmed = line_buffer;
        while (isspace(*trimmed))
            trimmed++;
        if (strncmp(trimmed, "exit", 4) == 0)
        {
            char *end = trimmed + 4;
            while (isspace(*end))
                end++;
            if (*end == '\0')
                break;
        }

        // Accumulate input
        if (current_code_buffer)
            free(current_code_buffer);
        current_code_buffer = strdup(line_buffer);
        code_buffer_size = strlen(current_code_buffer);

        // Handle multi-line input
        while (is_line_incomplete(current_code_buffer))
        {
            printf("... > ");
            if (!fgets(line_buffer, sizeof(line_buffer), stdin))
            {
                if (sigint_flag)
                {
                    // Interrupted during multi-line input
                    printf("\nKeyboardInterrupt\n");
                    longjmp(repl_error_jmp, 1);
                }
                break;
            }

            current_code_buffer = realloc(current_code_buffer, code_buffer_size + strlen(line_buffer) + 1);
            strcat(current_code_buffer, line_buffer);
            code_buffer_size += strlen(line_buffer);
        }

#ifdef DEBUG_DEEP_DIVE_INTERP
        printf("[DDI_REPL] Executing buffer:\n---\n%s\n---\n", current_code_buffer);
#endif

        // Tokenize
        TokenizerState t_state;
        current_tokenizer_state = &t_state;
        // Set error context to the current buffer so errors print nicely in REPL
        set_error_context(current_code_buffer, NULL);
        tokenize(current_code_buffer, &t_state);

        // Parse
        ParserState p_state = {&t_state, 0};
        ASTNode *root = parse_program(&p_state);
        current_ast_root = root;

        // Execute
        if (root->children_count > 0)
        {
            ASTNode *first_statement = root->children[0];
            // If it's a single expression, evaluate and print the result
            if (root->children_count == 1 && is_expression_node(first_statement->type))
            {
#ifdef DEBUG_DEEP_DIVE_INTERP
                printf("[DDI_REPL] Evaluating as expression.\n");
#endif
                Value result = eval(first_statement, global_env);
                if (result.type != VAL_VOID)
                {
                    print_value(result);
                    printf("\n");
                }
            }
            else
            {
                // Otherwise, execute as a module (statements)
#ifdef DEBUG_DEEP_DIVE_INTERP
                printf("[DDI_REPL] Executing as statement(s).\n");
#endif
                exec_module(root, &global_env);
            }
        }

        // Cleanup for next iteration
        free_tokens(&t_state);
        current_tokenizer_state = NULL;
        free_ast(root);
        current_ast_root = NULL;
    }

    if (current_code_buffer)
        free(current_code_buffer);
    printf("Exiting REPL.\n");
}
