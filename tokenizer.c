#include "tokenizer.h"
#include "debug.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>

/**
 * @brief Adds a token to the tokenizer state.
 * @param state The tokenizer state.
 * @param type The type of the token.
 * @param value The value of the token (e.g., a number or identifier).
 * @param line_num The line number for error reporting.
 */
void add_token(TokenizerState *state, TokenType type, const char *value, int line_num) {
    if (state->token_count >= state->token_capacity) {
        state->token_capacity = (state->token_capacity == 0) ? 128 : state->token_capacity * 2;
        state->tokens = realloc(state->tokens, state->token_capacity * sizeof(Token));
    }
    state->tokens[state->token_count].type = type;
    state->tokens[state->token_count].value = value ? strdup(value) : NULL;
    state->tokens[state->token_count].line_num = line_num;
    state->token_count++;
}

const char *keywords[] = {
    "print", "define", "return", "int", "string", "void", "float", "bool",
    "if", "else", "elif", "while", "for", "foreach", "in", "do", "switch", "case", "default",
    "break", "continue", "pass", "true", "false", "and", "or", "map", "import",
    "class", "new", "list", NULL
};

/**
 * @brief Checks if a word is a keyword.
 * @param word The word to check.
 * @return 1 if the word is a keyword, 0 otherwise.
 */
int is_keyword(const char *word) {
    for (int i = 0; keywords[i] != NULL; i++) {
        if (strcmp(word, keywords[i]) == 0) return 1;
    }
    return 0;
}

/**
 * @brief Tokenizes a source string.
 * @param source The source string to tokenize.
 * @param state The tokenizer state to populate.
 */
void tokenize(const char *source, TokenizerState *state) {
    state->tokens = NULL;
    state->token_count = 0;
    state->token_capacity = 0;

    int i = 0;
    int line_num = 1;
    int indent_stack[100];
    int indent_level = 0;
    indent_stack[0] = 0;

    int at_line_start = 1;

    while (source[i] != '\0') {
        char c = source[i];

        // DO NOT DISCARD DEBUG CODE
        #ifdef DEBUG_TRACE_TOKENIZER
        printf("[TOKENIZER] Processing char '%c' at index %d\n", c, i); fflush(stdout);
        #endif

        if (c == '#') {
            // Check for multi-line comment start '###'
            if (source[i+1] == '#' && source[i+2] == '#') {
                #ifdef DEBUG_TRACE_TOKENIZER
                printf("[TOKENIZER] Entering multi-line comment at line %d\n", line_num); fflush(stdout);
                #endif
                i += 3; // Skip '###'
                while (source[i] != '\0') {
                    // Check for multi-line comment end '###'
                    if (source[i] == '#' && source[i+1] == '#' && source[i+2] == '#') {
                        i += 3; // Skip '###'
                        #ifdef DEBUG_TRACE_TOKENIZER
                        printf("[TOKENIZER] Exiting multi-line comment at line %d\n", line_num); fflush(stdout);
                        #endif
                        break;
                    }
                    if (source[i] == '\n') {
                        line_num++;
                    }
                    i++;
                }
                continue;
            } else {
                // Single-line comment
                while (source[i] != '\n' && source[i] != '\0') i++;
                continue;
            }
        }

        if (at_line_start) {
            int spaces = 0;
            while (source[i] == ' ' || source[i] == '\t') {
                spaces++;
                i++;
            }
            if (source[i] == '\n' || source[i] == '\0') {
                at_line_start = 1;
                if (source[i] == '\n') { i++; line_num++; }
                continue;
            }

            if (spaces > indent_stack[indent_level]) {
                indent_level++;
                indent_stack[indent_level] = spaces;
                add_token(state, TOKEN_INDENT, NULL, line_num);
            } else {
                while (spaces < indent_stack[indent_level]) {
                    add_token(state, TOKEN_DEDENT, NULL, line_num);
                    indent_level--;
                }
            }
            at_line_start = 0;
            continue;
        }

        if (c == ' ' || c == '\t') { i++; continue; }

        if (c == '\n') {
            add_token(state, TOKEN_NEWLINE, NULL, line_num);
            line_num++;
            i++;
            at_line_start = 1;
            continue;
        }

        if (c == '(') { add_token(state, TOKEN_LPAREN, "(", line_num); i++; continue; }
        if (c == ')') { add_token(state, TOKEN_RPAREN, ")", line_num); i++; continue; }
        if (c == '[') { add_token(state, TOKEN_LBRACKET, "[", line_num); i++; continue; }
        if (c == ']') { add_token(state, TOKEN_RBRACKET, "]", line_num); i++; continue; }
        if (c == '{') { add_token(state, TOKEN_LBRACE, "{", line_num); i++; continue; }
        if (c == '}') { add_token(state, TOKEN_RBRACE, "}", line_num); i++; continue; }
        if (c == ':') { add_token(state, TOKEN_COLON, ":", line_num); i++; continue; }
        if (c == ',') { add_token(state, TOKEN_COMMA, ",", line_num); i++; continue; }
        if (c == ';') { add_token(state, TOKEN_SEMICOLON, ";", line_num); i++; continue; }
        if (c == '.') { add_token(state, TOKEN_DOT, ".", line_num); i++; continue; }
        if (c == '+') { add_token(state, TOKEN_PLUS, "+", line_num); i++; continue; }
        if (c == '-') { add_token(state, TOKEN_MINUS, "-", line_num); i++; continue; }
        if (c == '*') { add_token(state, TOKEN_STAR, "*", line_num); i++; continue; }
        if (c == '/') { add_token(state, TOKEN_SLASH, "/", line_num); i++; continue; }
        if (c == '%') { add_token(state, TOKEN_PERCENT, "%", line_num); i++; continue; }
        if (c == '^') { add_token(state, TOKEN_CARET, "^", line_num); i++; continue; }
        
        if (c == '!') {
            if (source[i+1] == '=') { add_token(state, TOKEN_NEQ, "!=", line_num); i += 2; continue; }
            add_token(state, TOKEN_BANG, "!", line_num); i++; continue;
        }
        if (c == '>') {
            if (source[i+1] == '=') { add_token(state, TOKEN_GTE, ">=", line_num); i += 2; continue; }
            add_token(state, TOKEN_GT, ">", line_num); i++; continue;
        }
        if (c == '<') {
            if (source[i+1] == '=') { add_token(state, TOKEN_LTE, "<=", line_num); i += 2; continue; }
            add_token(state, TOKEN_LT, "<", line_num); i++; continue;
        }
        if (c == '=') { 
            if (source[i+1] == '=') { add_token(state, TOKEN_EQ, "==", line_num); i += 2; continue; }
            add_token(state, TOKEN_ASSIGN, "=", line_num); i++; continue; 
        }

        if (c == '"') {
            i++;
            int start = i;
            while (source[i] != '"' && source[i] != '\0') i++;
            int length = i - start;
            char *str_val = malloc(length + 1);
            strncpy(str_val, source + start, length);
            str_val[length] = '\0';
            add_token(state, TOKEN_STRING, str_val, line_num);
            if (source[i] == '"') i++;
            continue;
        }

        if (isdigit(c)) {
            int start = i;
            int is_float = 0;
            while (isdigit(source[i]) || source[i] == '.') {
                if (source[i] == '.') is_float = 1;
                i++;
            }
            int length = i - start;
            char *num_val = malloc(length + 1);
            strncpy(num_val, source + start, length);
            num_val[length] = '\0';
            if (is_float) add_token(state, TOKEN_FLOAT_LITERAL, num_val, line_num);
            else add_token(state, TOKEN_NUMBER, num_val, line_num);
            continue;
        }

        if (isalpha(c)) {
            int start = i;
            while (isalnum(source[i]) || source[i] == '_') i++;
            int length = i - start;
            char *word = malloc(length + 1);
            strncpy(word, source + start, length);
            word[length] = '\0';

            if (is_keyword(word)) {
                 if (strcmp(word, "import") == 0) {
                    add_token(state, TOKEN_IMPORT, "import", line_num);
                } else {
                    add_token(state, TOKEN_KEYWORD, word, line_num);
                }
            } else {
                add_token(state, TOKEN_IDENTIFIER, word, line_num);
            }
            free(word);
            continue;
        }
        i++;
    }
    
    while (indent_level > 0) {
        add_token(state, TOKEN_DEDENT, NULL, line_num);
        indent_level--;
    }
    add_token(state, TOKEN_EOF, NULL, line_num);
}
