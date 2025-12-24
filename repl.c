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

// --- Global REPL State ---
static jmp_buf repl_error_jmp;
static error_reporter_t default_error_reporter;

/**
 * @brief Custom error reporting function for the REPL.
 * This function uses longjmp to return control to the main REPL loop
 * instead of exiting the program.
 */
void repl_report_error(int line, const char *format, ...) {
    fprintf(stderr, "[line %d] Error: ", line);
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr, "\n");
    longjmp(repl_error_jmp, 1);
}

/**
 * @brief Checks if a line of code is likely incomplete.
 * This is a simple heuristic to allow for multi-line blocks.
 * @param line The line of code to check.
 * @return 1 if the line is incomplete, 0 otherwise.
 */
int is_line_incomplete(const char* line) {
    int len = strlen(line);
    if (len == 0) return 0;
    // If the last non-whitespace character is a colon, we need more input.
    for (int i = len - 1; i >= 0; i--) {
        if (!isspace((unsigned char)line[i])) {
            if (line[i] == ':') return 1;
            break;
        }
    }
    return 0;
}

void start_repl() {
    printf("Pith REPL v0.1\n");
    printf("Type 'exit' to quit.\n");

    define_all_natives_in_env(&global_env);
    register_all_native_methods();
    register_all_native_modules();
    
    // Set the custom error reporter for the REPL
    set_error_reporter(repl_report_error);

    char line_buffer[1024];
    char* code_buffer = NULL;
    size_t code_buffer_size = 0;

    while (1) {
        if (setjmp(repl_error_jmp)) {
            if (code_buffer) free(code_buffer);
            code_buffer = NULL;
            code_buffer_size = 0;
            printf("\n");
            continue;
        }

        printf("pith > ");
        
        if (!fgets(line_buffer, sizeof(line_buffer), stdin)) {
            break; // EOF
        }

        if (strcmp(line_buffer, "exit\n") == 0) {
            break;
        }

        if (code_buffer) free(code_buffer);
        code_buffer = strdup(line_buffer);
        code_buffer_size = strlen(code_buffer);

        while (is_line_incomplete(code_buffer)) {
            printf("... > ");
            if (!fgets(line_buffer, sizeof(line_buffer), stdin)) break;
            if (strcmp(line_buffer, "\n") == 0) break;

            code_buffer = realloc(code_buffer, code_buffer_size + strlen(line_buffer) + 1);
            strcat(code_buffer, line_buffer);
            code_buffer_size += strlen(line_buffer);
        }

        #ifdef DEBUG_DEEP_DIVE_INTERP
        printf("[DDI_REPL] Executing buffer:\n---\n%s\n---\n", code_buffer);
        #endif

        TokenizerState t_state;
        tokenize(code_buffer, &t_state);
        ParserState p_state = {&t_state, 0};
        ASTNode* root = parse_program(&p_state);

        if (root->children_count > 0) {
            ASTNode* first_statement = root->children[0];
            if (root->children_count == 1 && first_statement->type > AST_CONTINUE) {
                #ifdef DEBUG_DEEP_DIVE_INTERP
                printf("[DDI_REPL] Evaluating as expression.\n");
                #endif
                Value result = eval(first_statement, global_env);
                if (result.type != VAL_VOID) {
                    print_value(result);
                    printf("\n");
                }
            } else {
                #ifdef DEBUG_DEEP_DIVE_INTERP
                printf("[DDI_REPL] Executing as statement(s).\n");
                #endif
                exec_module(root, &global_env);
            }
        }
    }

    if (code_buffer) free(code_buffer);
    printf("Exiting REPL.\n");
}
