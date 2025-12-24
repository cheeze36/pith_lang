#ifndef UNTITLED1_PARSER_H
#define UNTITLED1_PARSER_H

#include "tokenizer.h"
#include "common.h"

// --- AST Node Types ---
typedef enum {
    AST_PROGRAM,
    AST_IMPORT,
    AST_FUNC_DEF,
    AST_VAR_DECL,
    AST_PRINT,
    AST_ASSIGNMENT,
    AST_IF,
    AST_WHILE,
    AST_DO_WHILE,
    AST_FOR,
    AST_FOREACH,
    AST_RETURN,
    AST_BREAK,
    AST_CONTINUE,
    AST_SWITCH,
    AST_CASE,
    AST_DEFAULT,
    AST_STRING_LITERAL,
    AST_INT_LITERAL,
    AST_FUNC_CALL,
    AST_VAR_REF,
    AST_BINARY_OP,
    AST_UNARY_OP,
    AST_BLOCK,
    AST_FLOAT_LITERAL,
    AST_BOOL_LITERAL,
    AST_LIST_LITERAL,
    AST_HASHMAP_LITERAL,
    AST_FIELD_ACCESS,
    AST_INDEX_ACCESS,
    AST_ARRAY_SPECIFIER,
    AST_CLASS_DEF,
    AST_NEW_EXPR,
    AST_FIELD_DECL
} ASTNodeType;

// --- AST Node Structure ---
typedef struct ASTNode {
    ASTNodeType type;
    char *value;
    char *type_name; // For typed declarations
    struct ASTNode **children;
    int children_count;
    char **args;
    int arg_count;
    int line_num;
} ASTNode;

// --- Parser State ---
typedef struct {
    TokenizerState *tokenizer_state;
    int current_token;
} ParserState;

// --- Public API ---
/**
 * @brief Parses a sequence of tokens into an Abstract Syntax Tree (AST).
 * @param state The parser state, containing the token stream.
 * @return The root node of the generated AST.
 */
ASTNode* parse_program(ParserState *state);

#endif //UNTITLED1_PARSER_H
