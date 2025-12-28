/**
 * @file tokenizer.c
 * @brief Implementation of the Pith language tokenizer (lexer).
 *
 * This file contains the logic for converting raw source code text into a sequence of tokens.
 * It handles significant whitespace (indentation/dedentation), comments, literals (strings, numbers),
 * keywords, and various operators. The tokenizer produces a stream of tokens that is consumed by the parser.
 */

#include "tokenizer.h"
#include "debug.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>

/**
 * @brief Adds a token to the tokenizer state.
 *
 * Resizes the token array if necessary and appends the new token.
 *
 * @param state The tokenizer state.
 * @param type The type of the token.
 * @param value The value of the token (e.g., a number or identifier). Can be NULL for simple tokens.
 * @param line_num The line number for error reporting.
 */
void add_token(TokenizerState *state, TokenType type, const char *value, int line_num)
{
    if (state->token_count >= state->token_capacity)
    {
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
    "class", "new", "list", "extends", NULL
};

/**
 * @brief Checks if a word is a reserved keyword.
 *
 * @param word The word to check.
 * @return 1 if the word is a keyword, 0 otherwise.
 */
int is_keyword(const char *word)
{
    for (int i = 0; keywords[i] != NULL; i++)
    {
        if (strcmp(word, keywords[i]) == 0)
            return 1;
    }
    return 0;
}

/**
 * @brief Tokenizes a source string.
 *
 * Scans the source string character by character and populates the tokenizer state with tokens.
 * Handles indentation tracking for block structure.
 *
 * @param source The source string to tokenize.
 * @param state The tokenizer state to populate.
 */
void tokenize(const char *source, TokenizerState *state)
{
    state->tokens = NULL;
    state->token_count = 0;
    state->token_capacity = 0;

    int i = 0;
    int line_num = 1;
    int indent_stack[100];
    int indent_level = 0;
    indent_stack[0] = 0;

    int at_line_start = 1;

    while (source[i] != '\0')
    {
        char c = source[i];

        // DO NOT DISCARD DEBUG CODE
#ifdef DEBUG_TRACE_TOKENIZER
        printf("[TOKENIZER] Processing char '%c' at index %d\n", c, i); fflush(stdout);
#endif

        // --- Comment Handling ---
        if (c == '#')
        {
            // Check for multi-line comment start '###'
            if (source[i + 1] == '#' && source[i + 2] == '#')
            {
#ifdef DEBUG_TRACE_TOKENIZER
                printf("[TOKENIZER] Entering multi-line comment at line %d\n", line_num); fflush(stdout);
#endif
                i += 3; // Skip '###'
                while (source[i] != '\0')
                {
                    // Check for multi-line comment end '###'
                    if (source[i] == '#' && source[i + 1] == '#' && source[i + 2] == '#')
                    {
                        i += 3; // Skip '###'
#ifdef DEBUG_TRACE_TOKENIZER
                        printf("[TOKENIZER] Exiting multi-line comment at line %d\n", line_num); fflush(stdout);
#endif
                        break;
                    }
                    if (source[i] == '\n')
                    {
                        line_num++;
                    }
                    i++;
                }
                continue;
            }
            else
            {
                // Single-line comment using '#'
                while (source[i] != '\n' && source[i] != '\0')
                    i++;
                continue;
            }
        }

        // --- Indentation Handling ---
        if (at_line_start)
        {
            int spaces = 0;
            while (source[i] == ' ' || source[i] == '\t')
            {
                spaces++;
                i++;
            }

            // Handle CR (Windows line endings)
            if (source[i] == '\r')
            {
#ifdef DEBUG_TRACE_TOKENIZER
                printf("[TOKENIZER_DEBUG] Skipping CR at index %d\n", i);
#endif
                i++;
            }

            // Skip empty lines
            if (source[i] == '\n' || source[i] == '\0')
            {
#ifdef DEBUG_TRACE_TOKENIZER
                printf("[TOKENIZER_DEBUG] Empty line at index %d\n", i);
#endif
                at_line_start = 1;
                if (source[i] == '\n')
                {
                    i++;
                    line_num++;
                }
                continue;
            }

#ifdef DEBUG_TRACE_TOKENIZER
            printf("[TOKENIZER_DEBUG] Line start at index %d, spaces=%d, indent_level=%d, stack[level]=%d\n", i, spaces,
                   indent_level, indent_stack[indent_level]);
#endif

            // Check for indentation increase (INDENT)
            if (spaces > indent_stack[indent_level])
            {
                indent_level++;
                indent_stack[indent_level] = spaces;
                add_token(state, TOKEN_INDENT, NULL, line_num);
            }
            // Check for indentation decrease (DEDENT)
            else
            {
                while (spaces < indent_stack[indent_level])
                {
#ifdef DEBUG_TRACE_TOKENIZER
                    printf("[TOKENIZER_DEBUG] Emitting DEDENT. spaces=%d < stack[%d]=%d\n", spaces, indent_level,
                           indent_stack[indent_level]);
#endif
                    add_token(state, TOKEN_DEDENT, NULL, line_num);
                    indent_level--;
                }
            }
            at_line_start = 0;
            continue;
        }

        // Skip whitespace within lines
        if (c == ' ' || c == '\t' || c == '\r')
        {
            i++;
            continue;
        }

        // Handle newlines
        if (c == '\n')
        {
            add_token(state, TOKEN_NEWLINE, NULL, line_num);
            line_num++;
            i++;
            at_line_start = 1;
            continue;
        }

        // --- Single Character Tokens ---
        if (c == '(')
        {
            add_token(state, TOKEN_LPAREN, "(", line_num);
            i++;
            continue;
        }
        if (c == ')')
        {
            add_token(state, TOKEN_RPAREN, ")", line_num);
            i++;
            continue;
        }
        if (c == '[')
        {
            add_token(state, TOKEN_LBRACKET, "[", line_num);
            i++;
            continue;
        }
        if (c == ']')
        {
            add_token(state, TOKEN_RBRACKET, "]", line_num);
            i++;
            continue;
        }
        if (c == '{')
        {
            add_token(state, TOKEN_LBRACE, "{", line_num);
            i++;
            continue;
        }
        if (c == '}')
        {
            add_token(state, TOKEN_RBRACE, "}", line_num);
            i++;
            continue;
        }
        if (c == ':')
        {
            add_token(state, TOKEN_COLON, ":", line_num);
            i++;
            continue;
        }
        if (c == ',')
        {
            add_token(state, TOKEN_COMMA, ",", line_num);
            i++;
            continue;
        }
        if (c == ';')
        {
            add_token(state, TOKEN_SEMICOLON, ";", line_num);
            i++;
            continue;
        }
        if (c == '.')
        {
            add_token(state, TOKEN_DOT, ".", line_num);
            i++;
            continue;
        }
        if (c == '+')
        {
            add_token(state, TOKEN_PLUS, "+", line_num);
            i++;
            continue;
        }
        if (c == '-')
        {
            add_token(state, TOKEN_MINUS, "-", line_num);
            i++;
            continue;
        }
        if (c == '*')
        {
            add_token(state, TOKEN_STAR, "*", line_num);
            i++;
            continue;
        }
        if (c == '/')
        {
            add_token(state, TOKEN_SLASH, "/", line_num);
            i++;
            continue;
        }
        if (c == '%')
        {
            add_token(state, TOKEN_PERCENT, "%", line_num);
            i++;
            continue;
        }
        if (c == '^')
        {
            add_token(state, TOKEN_CARET, "^", line_num);
            i++;
            continue;
        }

        // --- Multi-Character Operators ---
        if (c == '!')
        {
            if (source[i + 1] == '=')
            {
                add_token(state, TOKEN_NEQ, "!=", line_num);
                i += 2;
                continue;
            }
            add_token(state, TOKEN_BANG, "!", line_num);
            i++;
            continue;
        }
        if (c == '>')
        {
            if (source[i + 1] == '=')
            {
                add_token(state, TOKEN_GTE, ">=", line_num);
                i += 2;
                continue;
            }
            add_token(state, TOKEN_GT, ">", line_num);
            i++;
            continue;
        }
        if (c == '<')
        {
            if (source[i + 1] == '=')
            {
                add_token(state, TOKEN_LTE, "<=", line_num);
                i += 2;
                continue;
            }
            add_token(state, TOKEN_LT, "<", line_num);
            i++;
            continue;
        }
        if (c == '=')
        {
            if (source[i + 1] == '=')
            {
                add_token(state, TOKEN_EQ, "==", line_num);
                i += 2;
                continue;
            }
            add_token(state, TOKEN_ASSIGN, "=", line_num);
            i++;
            continue;
        }

        // --- String Literals ---
        if (c == '"')
        {
            i++;
            int start = i;
            // We'll build processed string in a dynamic buffer to handle escapes
            char *buf = malloc(strlen(source) + 1); // conservative
            int bpos = 0;
            while (source[i] != '\0' && source[i] != '"')
            {
                if (source[i] == '\\' && source[i + 1] != '\0')
                {
                    i++; // skip backslash
                    char esc = source[i];
                    if (esc == 'n')
                        buf[bpos++] = '\n';
                    else if (esc == 't')
                        buf[bpos++] = '\t';
                    else if (esc == '\\')
                        buf[bpos++] = '\\';
                    else if (esc == '"')
                        buf[bpos++] = '"';
                    else if (esc == 'r')
                        buf[bpos++] = '\r';
                    else
                    {
                        // Unknown escape: keep the char as-is
                        buf[bpos++] = esc;
                    }
                    i++;
                    continue;
                }
                buf[bpos++] = source[i++];
            }
            buf[bpos] = '\0';
            char *str_val = strdup(buf);
            free(buf);
            add_token(state, TOKEN_STRING, str_val, line_num);
            if (source[i] == '"')
                i++;
            continue;
        }

        // --- Number Literals ---
        if (isdigit(c))
        {
            int start = i;
            int is_float = 0;
            while (isdigit(source[i]) || source[i] == '.')
            {
                if (source[i] == '.')
                    is_float = 1;
                i++;
            }
            int length = i - start;
            char *num_val = malloc(length + 1);
            strncpy(num_val, source + start, length);
            num_val[length] = '\0';
            if (is_float)
                add_token(state, TOKEN_FLOAT_LITERAL, num_val, line_num);
            else
                add_token(state, TOKEN_NUMBER, num_val, line_num);
            continue;
        }

        // --- Identifiers and Keywords ---
        if (isalpha(c))
        {
            int start = i;
            while (isalnum(source[i]) || source[i] == '_')
                i++;
            int length = i - start;
            char *word = malloc(length + 1);
            strncpy(word, source + start, length);
            word[length] = '\0';

            if (strcmp(word, "extends") == 0)
            {
                add_token(state, TOKEN_EXTENDS, "extends", line_num);
            }
            else if (is_keyword(word))
            {
                if (strcmp(word, "import") == 0)
                {
                    add_token(state, TOKEN_IMPORT, "import", line_num);
                }
                else
                {
                    add_token(state, TOKEN_KEYWORD, word, line_num);
                }
            }
            else
            {
                add_token(state, TOKEN_IDENTIFIER, word, line_num);
            }
            free(word);
            continue;
        }
        i++;
    }

    // Emit remaining DEDENT tokens at EOF
    while (indent_level > 0)
    {
        add_token(state, TOKEN_DEDENT, NULL, line_num);
        indent_level--;
    }
    add_token(state, TOKEN_EOF, NULL, line_num);
}

/**
 * @brief Frees memory associated with the tokenizer state.
 *
 * @param state The tokenizer state to free.
 */
void free_tokens(TokenizerState *state)
{
    if (state->tokens)
    {
        for (int i = 0; i < state->token_count; i++)
        {
            if (state->tokens[i].value)
            {
                free(state->tokens[i].value);
            }
        }
        free(state->tokens);
        state->tokens = NULL;
        state->token_count = 0;
        state->token_capacity = 0;
    }
}
