#include <stdio.h>
#include <stdlib.h>
#include "tokenizer.h"
#include "parser.h"
#include "interpreter.h"
#include "debug.h"
#include "repl.h"
#include "gc.h" // Include GC

// To enable debug traces, uncomment the desired flags in debug.h
// or define them here before the includes.

int main(int argc, char *argv[]) {
    // --- REPL Mode ---
    if (argc == 1) {
        start_repl();
        free_all_objects(); // Clean up GC objects
        return 0;
    }

    // --- File Mode ---
    if (argc != 2) {
        fprintf(stderr, "Usage: %s [filename]\n", argv[0]);
        return 1;
    }

    char *source = read_file_content(argv[1]);
    if (!source) {
        return 1;
    }

    TokenizerState tokenizer_state;
    tokenize(source, &tokenizer_state);

    ParserState parser_state = { &tokenizer_state, 0 };
    ASTNode* ast_root = parse_program(&parser_state);

    interpret(ast_root);

    // Free memory
    free(source);
    for (int i = 0; i < tokenizer_state.token_count; i++) {
        free(tokenizer_state.tokens[i].value);
    }
    free(tokenizer_state.tokens);
    
    free_all_objects(); // Clean up GC objects
    // TODO: Free AST nodes

    return 0;
}
