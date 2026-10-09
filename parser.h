/**
 * @file parser.h
 * @brief Header file for the Pith language parser.
 *
 * Defines the Abstract Syntax Tree (AST) node types and structures,
 * as well as the parser state and public interface.
 */

#ifndef PITH_PARSER_H
#define PITH_PARSER_H

#include "tokenizer.h"

/**
 * @brief Enumeration of all possible AST node types.
 */
typedef enum
{
    AST_PROGRAM, // Root node of the program
    AST_INT_LITERAL, // Integer literal (e.g., 42)
    AST_FLOAT_LITERAL, // Float literal (e.g., 3.14)
    AST_STRING_LITERAL, // String literal (e.g., "hello")
    AST_BOOL_LITERAL, // Boolean literal (true/false)
    AST_VAR_DECL, // Variable declaration (e.g., int x = 5)
    AST_ASSIGNMENT, // Assignment (e.g., x = 10)
    AST_VAR_REF, // Variable reference (e.g., x)
    AST_BINARY_OP, // Binary operation (e.g., a + b)
    AST_UNARY_OP, // Unary operation (e.g., -a, !b)
    AST_IF, // If statement
    AST_WHILE, // While loop
    AST_BLOCK, // Block of statements
    AST_FUNC_DEF, // Function definition
    AST_FUNC_CALL, // Function call
    AST_RETURN, // Return statement
    AST_PRINT, // Print statement
    AST_FOR, // C-style for loop
    AST_FOREACH, // Foreach loop
    AST_DO_WHILE, // Do-while loop
    AST_SWITCH, // Switch statement
    AST_CASE, // Case label
    AST_DEFAULT, // Default label
    AST_BREAK, // Break statement
    AST_CONTINUE, // Continue statement
    AST_IMPORT, // Import statement
    AST_CLASS_DEF, // Class definition
    AST_NEW_EXPR, // Object instantiation (new Class())
    AST_FIELD_ACCESS, // Field access (obj.field)
    AST_FIELD_DECL, // Field declaration in class
    AST_LIST_LITERAL, // List literal ([1, 2, 3])
    AST_INDEX_ACCESS, // Index access (list[0])
    AST_ARRAY_SPECIFIER, // Array size specifier (int[5])
    AST_HASHMAP_LITERAL // Hashmap literal ({ "a": 1 })
} ASTNodeType;

/**
 * @brief Operator codes for binary and unary operations.
 *
 * Resolved once from the operator string at parse time (see create_node) so the
 * interpreter can dispatch with a switch() instead of a chain of strcmp() calls
 * on every evaluation. OP_NONE marks a node that carries no operator.
 */
typedef enum
{
    OP_NONE = 0,
    // Arithmetic
    OP_ADD, // +
    OP_SUB, // -  (also unary negation)
    OP_MUL, // *
    OP_DIV, // /
    OP_MOD, // %
    OP_POW, // ^
    // Comparison
    OP_LT, // <
    OP_GT, // >
    OP_LE, // <=
    OP_GE, // >=
    OP_EQ, // ==
    OP_NE, // !=
    // Logical
    OP_AND, // and
    OP_OR, // or
    OP_NOT // !  (unary)
} OpCode;

/**
 * @brief Structure representing a node in the Abstract Syntax Tree.
 */
typedef struct ASTNode
{
    ASTNodeType type; // Type of the node
    char *value; // String value (name, literal, operator)
    char *type_name; // Type name for declarations (e.g., "int", "list<int>")
    char *parent_class_name; // Parent class name for inheritance
    struct ASTNode **children; // Array of child nodes
    int children_count; // Number of children
    char **args; // Array of argument names (for functions)
    int arg_count; // Number of arguments
    int line_num; // Source line number

    // --- Precomputed caches (populated by create_node, read by the interpreter) ---
    // These let the interpreter avoid re-parsing literals and re-matching operator
    // strings on every single evaluation of a node.
    OpCode op; // Resolved operator for AST_BINARY_OP / AST_UNARY_OP; OP_NONE otherwise.
    long int_value; // Parsed value for AST_INT_LITERAL and AST_BOOL_LITERAL (0/1).
    double float_value; // Parsed value for AST_FLOAT_LITERAL.

    // --- Variable resolution (populated by the resolver pass, see resolver.c) ---
    // For AST_VAR_REF, AST_VAR_DECL, and AST_FOREACH loop variables:
    int is_local; // 1 => a function-local addressed by (var_depth, var_slot); 0 => global/by-name.
    int var_depth; // Number of enclosing function frames to hop (0 = current frame).
    int var_slot; // Slot index within that frame.
    // For AST_FUNC_DEF: the number of slots its activation frame needs.
    int frame_slots;
} ASTNode;

/**
 * @brief State of the parser.
 */
typedef struct
{
    TokenizerState *tokenizer_state; // Pointer to tokenizer output
    int current_token; // Index of the current token being processed
} ParserState;

/**
 * @brief Parses a program from a token stream.
 * @param state The parser state initialized with tokens.
 * @return The root AST node of the parsed program.
 */
ASTNode *parse_program(ParserState *state);

/**
 * @brief Frees the memory associated with an AST.
 * @param node The root node of the AST to free.
 */
void free_ast(ASTNode *node);

#endif //PITH_PARSER_H
