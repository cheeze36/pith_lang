/**
 * @file tokenizer.h
 * @brief Header file for the Pith language tokenizer.
 *
 * Defines token types and structures used by the lexer.
 */

#ifndef PITH_TOKENIZER_H
#define PITH_TOKENIZER_H

/**
 * @brief Enumeration of all possible token types.
 */
typedef enum
{
    TOKEN_KEYWORD, // Reserved words (if, else, while, etc.)
    TOKEN_IDENTIFIER, // Variable and function names
    TOKEN_NUMBER, // Integer literals
    TOKEN_STRING, // String literals
    TOKEN_LPAREN, // (
    TOKEN_RPAREN, // )
    TOKEN_LBRACKET, // [
    TOKEN_RBRACKET, // ]
    TOKEN_LBRACE, // {
    TOKEN_RBRACE, // }
    TOKEN_COLON, // :
    TOKEN_COMMA, // ,
    TOKEN_SEMICOLON, // ;
    TOKEN_DOT, // .
    TOKEN_PLUS, // +
    TOKEN_MINUS, // -
    TOKEN_STAR, // *
    TOKEN_SLASH, // /
    TOKEN_PERCENT, // %
    TOKEN_CARET, // ^
    TOKEN_BANG, // !
    TOKEN_GT, // >
    TOKEN_LT, // <
    TOKEN_GTE, // >=
    TOKEN_LTE, // <=
    TOKEN_EQ, // ==
    TOKEN_NEQ, // !=
    TOKEN_ASSIGN, // =
    TOKEN_NEWLINE, // Newline character (significant)
    TOKEN_INDENT, // Increase in indentation level
    TOKEN_DEDENT, // Decrease in indentation level
    TOKEN_EOF, // End of file
    TOKEN_FLOAT_LITERAL, // Floating point literals
    TOKEN_BOOL_LITERAL, // Boolean literals (true/false)
    TOKEN_IMPORT, // 'import' keyword
    TOKEN_EXTENDS // 'extends' keyword
} TokenType;

/**
 * @brief Structure representing a single token.
 */
typedef struct
{
    TokenType type; // Type of the token
    char *value; // String value (if applicable, e.g., identifier name)
    int line_num; // Line number where the token appears
} Token;

/**
 * @brief State of the tokenizer.
 *
 * Holds the array of generated tokens.
 */
typedef struct
{
    Token *tokens; // Array of tokens
    int token_count; // Number of tokens
    int token_capacity; // Allocated capacity
} TokenizerState;

/**
 * @brief Tokenizes a source string.
 * @param source The source code string.
 * @param state The tokenizer state to populate.
 */
void tokenize(const char *source, TokenizerState *state);

/**
 * @brief Frees memory associated with tokenizer state.
 * @param state The tokenizer state to free.
 */
void free_tokens(TokenizerState *state);

#endif //PITH_TOKENIZER_H
