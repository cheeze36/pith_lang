/**
 * @file parser.c
 * @brief Implementation of the Pith language parser.
 *
 * This file contains the logic for parsing a stream of tokens into an Abstract Syntax Tree (AST).
 * It uses a recursive descent parsing strategy for statements and a Pratt parser for expressions.
 * The parser handles various language constructs including variable declarations, control flow,
 * function definitions, and class definitions.
 */

#include "parser.h"
#include "debug.h"
#include "common.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// --- Forward Declarations ---
ASTNode *parse_expression(ParserState *state);

ASTNode *parse_statement(ParserState *state);

ASTNode *parse_block(ParserState *state);

ASTNode *parse_function_definition(ParserState *state);

// --- Node Management ---

/**
 * @brief Creates a new AST node.
 *
 * Allocates memory for a new AST node and initializes its fields.
 *
 * @param type The type of the AST node (e.g., AST_VAR_DECL, AST_BINARY_OP).
 * @param value A string value associated with the node (e.g., variable name, literal value). Can be NULL.
 * @param line_num The line number in the source code where this node originated.
 * @return A pointer to the newly created ASTNode.
 */
ASTNode *create_node(ASTNodeType type, const char *value, int line_num)
{
    ASTNode *node = malloc(sizeof(ASTNode));
    if (!node)
    {
        fprintf(stderr, "Fatal: Memory allocation failed for AST node.\n");
        exit(1);
    }
    node->type = type;
    node->value = value ? strdup(value) : NULL;
    node->type_name = NULL;
    node->parent_class_name = NULL;
    node->children = NULL;
    node->children_count = 0;
    node->args = NULL;
    node->arg_count = 0;
    node->line_num = line_num;

    // DO NOT DISCARD DEBUG CODE
#ifdef DEBUG_DEEP_DIVE_PARSER
    printf("[DDP_CREATE] Created AST node %p of type %d with value '%s' at line %d\n", (void *) node, type,
           value ? value : "NULL", line_num);
#endif

#ifdef DEBUG_TRACE_ADVANCED_MEMORY
    printf("[MEMORY] Created AST node %p of type %d\n", (void *) node, type);
#endif
    return node;
}

/**
 * @brief Adds a child node to a parent AST node.
 *
 * Resizes the parent's children array and appends the new child.
 *
 * @param parent The parent AST node.
 * @param child The child AST node to add.
 */
void add_child(ASTNode *parent, ASTNode *child)
{
    if (!parent || !child)
        return;

    // DO NOT DISCARD DEBUG CODE
#ifdef DEBUG_DEEP_DIVE_PARSER
    printf("[DDP_LINK] Linking child %p to parent %p\n", (void *) child, (void *) parent);
#endif

    parent->children_count++;
    parent->children = realloc(parent->children, parent->children_count * sizeof(ASTNode *));
    if (!parent->children)
    {
        fprintf(stderr, "Fatal: Memory reallocation failed for AST children.\n");
        exit(1);
    }
    parent->children[parent->children_count - 1] = child;
}

/**
 * @brief Adds an argument name to a function or struct definition node.
 *
 * @param func_node The AST node representing the function or struct.
 * @param arg_name The name of the argument to add.
 */
void add_arg(ASTNode *func_node, const char *arg_name)
{
    if (!func_node || !arg_name)
        return;
#ifdef DEBUG_TRACE_PARSER_DETAIL
    printf("[PARSER_DETAIL] Adding arg '%s' to function/struct node\n", arg_name);
#endif
    func_node->arg_count++;
    func_node->args = realloc(func_node->args, func_node->arg_count * sizeof(char *));
    if (!func_node->args)
    {
        fprintf(stderr, "Fatal: Memory reallocation failed for function arguments.\n");
        exit(1);
    }
    func_node->args[func_node->arg_count - 1] = strdup(arg_name);
}

/**
 * @brief Recursively frees an AST node and its children.
 *
 * @param node The root of the AST subtree to free.
 */
void free_ast(ASTNode *node)
{
    if (!node)
        return;

    if (node->value)
        free(node->value);
    if (node->type_name)
        free(node->type_name);
    if (node->parent_class_name)
        free(node->parent_class_name);

    if (node->children)
    {
        for (int i = 0; i < node->children_count; i++)
        {
            free_ast(node->children[i]);
        }
        free(node->children);
    }

    if (node->args)
    {
        for (int i = 0; i < node->arg_count; i++)
        {
            free(node->args[i]);
        }
        free(node->args);
    }

    free(node);
}

// --- Parser State ---

/**
 * @brief Returns the current token without consuming it.
 *
 * @param state The current parser state.
 * @return The current Token.
 */
Token peek(ParserState *state)
{
    return state->tokenizer_state->tokens[state->current_token];
}

/**
 * @brief Returns the current token and advances the parser to the next token.
 *
 * @param state The current parser state.
 * @return The consumed Token.
 */
Token advance(ParserState *state)
{
    return state->tokenizer_state->tokens[state->current_token++];
}

/**
 * @brief Checks if the current token matches the expected type.
 *
 * If it matches, the token is consumed.
 *
 * @param state The current parser state.
 * @param type The expected token type.
 * @return 1 if matched and consumed, 0 otherwise.
 */
int match(ParserState *state, TokenType type)
{
    if (peek(state).type == type)
    {
        advance(state);
        return 1;
    }
    return 0;
}

// --- Expression Parsers (Pratt Parser) ---
ASTNode *parse_call(ParserState *state);

/**
 * @brief Parses primary expressions.
 *
 * Handles literals (numbers, strings, booleans), identifiers, parenthesized expressions,
 * list literals, map literals, and 'new' expressions for object instantiation.
 *
 * @param state The current parser state.
 * @return The AST node representing the primary expression.
 */
ASTNode *parse_primary(ParserState *state)
{
#ifdef DEBUG_TRACE_PARSER
    printf("[PARSER] Parsing primary expression\n");
#endif
    Token t = peek(state);

    // Handle 'new' keyword for object instantiation
    if (t.type == TOKEN_KEYWORD && strcmp(t.value, "new") == 0)
    {
        advance(state);
        ASTNode *call_node = parse_call(state);
        ASTNode *new_expr = create_node(AST_NEW_EXPR, NULL, t.line_num);
        add_child(new_expr, call_node);
        return new_expr;
    }

    // Handle literals
    if (t.type == TOKEN_NUMBER)
    {
        advance(state);
        return create_node(AST_INT_LITERAL, t.value, t.line_num);
    }
    if (t.type == TOKEN_FLOAT_LITERAL)
    {
        advance(state);
        return create_node(AST_FLOAT_LITERAL, t.value, t.line_num);
    }
    if (t.type == TOKEN_STRING)
    {
        advance(state);
        return create_node(AST_STRING_LITERAL, t.value, t.line_num);
    }
    if (t.type == TOKEN_KEYWORD && (strcmp(t.value, "true") == 0 || strcmp(t.value, "false") == 0))
    {
        advance(state);
        return create_node(AST_BOOL_LITERAL, t.value, t.line_num);
    }

    // Handle variable references
    if (t.type == TOKEN_IDENTIFIER)
    {
        advance(state);
        return create_node(AST_VAR_REF, t.value, t.line_num);
    }

    // Handle parenthesized expressions: ( expr )
    if (t.type == TOKEN_LPAREN)
    {
        advance(state);
        ASTNode *expr = parse_expression(state);
        match(state, TOKEN_RPAREN);
        return expr;
    }

    // Handle list literals: [ expr, expr, ... ]
    if (t.type == TOKEN_LBRACKET)
    {
        advance(state);
        ASTNode *list = create_node(AST_LIST_LITERAL, NULL, t.line_num);
        if (peek(state).type != TOKEN_RBRACKET)
        {
            add_child(list, parse_expression(state));
            while (match(state, TOKEN_COMMA))
            {
                add_child(list, parse_expression(state));
            }
        }
        match(state, TOKEN_RBRACKET);
        return list;
    }

    // Handle map literals: { key: value, ... }
    if (t.type == TOKEN_LBRACE)
    {
        advance(state);
        ASTNode *map = create_node(AST_HASHMAP_LITERAL, NULL, t.line_num);
        if (peek(state).type != TOKEN_RBRACE)
        {
            do
            {
                ASTNode *key = parse_expression(state);
                match(state, TOKEN_COLON);
                ASTNode *value = parse_expression(state);
                add_child(map, key);
                add_child(map, value);
            }
            while (match(state, TOKEN_COMMA));
        }
        match(state, TOKEN_RBRACE);
        return map;
    }
    return NULL;
}

/**
 * @brief Parses function calls, field access, and index access.
 *
 * This function handles postfix operators that appear after a primary expression,
 * such as `()` for calls, `.` for field access, and `[]` for indexing.
 *
 * @param state The current parser state.
 * @return The AST node representing the call or access.
 */
ASTNode *parse_call(ParserState *state)
{
    ASTNode *expr = parse_primary(state);
    while (1)
    {
        // Function call: expr(arg1, arg2, ...)
        if (peek(state).type == TOKEN_LPAREN)
        {
            Token t = advance(state);
            ASTNode *call = create_node(AST_FUNC_CALL, NULL, t.line_num);
            add_child(call, expr);
            if (peek(state).type != TOKEN_RPAREN)
            {
                do
                {
                    add_child(call, parse_expression(state));
                }
                while (match(state, TOKEN_COMMA));
            }
            match(state, TOKEN_RPAREN);
            expr = call;
        }
        // Field access: expr.member
        else if (peek(state).type == TOKEN_DOT)
        {
            Token t = advance(state);
            Token member_name = advance(state);
            ASTNode *access = create_node(AST_FIELD_ACCESS, member_name.value, t.line_num);
            add_child(access, expr);
            expr = access;
        }
        // Index access: expr[index]
        else if (peek(state).type == TOKEN_LBRACKET)
        {
            Token t = advance(state);
            ASTNode *index = parse_expression(state);
            match(state, TOKEN_RBRACKET);
            ASTNode *access = create_node(AST_INDEX_ACCESS, NULL, t.line_num);
            add_child(access, expr);
            add_child(access, index);
            expr = access;
        }
        else
        {
            break;
        }
    }
    return expr;
}

/**
 * @brief Parses unary operations.
 *
 * Handles prefix operators like `!` (not) and `-` (negation).
 *
 * @param state The current parser state.
 * @return The AST node representing the unary operation.
 */
ASTNode *parse_unary(ParserState *state)
{
    if (peek(state).type == TOKEN_BANG || peek(state).type == TOKEN_MINUS)
    {
        Token op = advance(state);
        ASTNode *operand = parse_unary(state);
        ASTNode *node = create_node(AST_UNARY_OP, op.value, op.line_num);
        add_child(node, operand);
        return node;
    }
    return parse_call(state);
}

/**
 * @brief Parses exponentiation operations.
 *
 * Handles the `^` operator.
 *
 * @param state The current parser state.
 * @return The AST node representing the power operation.
 */
ASTNode *parse_power(ParserState *state)
{
    ASTNode *left = parse_unary(state);
    while (peek(state).type == TOKEN_CARET)
    {
        Token op = advance(state);
        ASTNode *right = parse_unary(state);
        ASTNode *node = create_node(AST_BINARY_OP, op.value, op.line_num);
        add_child(node, left);
        add_child(node, right);
        left = node;
    }
    return left;
}

/**
 * @brief Parses multiplication, division, and modulo operations.
 *
 * Handles `*`, `/`, and `%`.
 *
 * @param state The current parser state.
 * @return The AST node representing the factor.
 */
ASTNode *parse_factor(ParserState *state)
{
    ASTNode *left = parse_power(state);
    while (peek(state).type == TOKEN_STAR || peek(state).type == TOKEN_SLASH || peek(state).type == TOKEN_PERCENT)
    {
        Token op = advance(state);
        ASTNode *right = parse_power(state);
        ASTNode *node = create_node(AST_BINARY_OP, op.value, op.line_num);
        add_child(node, left);
        add_child(node, right);
        left = node;
    }
    return left;
}

/**
 * @brief Parses addition and subtraction operations.
 *
 * Handles `+` and `-`.
 *
 * @param state The current parser state.
 * @return The AST node representing the term.
 */
ASTNode *parse_term(ParserState *state)
{
    ASTNode *left = parse_factor(state);
    while (peek(state).type == TOKEN_PLUS || peek(state).type == TOKEN_MINUS)
    {
        Token op = advance(state);
        ASTNode *right = parse_factor(state);
        ASTNode *node = create_node(AST_BINARY_OP, op.value, op.line_num);
        add_child(node, left);
        add_child(node, right);
        left = node;
    }
    return left;
}

/**
 * @brief Parses comparison operations.
 *
 * Handles `>`, `<`, `>=`, and `<=`.
 *
 * @param state The current parser state.
 * @return The AST node representing the comparison.
 */
ASTNode *parse_comparison(ParserState *state)
{
    ASTNode *left = parse_term(state);
    while (peek(state).type == TOKEN_GT || peek(state).type == TOKEN_LT ||
           peek(state).type == TOKEN_GTE || peek(state).type == TOKEN_LTE)
    {
        Token op = advance(state);
        ASTNode *right = parse_term(state);
        ASTNode *node = create_node(AST_BINARY_OP, op.value, op.line_num);
        add_child(node, left);
        add_child(node, right);
        left = node;
    }
    return left;
}

/**
 * @brief Parses equality checks.
 *
 * Handles `==` and `!=`.
 *
 * @param state The current parser state.
 * @return The AST node representing the equality check.
 */
ASTNode *parse_equality(ParserState *state)
{
    ASTNode *left = parse_comparison(state);
    while (peek(state).type == TOKEN_EQ || peek(state).type == TOKEN_NEQ)
    {
        Token op = advance(state);
        ASTNode *right = parse_comparison(state);
        ASTNode *node = create_node(AST_BINARY_OP, op.value, op.line_num);
        add_child(node, left);
        add_child(node, right);
        left = node;
    }
    return left;
}

/**
 * @brief Parses logical AND operations.
 *
 * Handles the `and` keyword.
 *
 * @param state The current parser state.
 * @return The AST node representing the logical AND.
 */
ASTNode *parse_logic_and(ParserState *state)
{
    ASTNode *left = parse_equality(state);
    while (peek(state).type == TOKEN_KEYWORD && strcmp(peek(state).value, "and") == 0)
    {
        Token op = advance(state);
        ASTNode *right = parse_equality(state);
        ASTNode *node = create_node(AST_BINARY_OP, "and", op.line_num);
        add_child(node, left);
        add_child(node, right);
        left = node;
    }
    return left;
}

/**
 * @brief Parses logical OR operations.
 *
 * Handles the `or` keyword. This is the lowest precedence expression parser.
 *
 * @param state The current parser state.
 * @return The AST node representing the logical OR.
 */
ASTNode *parse_logic_or(ParserState *state)
{
    ASTNode *left = parse_logic_and(state);
    while (peek(state).type == TOKEN_KEYWORD && strcmp(peek(state).value, "or") == 0)
    {
        Token op = advance(state);
        ASTNode *right = parse_logic_and(state);
        ASTNode *node = create_node(AST_BINARY_OP, "or", op.line_num);
        add_child(node, left);
        add_child(node, right);
        left = node;
    }
    return left;
}

/**
 * @brief Entry point for parsing an expression.
 *
 * @param state The current parser state.
 * @return The root AST node of the expression.
 */
ASTNode *parse_expression(ParserState *state)
{
#ifdef DEBUG_TRACE_PARSER
    printf("[PARSER] Parsing expression\n");
#endif
    return parse_logic_or(state);
}

// --- Statement & Block Parsers ---

/**
 * @brief Parses a block of code.
 *
 * A block is defined by indentation. It expects a colon, a newline, and then an indented block of statements.
 *
 * @param state The current parser state.
 * @return The AST node representing the block.
 */
ASTNode *parse_block(ParserState *state)
{
#ifdef DEBUG_TRACE_PARSER
    printf("[PARSER] Parsing block\n");
#endif
    Token t = peek(state);
    match(state, TOKEN_COLON);
    match(state, TOKEN_NEWLINE);
    match(state, TOKEN_INDENT);
    ASTNode *block = create_node(AST_BLOCK, NULL, t.line_num);

    // Parse statements until dedent or EOF
    while (peek(state).type != TOKEN_DEDENT && peek(state).type != TOKEN_EOF)
    {
        if (peek(state).type == TOKEN_NEWLINE)
        {
            advance(state);
            continue;
        }
        ASTNode *stmt = parse_statement(state);
        if (stmt)
            add_child(block, stmt);
        else
            advance(state);
    }
    match(state, TOKEN_DEDENT);
    return block;
}

/**
 * @brief Parses a function definition.
 *
 * Handles `define name(args): ...` or `define return_type name(args): ...`
 *
 * @param state The current parser state.
 * @return The AST node representing the function definition.
 */
ASTNode *parse_function_definition(ParserState *state)
{
#ifdef DEBUG_TRACE_PARSER
    printf("[PARSER] Parsing function definition\n");
#endif
    advance(state); // consume 'define'
    Token name;

    // Check if return type is specified
    if (state->tokenizer_state->tokens[state->current_token + 1].type == TOKEN_LPAREN)
    {
        name = advance(state);
    }
    else
    {
        advance(state); // consume return type (ignored for now in AST construction)
        // Handle generic return types like list[int]
        if (peek(state).type == TOKEN_LBRACKET)
        {
            advance(state);
            match(state, TOKEN_RBRACKET);
        }
        name = advance(state);
    }

    match(state, TOKEN_LPAREN);
    ASTNode *func = create_node(AST_FUNC_DEF, name.value, name.line_num);

    // Parse arguments
    if (peek(state).type != TOKEN_RPAREN)
    {
        do
        {
            if (state->tokenizer_state->tokens[state->current_token + 1].type == TOKEN_IDENTIFIER)
            {
                advance(state); // consume type (ignored for now)
            }
            Token arg_name = advance(state);
            add_arg(func, arg_name.value);
        }
        while (match(state, TOKEN_COMMA));
    }
    match(state, TOKEN_RPAREN);

    // Parse function body
    ASTNode *body = parse_block(state);
    add_child(func, body);
    return func;
}

/**
 * @brief Parses a single statement.
 *
 * Dispatches to specific parsing logic based on the current token (e.g., 'if', 'while', 'class', etc.).
 *
 * @param state The current parser state.
 * @return The AST node representing the statement.
 */
ASTNode *parse_statement(ParserState *state)
{
#ifdef DEBUG_TRACE_PARSER
    printf("[PARSER] Parsing statement\n");
#endif
    Token t = peek(state);

    // --- Class Definition ---
    if (t.type == TOKEN_KEYWORD && strcmp(t.value, "class") == 0)
    {
        advance(state);
        Token name = advance(state);
        ASTNode *class_node = create_node(AST_CLASS_DEF, name.value, name.line_num);

        // Handle inheritance
        if (peek(state).type == TOKEN_EXTENDS)
        {
            advance(state); // consume 'extends'
            Token parent_name = advance(state);
            class_node->parent_class_name = strdup(parent_name.value);
        }

        // Parse class body
        if (peek(state).type == TOKEN_COLON)
        {
            match(state, TOKEN_COLON);
            match(state, TOKEN_NEWLINE);
            match(state, TOKEN_INDENT);

            while (peek(state).type != TOKEN_DEDENT && peek(state).type != TOKEN_EOF)
            {
#ifdef DEBUG_TRACE_PARSER
                Token t = peek(state);
                printf("[PARSER_DEBUG] Class loop peek: type=%d, value='%s', line=%d\n", t.type,
                       t.value ? t.value : "NULL", t.line_num);
#endif

                // Skip any blank lines within the class definition
                if (peek(state).type == TOKEN_NEWLINE)
                {
#ifdef DEBUG_TRACE_PARSER
                    printf("[PARSER_DEBUG] Skipping NEWLINE in class loop\n");
#endif
                    advance(state);
                    continue;
                }

                // If the class body contains a 'pass' keyword, just consume it and continue.
                if (peek(state).type == TOKEN_KEYWORD && strcmp(peek(state).value, "pass") == 0)
                {
#ifdef DEBUG_TRACE_PARSER
                    printf("[PARSER_DEBUG] Skipping 'pass' in class body\n");
#endif
                    advance(state);
                    continue;
                }

                // Check if the line starts with 'define' for a method
                if (peek(state).type == TOKEN_KEYWORD && strcmp(peek(state).value, "define") == 0)
                {
#ifdef DEBUG_TRACE_PARSER
                    printf("[PARSER_DEBUG] Found method definition\n");
#endif
                    ASTNode *method_node = parse_function_definition(state);
                    add_child(class_node, method_node);
                }
                // Otherwise, parse it as a field declaration
                else
                {
#ifdef DEBUG_TRACE_PARSER
                    printf("[PARSER_DEBUG] Found field declaration\n");
#endif
                    Token type_name = advance(state);
                    char *full_type_name = strdup(type_name.value);

                    // Handle generic types (e.g., list<string>)
                    if ((strcmp(type_name.value, "list") == 0 || strcmp(type_name.value, "map") == 0) && peek(state).
                        type == TOKEN_LT)
                    {
                        match(state, TOKEN_LT);
                        Token inner_type1 = advance(state);
                        if (match(state, TOKEN_COMMA))
                        {
                            Token inner_type2 = advance(state);
                            match(state, TOKEN_GT);
                            char buffer[128];
                            snprintf(buffer, sizeof(buffer), "map<%s,%s>", inner_type1.value, inner_type2.value);
                            free(full_type_name);
                            full_type_name = strdup(buffer);
                        }
                        else
                        {
                            match(state, TOKEN_GT);
                            char buffer[64];
                            snprintf(buffer, sizeof(buffer), "list<%s>", inner_type1.value);
                            free(full_type_name);
                            full_type_name = strdup(buffer);
                        }
                    }

                    // Optional: handle array types like 'int[]' if needed
                    if (peek(state).type == TOKEN_LBRACKET)
                    {
                        advance(state);
                        match(state, TOKEN_RBRACKET);
                    }
                    Token field_name = advance(state);
                    ASTNode *field_node = create_node(AST_FIELD_DECL, field_name.value, field_name.line_num);
                    field_node->type_name = full_type_name;
                    add_child(class_node, field_node);
                }
            }
#ifdef DEBUG_TRACE_PARSER
            printf("[PARSER_DEBUG] Exiting class loop. Next token type=%d\n", peek(state).type);
#endif
            match(state, TOKEN_DEDENT);
        }
        return class_node;
    }
    // --- Function Definition ---
    else if (t.type == TOKEN_KEYWORD && strcmp(t.value, "define") == 0)
    {
        return parse_function_definition(state);
    }
    // --- Print Statement ---
    else if (t.type == TOKEN_KEYWORD && strcmp(t.value, "print") == 0)
    {
        advance(state);
        ASTNode *node = create_node(AST_PRINT, NULL, t.line_num);
        match(state, TOKEN_LPAREN);
        if (peek(state).type != TOKEN_RPAREN)
        {
            do
            {
                add_child(node, parse_expression(state));
            }
            while (match(state, TOKEN_COMMA));
        }
        match(state, TOKEN_RPAREN);
        return node;
    }
    // --- Import Statement ---
    else if (t.type == TOKEN_IMPORT)
    {
        advance(state);
        Token name = advance(state);
        return create_node(AST_IMPORT, name.value, name.line_num);
    }
    // --- Variable Declaration (Explicit Type) ---
    else if (t.type == TOKEN_KEYWORD && (strcmp(t.value, "int") == 0 || strcmp(t.value, "string") == 0 ||
                                         strcmp(t.value, "float") == 0 || strcmp(t.value, "bool") == 0 ||
                                         strcmp(t.value, "map") == 0 || strcmp(t.value, "list") == 0))
    {
        Token type_name = advance(state);
        char *full_type_name = strdup(type_name.value);

        // Handle generics for list and map
        if ((strcmp(type_name.value, "list") == 0 || strcmp(type_name.value, "map") == 0) && peek(state).type ==
            TOKEN_LT)
        {
            match(state, TOKEN_LT);
            Token inner_type1 = advance(state);
            if (match(state, TOKEN_COMMA))
            {
                Token inner_type2 = advance(state);
                match(state, TOKEN_GT);
                char buffer[128];
                snprintf(buffer, sizeof(buffer), "map<%s,%s>", inner_type1.value, inner_type2.value);
                free(full_type_name);
                full_type_name = strdup(buffer);
            }
            else
            {
                match(state, TOKEN_GT);
                char buffer[64];
                snprintf(buffer, sizeof(buffer), "list<%s>", inner_type1.value);
                free(full_type_name);
                full_type_name = strdup(buffer);
            }
        }

        ASTNode *array_spec = NULL;
        if (peek(state).type == TOKEN_LBRACKET)
        {
            advance(state);
            array_spec = create_node(AST_ARRAY_SPECIFIER, NULL, peek(state).line_num);
            if (peek(state).type == TOKEN_NUMBER)
            {
                Token size_token = advance(state);
                add_child(array_spec, create_node(AST_INT_LITERAL, size_token.value, size_token.line_num));
            }
            match(state, TOKEN_RBRACKET);
        }
        Token name = advance(state);
        ASTNode *node = create_node(AST_VAR_DECL, name.value, name.line_num);
        node->type_name = full_type_name;
        if (array_spec)
            add_child(node, array_spec);
        if (match(state, TOKEN_ASSIGN))
        {
            add_child(node, parse_expression(state));
        }
        return node;
    }
    // --- Variable Declaration (User-Defined Type) ---
    else if (peek(state).type == TOKEN_IDENTIFIER && state->tokenizer_state->tokens[state->current_token + 1].type ==
             TOKEN_IDENTIFIER)
    {
        Token type_name = advance(state);
        Token var_name = advance(state);
        ASTNode *node = create_node(AST_VAR_DECL, var_name.value, var_name.line_num);
        node->type_name = strdup(type_name.value);
        if (match(state, TOKEN_ASSIGN))
        {
            add_child(node, parse_expression(state));
        }
        else
        {
            add_child(node, create_node(AST_VAR_REF, type_name.value, type_name.line_num));
        }
        return node;
    }
    // --- If Statement ---
    else if (t.type == TOKEN_KEYWORD && strcmp(t.value, "if") == 0)
    {
        advance(state);
        ASTNode *node = create_node(AST_IF, NULL, t.line_num);
        add_child(node, parse_expression(state));
        add_child(node, parse_block(state));

        // Handle elif blocks
        ASTNode *current_if = node;
        while (peek(state).type == TOKEN_KEYWORD && strcmp(peek(state).value, "elif") == 0)
        {
            Token elif_token = advance(state);
            ASTNode *elif_node = create_node(AST_IF, NULL, elif_token.line_num);
            add_child(elif_node, parse_expression(state));
            add_child(elif_node, parse_block(state));
            add_child(current_if, elif_node);
            current_if = elif_node;
        }

        // Handle else block
        if (peek(state).type == TOKEN_KEYWORD && strcmp(peek(state).value, "else") == 0)
        {
            advance(state);
            add_child(current_if, parse_block(state));
        }
        return node;
    }
    // --- While Loop ---
    else if (t.type == TOKEN_KEYWORD && strcmp(t.value, "while") == 0)
    {
        advance(state);
        ASTNode *node = create_node(AST_WHILE, NULL, t.line_num);
        add_child(node, parse_expression(state));
        add_child(node, parse_block(state));
        return node;
    }
    // --- Foreach Loop ---
    else if (t.type == TOKEN_KEYWORD && strcmp(t.value, "foreach") == 0)
    {
        advance(state);
        match(state, TOKEN_LPAREN);

        Token type_name = advance(state);
        Token var_name = advance(state);

        if (peek(state).type != TOKEN_KEYWORD || strcmp(peek(state).value, "in") != 0)
        {
            report_error(t.line_num, "Expected 'in' keyword in foreach-loop.");
        }
        advance(state); // Consume 'in'

        ASTNode *collection = parse_expression(state);
        match(state, TOKEN_RPAREN);

        ASTNode *for_node = create_node(AST_FOREACH, var_name.value, t.line_num);
        for_node->type_name = strdup(type_name.value);
        add_child(for_node, collection);
        add_child(for_node, parse_block(state));
        return for_node;
    }
    // --- For Loop ---
    else if (t.type == TOKEN_KEYWORD && strcmp(t.value, "for") == 0)
    {
        advance(state);
        match(state, TOKEN_LPAREN);

        ASTNode *initializer = parse_statement(state);
        match(state, TOKEN_SEMICOLON);

        ASTNode *condition = parse_expression(state);
        match(state, TOKEN_SEMICOLON);

        ASTNode *increment = parse_statement(state);
        match(state, TOKEN_RPAREN);

        ASTNode *for_node = create_node(AST_FOR, NULL, t.line_num);
        add_child(for_node, initializer);
        add_child(for_node, condition);
        add_child(for_node, increment);
        add_child(for_node, parse_block(state));
        return for_node;
    }
    // --- Do-While Loop ---
    else if (t.type == TOKEN_KEYWORD && strcmp(t.value, "do") == 0)
    {
        advance(state);
        ASTNode *node = create_node(AST_DO_WHILE, NULL, t.line_num);
        add_child(node, parse_block(state));
        if (peek(state).type == TOKEN_KEYWORD && strcmp(peek(state).value, "while") == 0)
        {
            advance(state);
            match(state, TOKEN_LPAREN);
            add_child(node, parse_expression(state));
            match(state, TOKEN_RPAREN);
        }
        else
        {
            fprintf(stderr, "PARSER_ERROR: Expected 'while' after 'do' block on line %d.\n", t.line_num);
        }
        return node;
    }
    // --- Switch Statement ---
    else if (t.type == TOKEN_KEYWORD && strcmp(t.value, "switch") == 0)
    {
        advance(state);
        match(state, TOKEN_LPAREN);
        ASTNode *expr = parse_expression(state);
        match(state, TOKEN_RPAREN);

        ASTNode *switch_node = create_node(AST_SWITCH, NULL, t.line_num);
        add_child(switch_node, expr);

        match(state, TOKEN_COLON);
        match(state, TOKEN_NEWLINE);
        match(state, TOKEN_INDENT);

        while (peek(state).type != TOKEN_DEDENT && peek(state).type != TOKEN_EOF)
        {
            if (peek(state).type == TOKEN_NEWLINE)
            {
                advance(state);
                continue;
            }

            if (peek(state).type == TOKEN_KEYWORD && strcmp(peek(state).value, "case") == 0)
            {
                Token case_token = advance(state);
                ASTNode *case_node = create_node(AST_CASE, NULL, case_token.line_num);
                add_child(case_node, parse_expression(state));
                add_child(case_node, parse_block(state));
                add_child(switch_node, case_node);
            }
            else if (peek(state).type == TOKEN_KEYWORD && strcmp(peek(state).value, "default") == 0)
            {
                Token default_token = advance(state);
                ASTNode *default_node = create_node(AST_DEFAULT, NULL, default_token.line_num);
                add_child(default_node, parse_block(state));
                add_child(switch_node, default_node);
            }
            else
            {
                advance(state);
            }
        }
        match(state, TOKEN_DEDENT);
        return switch_node;
    }
    // --- Return Statement ---
    else if (t.type == TOKEN_KEYWORD && strcmp(t.value, "return") == 0)
    {
        advance(state);
        ASTNode *node = create_node(AST_RETURN, NULL, t.line_num);
        add_child(node, parse_expression(state));
        return node;
    }
    // --- Pass Statement ---
    else if (t.type == TOKEN_KEYWORD && strcmp(t.value, "pass") == 0)
    {
        advance(state);
        return create_node(AST_BLOCK, NULL, t.line_num); // Empty block does nothing
    }
    // --- Break Statement ---
    else if (t.type == TOKEN_KEYWORD && strcmp(t.value, "break") == 0)
    {
        advance(state);
        return create_node(AST_BREAK, NULL, t.line_num);
    }
    // --- Continue Statement ---
    else if (t.type == TOKEN_KEYWORD && strcmp(t.value, "continue") == 0)
    {
        advance(state);
        return create_node(AST_CONTINUE, NULL, t.line_num);
    }
    // --- Expression Statement or Assignment ---
    else
    {
        ASTNode *expr = parse_expression(state);
        if (expr)
        {
            if (match(state, TOKEN_ASSIGN))
            {
                ASTNode *right = parse_expression(state);
                ASTNode *node = create_node(AST_ASSIGNMENT, NULL, expr->line_num);
                add_child(node, expr);
                add_child(node, right);
                return node;
            }
            return expr;
        }
    }

    advance(state);
    return NULL;
}

/**
 * @brief Parses the entire program.
 *
 * Creates the root AST node and parses statements until EOF.
 *
 * @param state The current parser state.
 * @return The root AST node of the program.
 */
ASTNode *parse_program(ParserState *state)
{
#ifdef DEBUG_TRACE_PARSER
    printf("[PARSER] Parsing program\n");
#endif
    ASTNode *root = create_node(AST_PROGRAM, "root", 0);
    while (peek(state).type != TOKEN_EOF)
    {
        if (peek(state).type == TOKEN_NEWLINE)
        {
            advance(state);
            continue;
        }
        ASTNode *stmt = parse_statement(state);
        if (stmt)
            add_child(root, stmt);
    }
    return root;
}
