/**
 * @file template_module.c
 * @brief Implementation of the native `template` module (see template_module.h).
 *
 * ## Pipeline
 *
 *   template text --lex--> text and tag items --parse--> node tree --render--> string
 *
 *  - Compiling (template.load / template.compile) lexes the source into text
 *    and tag items, parses tag expressions, builds a tree of nodes (text,
 *    output, if, each, partial), and loads partials recursively. Everything
 *    is allocated in one arena owned by the Template, freed in one go. Syntax
 *    errors longjmp back to the entry point, which frees the arena and raises
 *    a Pith error naming file:line:column.
 *  - Rendering walks the tree with a small evaluator for the expression
 *    subset, writing into a Pith `buffer` (a GC object, so nothing leaks if an
 *    error interrupts rendering). The whole render runs under
 *    interpreter_run_protected(), so any error (type mismatch, unknown name, or
 *    one raised inside a helper) is re-raised prefixed with the template
 *    location being evaluated at the time.
 *  - Binary operators are evaluated by the interpreter's own
 *    apply_binary_operator(), so they behave exactly as in Pith.
 *
 * ## Memory / GC
 *
 * A Template's registered helpers live in a Pith map referenced from its C
 * data; the native class's mark hook keeps it alive. During rendering, the
 * output buffer, every collection being iterated, and operands awaiting their
 * partner are GC-rooted.
 */

#include <ctype.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "template_module.h"
#include "interpreter.h"
#include "common.h"
#include "gc.h"
#include "native_object.h"
#include "pith_buffer.h"
#include "pith_string.h"
#include "types.h"

#define MAX_LOOP_NESTING 64 // {{#each}} inside {{#each}} ...
#define MAX_PARTIAL_NESTING 32 // Partials including partials; also stops include cycles.
#define MAX_BLOCK_NESTING 100 // {{#if}} / {{#each}} blocks inside each other
#define MAX_EXPRESSION_NESTING 100 // Parentheses, unary operators and [ ] inside each other

// --- Arena: every allocation of a compiled template, freed together ---

typedef struct
{
    void **blocks;
    int count;
    int capacity;
} Arena;

static void *arena_allocate(Arena *arena, size_t size)
{
    if (arena->count == arena->capacity)
    {
        arena->capacity = arena->capacity ? arena->capacity * 2 : 64;
        arena->blocks = realloc(arena->blocks, sizeof(void *) * (size_t) arena->capacity);
    }
    void *block = calloc(1, size > 0 ? size : 1);
    arena->blocks[arena->count++] = block;
    return block;
}

static char *arena_copy_text(Arena *arena, const char *text, size_t length)
{
    char *copy = arena_allocate(arena, length + 1);
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

static void arena_free(Arena *arena)
{
    for (int i = 0; i < arena->count; i++)
        free(arena->blocks[i]);
    free(arena->blocks);
    arena->blocks = NULL;
    arena->count = arena->capacity = 0;
}

// --- Source locations ---

typedef struct
{
    const char *file; // Arena-owned file name (or "<string>")
    int line;
    int column;
} SourceLocation;

/**
 * @brief One template source being compiled, with incremental line tracking.
 */
typedef struct
{
    const char *text;
    int length;
    const char *file;
    const char *directory; // For finding partials
    const char *extension; // Added to partial names without one, e.g. ".html"
    // Line/column of `known_position`, advanced incrementally by locate().
    int known_position;
    int known_line;
    int known_column;
} SourceFile;

static SourceLocation locate(SourceFile *source, int position)
{
    if (position < source->known_position)
    {
        source->known_position = 0;
        source->known_line = 1;
        source->known_column = 1;
    }
    for (; source->known_position < position && source->known_position < source->length; source->known_position++)
    {
        if (source->text[source->known_position] == '\n')
        {
            source->known_line++;
            source->known_column = 1;
        }
        else
            source->known_column++;
    }
    SourceLocation where = {source->file, source->known_line, source->known_column};
    return where;
}

// --- Compile session and errors ---

typedef struct
{
    Arena *arena; // The template's arena (nodes, expressions, names)
    jmp_buf fail;
    char error[800];
    int partial_depth;
} CompileSession;

static void compile_fail(CompileSession *session, SourceFile *source, int position, const char *format, ...)
{
    SourceLocation where = locate(source, position);
    int prefix = snprintf(session->error, sizeof(session->error), "%s:%d:%d: ", where.file, where.line,
                          where.column);
    va_list args;
    va_start(args, format);
    vsnprintf(session->error + prefix, sizeof(session->error) - (size_t) prefix, format, args);
    va_end(args);
    longjmp(session->fail, 1);
}

// --- Expressions ---

typedef enum
{
    EXPRESSION_INT,
    EXPRESSION_FLOAT,
    EXPRESSION_STRING,
    EXPRESSION_BOOL,
    EXPRESSION_VOID,
    EXPRESSION_NAME, // A data value or loop variable
    EXPRESSION_LOOP_INFO, // @index, @first, @last
    EXPRESSION_FIELD, // object.name
    EXPRESSION_INDEX, // object[index]
    EXPRESSION_LENGTH, // object.len()
    EXPRESSION_NOT, // !operand
    EXPRESSION_NEGATE, // -operand
    EXPRESSION_AND,
    EXPRESSION_OR,
    EXPRESSION_BINARY, // + - * / % == != < > <= >=
    EXPRESSION_HELPER_CALL // helper arg arg ...
} ExpressionKind;

typedef struct Expression
{
    ExpressionKind kind;
    SourceLocation where;
    int64_t int_value; // INT, BOOL
    double float_value; // FLOAT
    char *text; // STRING bytes; NAME / FIELD / LOOP_INFO / HELPER_CALL name
    int text_length; // STRING length
    OpCode op; // BINARY
    char operator_text[4]; // BINARY, for error messages
    struct Expression *left; // Operand / object
    struct Expression *right; // Second operand / index
    struct Expression **arguments; // HELPER_CALL
    int argument_count;
} Expression;

typedef enum
{
    EXPRESSION_TOKEN_END,
    EXPRESSION_TOKEN_NAME,
    EXPRESSION_TOKEN_LOOP_INFO,
    EXPRESSION_TOKEN_INT,
    EXPRESSION_TOKEN_FLOAT,
    EXPRESSION_TOKEN_STRING,
    EXPRESSION_TOKEN_OPERATOR,
    EXPRESSION_TOKEN_OPEN_PAREN,
    EXPRESSION_TOKEN_CLOSE_PAREN,
    EXPRESSION_TOKEN_DOT,
    EXPRESSION_TOKEN_OPEN_BRACKET,
    EXPRESSION_TOKEN_CLOSE_BRACKET
} ExpressionTokenKind;

typedef struct
{
    ExpressionTokenKind kind;
    int start; // Position in the source file
    int length;
} ExpressionToken;

/**
 * @brief Parser state for the expression inside one tag.
 */
typedef struct
{
    CompileSession *session;
    SourceFile *source;
    int position; // Next unread character
    int end; // End of the tag's expression text
    ExpressionToken current;
    int depth; // Nested ( ), [ ] and unary operators (see MAX_EXPRESSION_NESTING)
} ExpressionParser;

static int token_is(ExpressionParser *parser, ExpressionToken token, const char *text)
{
    size_t length = strlen(text);
    return (size_t) token.length == length && strncmp(parser->source->text + token.start, text, length) == 0;
}

static int is_keyword(ExpressionParser *parser, ExpressionToken token)
{
    if (token.kind != EXPRESSION_TOKEN_NAME)
        return 0;
    return token_is(parser, token, "and") || token_is(parser, token, "or") || token_is(parser, token, "as") ||
           token_is(parser, token, "true") || token_is(parser, token, "false") || token_is(parser, token, "void");
}

/**
 * @brief Reads the next token starting at @p position (without consuming it).
 */
static ExpressionToken scan_token(ExpressionParser *parser, int position)
{
    const char *text = parser->source->text;
    while (position < parser->end && isspace((unsigned char) text[position]))
        position++;
    ExpressionToken token = {EXPRESSION_TOKEN_END, position, 0};
    if (position >= parser->end)
        return token;

    char c = text[position];
    int end = position + 1;
    if (isalpha((unsigned char) c) || c == '_' || c == '@')
    {
        while (end < parser->end && (isalnum((unsigned char) text[end]) || text[end] == '_'))
            end++;
        token.kind = c == '@' ? EXPRESSION_TOKEN_LOOP_INFO : EXPRESSION_TOKEN_NAME;
        if (c == '@' && end == position + 1)
            compile_fail(parser->session, parser->source, position, "expected a name after '@'");
    }
    else if (isdigit((unsigned char) c))
    {
        token.kind = EXPRESSION_TOKEN_INT;
        while (end < parser->end && isdigit((unsigned char) text[end]))
            end++;
        if (end + 1 < parser->end && text[end] == '.' && isdigit((unsigned char) text[end + 1]))
        {
            token.kind = EXPRESSION_TOKEN_FLOAT;
            end++;
            while (end < parser->end && isdigit((unsigned char) text[end]))
                end++;
        }
    }
    else if (c == '"' || c == '\'')
    {
        token.kind = EXPRESSION_TOKEN_STRING;
        while (end < parser->end && text[end] != c)
            end += text[end] == '\\' ? 2 : 1;
        if (end >= parser->end)
            compile_fail(parser->session, parser->source, position, "unterminated string");
        end++; // Closing quote
    }
    else if (c == '(' || c == ')' || c == '.' || c == '[' || c == ']')
        token.kind = c == '(' ? EXPRESSION_TOKEN_OPEN_PAREN : c == ')' ? EXPRESSION_TOKEN_CLOSE_PAREN
                     : c == '.' ? EXPRESSION_TOKEN_DOT : c == '[' ? EXPRESSION_TOKEN_OPEN_BRACKET
                     : EXPRESSION_TOKEN_CLOSE_BRACKET;
    else if (strchr("=!<>", c) != NULL && end < parser->end && text[end] == '=')
    {
        token.kind = EXPRESSION_TOKEN_OPERATOR; // == != <= >=
        end++;
    }
    else if (strchr("<>+-*/%!", c) != NULL)
        token.kind = EXPRESSION_TOKEN_OPERATOR;
    else
        compile_fail(parser->session, parser->source, position, "unexpected character '%c' in expression", c);
    token.length = end - position;
    return token;
}

static void advance_token(ExpressionParser *parser)
{
    parser->position = parser->current.start + parser->current.length;
    parser->current = scan_token(parser, parser->position);
}

static ExpressionToken peek_after_current(ExpressionParser *parser)
{
    return scan_token(parser, parser->current.start + parser->current.length);
}

static Expression *new_expression(ExpressionParser *parser, ExpressionKind kind, int position)
{
    Expression *expression = arena_allocate(parser->session->arena, sizeof(Expression));
    expression->kind = kind;
    expression->where = locate(parser->source, position);
    return expression;
}

static Expression *parse_or(ExpressionParser *parser);
static Expression *parse_tag_expression(ExpressionParser *parser);

/**
 * @brief Decodes a string literal token (with \" \' \\ \n \t escapes) into the arena.
 */
static Expression *parse_string_literal(ExpressionParser *parser, ExpressionToken token)
{
    const char *text = parser->source->text;
    Expression *expression = new_expression(parser, EXPRESSION_STRING, token.start);
    char *bytes = arena_allocate(parser->session->arena, (size_t) token.length);
    int length = 0;
    for (int i = token.start + 1; i < token.start + token.length - 1; i++)
    {
        if (text[i] != '\\')
        {
            bytes[length++] = text[i];
            continue;
        }
        char escaped = text[++i];
        bytes[length++] = escaped == 'n' ? '\n' : escaped == 't' ? '\t' : escaped;
    }
    expression->text = bytes;
    expression->text_length = length;
    return expression;
}

static Expression *parse_primary(ExpressionParser *parser)
{
    ExpressionToken token = parser->current;
    switch (token.kind)
    {
        case EXPRESSION_TOKEN_INT:
        case EXPRESSION_TOKEN_FLOAT:
        {
            char number[64];
            snprintf(number, sizeof(number), "%.*s", token.length, parser->source->text + token.start);
            Expression *expression = new_expression(parser, token.kind == EXPRESSION_TOKEN_INT ? EXPRESSION_INT
                                                                                                : EXPRESSION_FLOAT,
                                                    token.start);
            expression->int_value = strtoll(number, NULL, 10);
            expression->float_value = strtod(number, NULL);
            advance_token(parser);
            return expression;
        }
        case EXPRESSION_TOKEN_STRING:
        {
            Expression *expression = parse_string_literal(parser, token);
            advance_token(parser);
            return expression;
        }
        case EXPRESSION_TOKEN_LOOP_INFO:
        {
            Expression *expression = new_expression(parser, EXPRESSION_LOOP_INFO, token.start);
            expression->text = arena_copy_text(parser->session->arena, parser->source->text + token.start + 1,
                                               (size_t) token.length - 1);
            if (strcmp(expression->text, "index") != 0 && strcmp(expression->text, "first") != 0 &&
                strcmp(expression->text, "last") != 0)
                compile_fail(parser->session, parser->source, token.start,
                             "unknown loop variable '@%s' (use @index, @first or @last)", expression->text);
            advance_token(parser);
            return expression;
        }
        case EXPRESSION_TOKEN_OPEN_PAREN:
        {
            // Parentheses group an expression or a nested helper call
            // ("subexpression"): {{shout (str n) 2}}.
            advance_token(parser);
            Expression *inner = parse_tag_expression(parser);
            if (parser->current.kind != EXPRESSION_TOKEN_CLOSE_PAREN)
                compile_fail(parser->session, parser->source, parser->current.start, "expected ')'");
            advance_token(parser);
            return inner;
        }
        case EXPRESSION_TOKEN_NAME:
        {
            if (token_is(parser, token, "true") || token_is(parser, token, "false"))
            {
                Expression *expression = new_expression(parser, EXPRESSION_BOOL, token.start);
                expression->int_value = token_is(parser, token, "true");
                advance_token(parser);
                return expression;
            }
            if (token_is(parser, token, "void"))
            {
                Expression *expression = new_expression(parser, EXPRESSION_VOID, token.start);
                advance_token(parser);
                return expression;
            }
            if (is_keyword(parser, token))
                break;
            Expression *expression = new_expression(parser, EXPRESSION_NAME, token.start);
            expression->text = arena_copy_text(parser->session->arena, parser->source->text + token.start,
                                               (size_t) token.length);
            advance_token(parser);
            return expression;
        }
        default:
            break;
    }
    if (token.kind == EXPRESSION_TOKEN_END)
        compile_fail(parser->session, parser->source, token.start, "expected a value");
    compile_fail(parser->session, parser->source, token.start, "unexpected '%.*s'", token.length,
                 parser->source->text + token.start);
    return NULL;
}

/**
 * @brief primary, followed by any number of .name, .len() or [index].
 */
static Expression *parse_postfix(ExpressionParser *parser)
{
    Expression *expression = parse_primary(parser);
    for (;;)
    {
        if (parser->current.kind == EXPRESSION_TOKEN_DOT)
        {
            int dot = parser->current.start;
            advance_token(parser);
            ExpressionToken name = parser->current;
            if (name.kind != EXPRESSION_TOKEN_NAME)
                compile_fail(parser->session, parser->source, name.start, "expected a name after '.'");
            advance_token(parser);
            if (token_is(parser, name, "len") && parser->current.kind == EXPRESSION_TOKEN_OPEN_PAREN)
            {
                advance_token(parser);
                if (parser->current.kind != EXPRESSION_TOKEN_CLOSE_PAREN)
                    compile_fail(parser->session, parser->source, parser->current.start, "len() takes no arguments");
                advance_token(parser);
                Expression *length = new_expression(parser, EXPRESSION_LENGTH, dot);
                length->left = expression;
                expression = length;
                continue;
            }
            if (parser->current.kind == EXPRESSION_TOKEN_OPEN_PAREN)
                compile_fail(parser->session, parser->source, parser->current.start,
                             "templates can only call len(); use a helper for other functions");
            Expression *field = new_expression(parser, EXPRESSION_FIELD, name.start);
            field->left = expression;
            field->text = arena_copy_text(parser->session->arena, parser->source->text + name.start,
                                          (size_t) name.length);
            expression = field;
        }
        else if (parser->current.kind == EXPRESSION_TOKEN_OPEN_BRACKET)
        {
            Expression *index = new_expression(parser, EXPRESSION_INDEX, parser->current.start);
            advance_token(parser);
            index->left = expression;
            index->right = parse_or(parser);
            if (parser->current.kind != EXPRESSION_TOKEN_CLOSE_BRACKET)
                compile_fail(parser->session, parser->source, parser->current.start, "expected ']'");
            advance_token(parser);
            expression = index;
        }
        else
            return expression;
    }
}

static Expression *parse_unary(ExpressionParser *parser)
{
    // Every nested construct passes through here, so this bounds the recursion.
    if (++parser->depth > MAX_EXPRESSION_NESTING)
        compile_fail(parser->session, parser->source, parser->current.start, "expression is nested too deeply");
    if (parser->current.kind == EXPRESSION_TOKEN_OPERATOR && (token_is(parser, parser->current, "!") ||
                                                               token_is(parser, parser->current, "-")))
    {
        int position = parser->current.start;
        ExpressionKind kind = token_is(parser, parser->current, "!") ? EXPRESSION_NOT : EXPRESSION_NEGATE;
        advance_token(parser);
        Expression *expression = new_expression(parser, kind, position);
        expression->left = parse_unary(parser);
        parser->depth--;
        return expression;
    }
    Expression *expression = parse_postfix(parser);
    parser->depth--;
    return expression;
}

/**
 * @brief Parses a left-associative chain of the binary operators in @p operators
 *        (space-separated), each operand parsed by @p operand.
 */
static Expression *parse_binary_level(ExpressionParser *parser, Expression *(*operand)(ExpressionParser *),
                                      const char *const *operators, const OpCode *codes, int operator_count)
{
    Expression *left = operand(parser);
    for (;;)
    {
        if (parser->current.kind != EXPRESSION_TOKEN_OPERATOR)
            return left;
        int match = -1;
        for (int i = 0; i < operator_count && match < 0; i++)
        {
            if (token_is(parser, parser->current, operators[i]))
                match = i;
        }
        if (match < 0)
            return left;
        Expression *binary = new_expression(parser, EXPRESSION_BINARY, parser->current.start);
        snprintf(binary->operator_text, sizeof(binary->operator_text), "%s", operators[match]);
        binary->op = codes[match];
        advance_token(parser);
        binary->left = left;
        binary->right = operand(parser);
        left = binary;
    }
}

static Expression *parse_term(ExpressionParser *parser)
{
    static const char *const operators[] = {"*", "/", "%"};
    static const OpCode codes[] = {OP_MUL, OP_DIV, OP_MOD};
    return parse_binary_level(parser, parse_unary, operators, codes, 3);
}

static Expression *parse_sum(ExpressionParser *parser)
{
    static const char *const operators[] = {"+", "-"};
    static const OpCode codes[] = {OP_ADD, OP_SUB};
    return parse_binary_level(parser, parse_term, operators, codes, 2);
}

static Expression *parse_comparison(ExpressionParser *parser)
{
    static const char *const operators[] = {"<=", ">=", "<", ">"};
    static const OpCode codes[] = {OP_LE, OP_GE, OP_LT, OP_GT};
    return parse_binary_level(parser, parse_sum, operators, codes, 4);
}

static Expression *parse_equality(ExpressionParser *parser)
{
    static const char *const operators[] = {"==", "!="};
    static const OpCode codes[] = {OP_EQ, OP_NE};
    return parse_binary_level(parser, parse_comparison, operators, codes, 2);
}

static Expression *parse_and(ExpressionParser *parser)
{
    Expression *left = parse_equality(parser);
    while (parser->current.kind == EXPRESSION_TOKEN_NAME && token_is(parser, parser->current, "and"))
    {
        Expression *expression = new_expression(parser, EXPRESSION_AND, parser->current.start);
        advance_token(parser);
        expression->left = left;
        expression->right = parse_equality(parser);
        left = expression;
    }
    return left;
}

static Expression *parse_or(ExpressionParser *parser)
{
    Expression *left = parse_and(parser);
    while (parser->current.kind == EXPRESSION_TOKEN_NAME && token_is(parser, parser->current, "or"))
    {
        Expression *expression = new_expression(parser, EXPRESSION_OR, parser->current.start);
        advance_token(parser);
        expression->left = left;
        expression->right = parse_and(parser);
        left = expression;
    }
    return left;
}

/**
 * @brief True if @p token can start a helper argument.
 */
static int starts_argument(ExpressionParser *parser, ExpressionToken token)
{
    switch (token.kind)
    {
        case EXPRESSION_TOKEN_NAME:
            return !(token_is(parser, token, "and") || token_is(parser, token, "or") || token_is(parser, token, "as"));
        case EXPRESSION_TOKEN_LOOP_INFO:
        case EXPRESSION_TOKEN_INT:
        case EXPRESSION_TOKEN_FLOAT:
        case EXPRESSION_TOKEN_STRING:
        case EXPRESSION_TOKEN_OPEN_PAREN:
            return 1;
        default:
            return 0;
    }
}

/**
 * @brief Parses the expression of a tag: either a helper call
 *        (`name arg arg ...`) or an ordinary expression.
 */
static Expression *parse_tag_expression(ExpressionParser *parser)
{
    ExpressionToken first = parser->current;
    if (first.kind == EXPRESSION_TOKEN_NAME && !is_keyword(parser, first) &&
        starts_argument(parser, peek_after_current(parser)))
    {
        Expression *call = new_expression(parser, EXPRESSION_HELPER_CALL, first.start);
        call->text = arena_copy_text(parser->session->arena, parser->source->text + first.start,
                                     (size_t) first.length);
        advance_token(parser);
        Expression *arguments[64];
        while (starts_argument(parser, parser->current))
        {
            if (call->argument_count == 64)
                compile_fail(parser->session, parser->source, parser->current.start, "too many helper arguments");
            arguments[call->argument_count++] = parse_postfix(parser);
        }
        call->arguments = arena_allocate(parser->session->arena, sizeof(Expression *) * (size_t) call->argument_count);
        memcpy(call->arguments, arguments, sizeof(Expression *) * (size_t) call->argument_count);
        return call;
    }
    return parse_or(parser);
}

static void start_expression_parser(ExpressionParser *parser, CompileSession *session, SourceFile *source, int start,
                                    int end)
{
    parser->session = session;
    parser->source = source;
    parser->position = start;
    parser->end = end;
    parser->depth = 0;
    parser->current = scan_token(parser, start);
}

/**
 * @brief Parses a whole tag expression; nothing may follow it.
 */
static Expression *parse_complete_expression(CompileSession *session, SourceFile *source, int start, int end)
{
    ExpressionParser parser;
    start_expression_parser(&parser, session, source, start, end);
    if (parser.current.kind == EXPRESSION_TOKEN_END)
        compile_fail(session, source, start, "expected an expression");
    Expression *expression = parse_tag_expression(&parser);
    if (parser.current.kind != EXPRESSION_TOKEN_END)
        compile_fail(session, source, parser.current.start, "unexpected '%.*s' after the expression",
                     parser.current.length, source->text + parser.current.start);
    return expression;
}

// --- Lexing: text and tags ---

typedef enum
{
    ITEM_TEXT,
    ITEM_OUTPUT, // {{ expression }}
    ITEM_RAW, // {{raw expression}}
    ITEM_IF, // {{#if expression}}
    ITEM_ELSE_IF, // {{else if expression}}
    ITEM_ELSE, // {{else}}
    ITEM_END_IF, // {{/if}}
    ITEM_EACH, // {{#each expression as name}}
    ITEM_END_EACH, // {{/each}}
    ITEM_PARTIAL, // {{> name}}
    ITEM_COMMENT // {{! ... }}
} ItemKind;

typedef struct
{
    ItemKind kind;
    int start; // TEXT: the text; tags: the argument (expression, name, ...)
    int end;
    int tag_position; // Where the tag's "{{" is (for errors)
} Item;

typedef struct
{
    Item *items;
    int count;
    int capacity;
} ItemList;

static void add_item(CompileSession *session, ItemList *list, ItemKind kind, int start, int end, int tag_position)
{
    if (list->count == list->capacity)
    {
        // Item arrays are temporary but live in the arena, so a compile error
        // cannot leak them.
        int new_capacity = list->capacity ? list->capacity * 2 : 64;
        Item *grown = arena_allocate(session->arena, sizeof(Item) * (size_t) new_capacity);
        if (list->count > 0)
            memcpy(grown, list->items, sizeof(Item) * (size_t) list->count);
        list->items = grown;
        list->capacity = new_capacity;
    }
    Item item = {kind, start, end, tag_position};
    list->items[list->count++] = item;
}

static int skip_spaces(const char *text, int position, int end)
{
    while (position < end && isspace((unsigned char) text[position]))
        position++;
    return position;
}

static int starts_with_word(const char *text, int position, int end, const char *word)
{
    int length = (int) strlen(word);
    return position + length <= end && strncmp(text + position, word, (size_t) length) == 0 &&
           (position + length == end || !(isalnum((unsigned char) text[position + length]) ||
                                          text[position + length] == '_'));
}

/**
 * @brief Classifies the tag whose content is text[start..end).
 *
 * @param argument_start Receives where the tag's argument (expression, name, ...) starts.
 * @param argument_end Receives where it ends.
 * @return The item kind.
 */
static ItemKind classify_tag(CompileSession *session, SourceFile *source, int start, int end, int tag_position,
                             int *argument_start, int *argument_end)
{
    const char *text = source->text;
    while (end > start && isspace((unsigned char) text[end - 1]))
        end--;
    start = skip_spaces(text, start, end);
    if (start == end)
        compile_fail(session, source, tag_position, "empty tag {{}}");

    ItemKind kind;
    int argument = start;
    char first = text[start];
    if (first == '!')
        kind = ITEM_COMMENT;
    else if (first == '#' || first == '/')
    {
        int word = skip_spaces(text, start + 1, end);
        if (starts_with_word(text, word, end, "if"))
        {
            kind = first == '#' ? ITEM_IF : ITEM_END_IF;
            argument = word + 2;
        }
        else if (starts_with_word(text, word, end, "each"))
        {
            kind = first == '#' ? ITEM_EACH : ITEM_END_EACH;
            argument = word + 4;
        }
        else
        {
            int word_end = word;
            while (word_end < end && !isspace((unsigned char) text[word_end]))
                word_end++;
            compile_fail(session, source, tag_position, "unknown block tag '%c%.*s' (use #if or #each)", first,
                         word_end - word, text + word);
            return ITEM_TEXT; // Unreachable
        }
        if (first == '/' && skip_spaces(text, argument, end) != end)
            compile_fail(session, source, tag_position, "a closing tag takes nothing after its name");
    }
    else if (first == '>')
    {
        kind = ITEM_PARTIAL;
        argument = skip_spaces(text, start + 1, end);
    }
    else if (starts_with_word(text, start, end, "else"))
    {
        int rest = skip_spaces(text, start + 4, end);
        if (rest == end)
            kind = ITEM_ELSE;
        else if (starts_with_word(text, rest, end, "if"))
        {
            kind = ITEM_ELSE_IF;
            argument = rest + 2;
        }
        else
        {
            compile_fail(session, source, tag_position, "expected {{else}} or {{else if condition}}");
            return ITEM_TEXT; // Unreachable
        }
    }
    else if (starts_with_word(text, start, end, "raw") && skip_spaces(text, start + 3, end) != end)
    {
        kind = ITEM_RAW;
        argument = start + 3;
    }
    else
        kind = ITEM_OUTPUT;

    *argument_start = argument;
    *argument_end = end;
    return kind;
}

/**
 * @brief Splits a template source into text and tag items.
 *
 * A block-style tag that is the only thing on its line takes the whole line
 * with it (its indentation and line break), so block structure does not leave
 * blank lines in the output.
 */
static void lex_template(CompileSession *session, SourceFile *source, ItemList *items)
{
    const char *text = source->text;
    int length = source->length;
    int text_start = 0;
    int position = 0;
    while (position < length)
    {
        if (text[position] == '\\' && position + 2 < length && text[position + 1] == '{' && text[position + 2] == '{')
        {
            // \{{ is a literal "{{": keep the braces, drop the backslash.
            if (position > text_start)
                add_item(session, items, ITEM_TEXT, text_start, position, position);
            add_item(session, items, ITEM_TEXT, position + 1, position + 3, position);
            position += 3;
            text_start = position;
            continue;
        }
        if (!(text[position] == '{' && position + 1 < length && text[position + 1] == '{'))
        {
            position++;
            continue;
        }

        int tag_position = position;
        const char *close = NULL;
        for (int i = position + 2; i + 1 < length && close == NULL; i++)
        {
            if (text[i] == '}' && text[i + 1] == '}')
                close = text + i;
        }
        if (close == NULL)
            compile_fail(session, source, tag_position, "'{{' is never closed with '}}'");
        int content_start = position + 2;
        int content_end = (int) (close - text);
        int tag_end = content_end + 2;

        int argument_start, argument_end;
        ItemKind kind = classify_tag(session, source, content_start, content_end, tag_position, &argument_start,
                                     &argument_end);
        int text_end = tag_position;
        int next_position = tag_end;
        if (kind != ITEM_OUTPUT && kind != ITEM_RAW)
        {
            int line_start = tag_position;
            while (line_start > 0 && (text[line_start - 1] == ' ' || text[line_start - 1] == '\t'))
                line_start--;
            int after = tag_end;
            while (after < length && (text[after] == ' ' || text[after] == '\t' || text[after] == '\r'))
                after++;
            int alone_on_line = (line_start == 0 || text[line_start - 1] == '\n') &&
                                (after == length || text[after] == '\n');
            if (alone_on_line)
            {
                text_end = line_start;
                next_position = after < length ? after + 1 : after;
            }
        }
        if (text_end > text_start)
            add_item(session, items, ITEM_TEXT, text_start, text_end, text_start);
        add_item(session, items, kind, argument_start, argument_end, tag_position);
        position = text_start = next_position;
    }
    if (length > text_start)
        add_item(session, items, ITEM_TEXT, text_start, length, text_start);
}

// --- Nodes and parsing ---

typedef enum
{
    NODE_TEXT,
    NODE_OUTPUT,
    NODE_RAW_OUTPUT,
    NODE_IF,
    NODE_EACH,
    NODE_PARTIAL
} NodeKind;

typedef struct Node Node;

typedef struct
{
    Node **nodes;
    int count;
} NodeList;

struct Node
{
    NodeKind kind;
    SourceLocation where;
    const char *text; // TEXT (points into the arena-owned source copy)
    int text_length;
    Expression *expression; // OUTPUT, RAW_OUTPUT; EACH: the collection
    // IF: branch i renders bodies[i] when conditions[i] is true; else_body otherwise.
    Expression **conditions;
    NodeList *bodies;
    int branch_count;
    NodeList else_body; // IF, EACH (rendered when the collection is empty)
    const char *variable_name; // EACH
    NodeList body; // EACH: loop body; PARTIAL: the included template
};

typedef struct
{
    CompileSession *session;
    SourceFile *source;
    ItemList items;
    int next; // Next item to parse
    int block_depth; // Blocks currently open (see MAX_BLOCK_NESTING)
} NodeParser;

static Node *new_node(NodeParser *parser, NodeKind kind, int position)
{
    Node *node = arena_allocate(parser->session->arena, sizeof(Node));
    node->kind = kind;
    node->where = locate(parser->source, position);
    return node;
}

static NodeList compile_source(CompileSession *session, const char *file, const char *text, int length);

/**
 * @brief Loads and compiles the partial named @p name, relative to the including file.
 */
static NodeList compile_partial(NodeParser *parser, Item item)
{
    CompileSession *session = parser->session;
    SourceFile *source = parser->source;
    int name_length = item.end - item.start;
    if (name_length == 0)
        compile_fail(session, source, item.tag_position, "{{>}} needs a template name");
    for (int i = item.start; i < item.end; i++)
    {
        char c = source->text[i];
        if (!(isalnum((unsigned char) c) || c == '_' || c == '-' || c == '/' || c == '.'))
            compile_fail(session, source, item.tag_position, "invalid partial name '%.*s'", name_length,
                         source->text + item.start);
    }
    if (++session->partial_depth > MAX_PARTIAL_NESTING)
        compile_fail(session, source, item.tag_position,
                     "partials are nested more than %d levels deep (do two partials include each other?)",
                     MAX_PARTIAL_NESTING);

    // name -> <directory>/<name>[<extension>]
    const char *name = source->text + item.start;
    int has_extension = memchr(name, '.', (size_t) name_length) != NULL;
    size_t size = strlen(source->directory) + (size_t) name_length + strlen(source->extension) + 3;
    char *path = arena_allocate(session->arena, size);
    snprintf(path, size, "%s/%.*s%s", source->directory, name_length, name, has_extension ? "" : source->extension);

    long length = 0;
    char *text = read_file_bytes(path, &length);
    if (text == NULL)
        compile_fail(session, source, item.tag_position, "partial '%.*s' not found (looked for %s)", name_length,
                     name, path);
    char *copy = arena_copy_text(session->arena, text, (size_t) length);
    free(text);
    NodeList nodes = compile_source(session, path, copy, (int) length);
    session->partial_depth--;
    return nodes;
}

typedef enum
{
    STOP_AT_END, // Top level: parse to the end of the template
    STOP_IN_IF, // Inside {{#if}}: stop at else / else if / /if
    STOP_IN_EACH // Inside {{#each}}: stop at else / /each
} StopRule;

static NodeList parse_nodes(NodeParser *parser, StopRule rule, Item opener, Item *stopped_at);

static NodeList finish_list(NodeParser *parser, Node **nodes, int count)
{
    NodeList list;
    list.count = count;
    list.nodes = arena_allocate(parser->session->arena, sizeof(Node *) * (size_t) (count > 0 ? count : 1));
    if (count > 0)
        memcpy(list.nodes, nodes, sizeof(Node *) * (size_t) count);
    return list;
}

static Node *parse_if(NodeParser *parser, Item opener)
{
    CompileSession *session = parser->session;
    Node *node = new_node(parser, NODE_IF, opener.tag_position);
    Expression *conditions[256];
    NodeList bodies[256];
    Item stopped;

    conditions[0] = parse_complete_expression(session, parser->source, opener.start, opener.end);
    bodies[0] = parse_nodes(parser, STOP_IN_IF, opener, &stopped);
    node->branch_count = 1;
    while (stopped.kind == ITEM_ELSE_IF)
    {
        if (node->branch_count == 256)
            compile_fail(session, parser->source, stopped.tag_position, "too many {{else if}} branches");
        conditions[node->branch_count] = parse_complete_expression(session, parser->source, stopped.start,
                                                                   stopped.end);
        bodies[node->branch_count] = parse_nodes(parser, STOP_IN_IF, opener, &stopped);
        node->branch_count++;
    }
    if (stopped.kind == ITEM_ELSE)
    {
        node->else_body = parse_nodes(parser, STOP_IN_IF, opener, &stopped);
        if (stopped.kind != ITEM_END_IF)
            compile_fail(session, parser->source, stopped.tag_position,
                         "{{else}} has already been used in this {{#if}}; expected {{/if}}");
    }
    node->conditions = arena_allocate(session->arena, sizeof(Expression *) * (size_t) node->branch_count);
    node->bodies = arena_allocate(session->arena, sizeof(NodeList) * (size_t) node->branch_count);
    memcpy(node->conditions, conditions, sizeof(Expression *) * (size_t) node->branch_count);
    memcpy(node->bodies, bodies, sizeof(NodeList) * (size_t) node->branch_count);
    return node;
}

static Node *parse_each(NodeParser *parser, Item opener)
{
    CompileSession *session = parser->session;
    SourceFile *source = parser->source;
    Node *node = new_node(parser, NODE_EACH, opener.tag_position);

    // {{#each collection as name}}
    ExpressionParser expression_parser;
    start_expression_parser(&expression_parser, session, source, opener.start, opener.end);
    if (expression_parser.current.kind == EXPRESSION_TOKEN_END)
        compile_fail(session, source, opener.tag_position, "{{#each}} needs a list: {{#each items as item}}");
    node->expression = parse_tag_expression(&expression_parser);
    ExpressionToken as = expression_parser.current;
    if (!(as.kind == EXPRESSION_TOKEN_NAME && token_is(&expression_parser, as, "as")))
        compile_fail(session, source, as.start, "expected 'as' and a variable name: {{#each items as item}}");
    advance_token(&expression_parser);
    ExpressionToken name = expression_parser.current;
    if (name.kind != EXPRESSION_TOKEN_NAME || is_keyword(&expression_parser, name))
        compile_fail(session, source, name.start, "expected a variable name after 'as'");
    advance_token(&expression_parser);
    if (expression_parser.current.kind != EXPRESSION_TOKEN_END)
        compile_fail(session, source, expression_parser.current.start, "unexpected text after the variable name");
    node->variable_name = arena_copy_text(session->arena, source->text + name.start, (size_t) name.length);

    Item stopped;
    node->body = parse_nodes(parser, STOP_IN_EACH, opener, &stopped);
    if (stopped.kind == ITEM_ELSE)
    {
        node->else_body = parse_nodes(parser, STOP_IN_EACH, opener, &stopped);
        if (stopped.kind != ITEM_END_EACH)
            compile_fail(session, source, stopped.tag_position, "expected {{/each}} after the {{else}} part");
    }
    return node;
}

/**
 * @brief Parses items into nodes until the end of the template or, inside a
 *        block, until a tag that ends or divides that block.
 *
 * @param opener The block's opening tag (for "never closed" errors).
 * @param stopped_at Receives the tag that stopped parsing (inside blocks).
 */
static NodeList parse_nodes(NodeParser *parser, StopRule rule, Item opener, Item *stopped_at)
{
    CompileSession *session = parser->session;
    SourceFile *source = parser->source;
    // The node array grows in the arena (old copies are freed with the template),
    // so a compile error can never leak it and deep nesting uses no C stack for it.
    int capacity = 16;
    Node **nodes = arena_allocate(session->arena, sizeof(Node *) * (size_t) capacity);
    int count = 0;
    if (rule != STOP_AT_END && ++parser->block_depth > MAX_BLOCK_NESTING)
        compile_fail(session, source, opener.tag_position, "blocks are nested more than %d deep", MAX_BLOCK_NESTING);
    for (;;)
    {
        if (parser->next >= parser->items.count)
        {
            if (rule == STOP_AT_END)
                return finish_list(parser, nodes, count);
            compile_fail(session, source, opener.tag_position, "{{#%s}} is never closed with {{/%s}}",
                         rule == STOP_IN_IF ? "if" : "each", rule == STOP_IN_IF ? "if" : "each");
        }
        Item item = parser->items.items[parser->next++];
        if (count == capacity)
        {
            capacity *= 2;
            Node **grown = arena_allocate(session->arena, sizeof(Node *) * (size_t) capacity);
            memcpy(grown, nodes, sizeof(Node *) * (size_t) count);
            nodes = grown;
        }

        switch (item.kind)
        {
            case ITEM_TEXT:
            {
                Node *node = new_node(parser, NODE_TEXT, item.start);
                node->text = source->text + item.start;
                node->text_length = item.end - item.start;
                nodes[count++] = node;
                break;
            }
            case ITEM_OUTPUT:
            case ITEM_RAW:
            {
                Node *node = new_node(parser, item.kind == ITEM_OUTPUT ? NODE_OUTPUT : NODE_RAW_OUTPUT,
                                      item.tag_position);
                node->expression = parse_complete_expression(session, source, item.start, item.end);
                nodes[count++] = node;
                break;
            }
            case ITEM_COMMENT:
                break;
            case ITEM_PARTIAL:
            {
                Node *node = new_node(parser, NODE_PARTIAL, item.tag_position);
                node->body = compile_partial(parser, item);
                nodes[count++] = node;
                break;
            }
            case ITEM_IF:
                nodes[count++] = parse_if(parser, item);
                break;
            case ITEM_EACH:
                nodes[count++] = parse_each(parser, item);
                break;
            case ITEM_ELSE:
            case ITEM_ELSE_IF:
            case ITEM_END_IF:
            case ITEM_END_EACH:
            {
                int fits = (rule == STOP_IN_IF && item.kind != ITEM_END_EACH) ||
                           (rule == STOP_IN_EACH && (item.kind == ITEM_ELSE || item.kind == ITEM_END_EACH));
                if (!fits)
                {
                    const char *tag = item.kind == ITEM_ELSE ? "{{else}}" : item.kind == ITEM_ELSE_IF ? "{{else if}}"
                                      : item.kind == ITEM_END_IF ? "{{/if}}" : "{{/each}}";
                    if (rule == STOP_AT_END)
                        compile_fail(session, source, item.tag_position, "%s without a matching opening block", tag);
                    SourceLocation opened = locate(source, opener.tag_position);
                    compile_fail(session, source, item.tag_position, "%s does not belong to the {{#%s}} opened at line %d",
                                 tag, rule == STOP_IN_IF ? "if" : "each", opened.line);
                }
                *stopped_at = item;
                parser->block_depth--;
                return finish_list(parser, nodes, count);
            }
        }
    }
}

/**
 * @brief Compiles one template source (the main file or a partial) into nodes.
 */
static NodeList compile_source(CompileSession *session, const char *file, const char *text, int length)
{
    SourceFile *source = arena_allocate(session->arena, sizeof(SourceFile));
    source->text = text;
    source->length = length;
    source->file = arena_copy_text(session->arena, file, strlen(file));
    source->known_line = 1;
    source->known_column = 1;

    // Partials are looked up next to this file, with its extension by default.
    const char *slash = strrchr(file, '/');
    source->directory = slash ? arena_copy_text(session->arena, file, (size_t) (slash - file)) : ".";
    const char *base = slash ? slash + 1 : file;
    const char *dot = strrchr(base, '.');
    source->extension = dot && dot != base ? arena_copy_text(session->arena, dot, strlen(dot)) : ".html";

    NodeParser parser = {session, source, {NULL, 0, 0}, 0, 0};
    lex_template(session, source, &parser.items);
    Item none = {ITEM_TEXT, 0, 0, 0};
    return parse_nodes(&parser, STOP_AT_END, none, &none);
}

// --- The Template type ---

typedef struct
{
    Arena arena; // Owns every node, expression and source copy
    NodeList nodes;
    Value helpers; // Pith map: helper name -> function
} TemplateData;

static NativeClass *template_class;

static void template_finalize(void *data)
{
    TemplateData *compiled = (TemplateData *) data;
    arena_free(&compiled->arena);
    free(compiled);
}

static void template_mark(void *data)
{
    mark_value(((TemplateData *) data)->helpers);
}

/**
 * @brief Compiles @p text (named @p file in messages) into a new Template value.
 */
static Value make_template(const char *file, const char *text, int length)
{
    TemplateData *compiled = calloc(1, sizeof(TemplateData));
    CompileSession session;
    memset(&session, 0, sizeof(session));
    session.arena = &compiled->arena;
    if (setjmp(session.fail) != 0)
    {
        char message[sizeof(session.error)];
        memcpy(message, session.error, sizeof(message));
        arena_free(&compiled->arena);
        free(compiled);
        report_error(get_exec_error_line(), "%s", message);
    }
    char *copy = arena_copy_text(&compiled->arena, text, (size_t) length);
    compiled->nodes = compile_source(&session, file, copy, length);

    // The helper map is allocated before the Template object exists, so root it
    // across that allocation.
    compiled->helpers.type = VAL_HASHMAP;
    compiled->helpers.hashmap = hashmap_create(VAL_STRING, VAL_VOID);
    gc_push_value(compiled->helpers);
    Value result = native_object_new(template_class, compiled);
    gc_pop_root();
    return result;
}

// --- Rendering ---

typedef struct
{
    const char *name;
    Value value;
    int index; // @index
    int count; // Number of items, for @last
} LoopScope;

typedef struct
{
    TemplateData *compiled;
    HashMap *data;
    Buffer *output;
    LoopScope loops[MAX_LOOP_NESTING];
    int loop_depth;
    SourceLocation current; // What is being evaluated, for error messages
} RenderContext;

/** @brief Raises a render error; the location is added by the render wrapper. */
static void render_fail(const char *format, ...)
{
    char message[600];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    report_error(get_exec_error_line(), "%s", message);
}

static Value evaluate(RenderContext *context, Expression *expression);

static Value make_bool_value(int truth)
{
    Value v;
    v.type = VAL_BOOL;
    v.int_val = truth ? 1 : 0;
    return v;
}

static int require_bool(Value value, const char *what)
{
    if (value.type != VAL_BOOL)
        render_fail("%s must be a bool, got %s", what, type_describe_value(value));
    return (int) value.int_val;
}

static Value evaluate_name(RenderContext *context, Expression *expression)
{
    for (int i = context->loop_depth - 1; i >= 0; i--)
    {
        if (strcmp(context->loops[i].name, expression->text) == 0)
            return context->loops[i].value;
    }
    if (hashmap_has(context->data, expression->text))
        return hashmap_get(context->data, expression->text);
    render_fail("no value named '%s' (it is not in the data map or a loop variable)", expression->text);
    return (Value){VAL_VOID};
}

static Value evaluate_field(Value object, Expression *expression)
{
    if (object.type == VAL_HASHMAP)
    {
        if (!hashmap_has(object.hashmap, expression->text))
            render_fail("'%s' has no key '%s'", expression->left->kind == EXPRESSION_NAME ? expression->left->text
                                                                                        : "the map",
                        expression->text);
        return hashmap_get(object.hashmap, expression->text);
    }
    if (object.type == VAL_INSTANCE)
    {
        if (!hashmap_has(object.instance->fields, expression->text))
            render_fail("%s has no field '%s'", object.instance->pith_class->name, expression->text);
        return hashmap_get(object.instance->fields, expression->text);
    }
    render_fail("cannot read .%s of %s (only maps and objects have fields)", expression->text,
                type_describe_value(object));
    return (Value){VAL_VOID};
}

static Value evaluate_index(RenderContext *context, Expression *expression)
{
    Value object = evaluate(context, expression->left);
    gc_push_value(object);
    Value index = evaluate(context, expression->right);
    gc_pop_root();
    context->current = expression->where;
    if (object.type == VAL_LIST)
    {
        if (index.type != VAL_INT)
            render_fail("a list index must be an int, got %s", type_describe_value(index));
        if (index.int_val < 0 || index.int_val >= object.list->count)
            render_fail("list index %" PITH_INT_FMT " is out of range (length %d)", index.int_val, object.list->count);
        return object.list->items[index.int_val];
    }
    if (object.type == VAL_HASHMAP)
    {
        if (index.type != VAL_STRING)
            render_fail("a map key must be a string, got %s", type_describe_value(index));
        if (!hashmap_has(object.hashmap, AS_CSTRING(index)))
            render_fail("map has no key '%s'", AS_CSTRING(index));
        return hashmap_get(object.hashmap, AS_CSTRING(index));
    }
    if (object.type == VAL_STRING)
    {
        if (index.type != VAL_INT)
            render_fail("a string index must be an int, got %s", type_describe_value(index));
        int64_t position = index.int_val < 0 ? index.int_val + STRING_LENGTH(object) : index.int_val;
        if (position < 0 || position >= STRING_LENGTH(object))
            render_fail("string index %" PITH_INT_FMT " is out of range (length %d)", index.int_val,
                        STRING_LENGTH(object));
        gc_push_value(object);
        Value character = string_value(AS_CSTRING(object) + position, 1);
        gc_pop_root();
        return character;
    }
    render_fail("cannot index %s", type_describe_value(object));
    return (Value){VAL_VOID};
}

static Value evaluate_length(Value object)
{
    Value length;
    length.type = VAL_INT;
    switch (object.type)
    {
        case VAL_STRING: length.int_val = STRING_LENGTH(object); break;
        case VAL_LIST: length.int_val = object.list->count; break;
        case VAL_HASHMAP: length.int_val = object.hashmap->count; break;
        case VAL_BUFFER:
            buffer_require_live(object.buffer, 0);
            length.int_val = object.buffer->length;
            break;
        default:
            render_fail("%s has no len()", type_describe_value(object));
    }
    return length;
}

static Value evaluate_helper_call(RenderContext *context, Expression *expression)
{
    HashMap *helpers = context->compiled->helpers.hashmap;
    if (!hashmap_has(helpers, expression->text))
        render_fail("no helper named '%s' (register it with template.helper(\"%s\", function))", expression->text,
                    expression->text);
    Value function = hashmap_get(helpers, expression->text);
    Value arguments[64];
    int root_depth = gc_temp_root_depth();
    for (int i = 0; i < expression->argument_count; i++)
    {
        arguments[i] = evaluate(context, expression->arguments[i]);
        gc_push_value(arguments[i]); // Rooted until the helper returns.
    }
    context->current = expression->where;
    Value result = interpreter_call(function, arguments, expression->argument_count, get_exec_error_line());
    gc_pop_roots_to(root_depth);
    return result;
}

static Value evaluate(RenderContext *context, Expression *expression)
{
    check_stack_space(0);
    context->current = expression->where;
    Value value = {VAL_VOID};
    switch (expression->kind)
    {
        case EXPRESSION_INT:
            value.type = VAL_INT;
            value.int_val = expression->int_value;
            return value;
        case EXPRESSION_FLOAT:
            value.type = VAL_FLOAT;
            value.float_val = expression->float_value;
            return value;
        case EXPRESSION_STRING:
            return string_value(expression->text, expression->text_length);
        case EXPRESSION_BOOL:
            return make_bool_value((int) expression->int_value);
        case EXPRESSION_VOID:
            return value;
        case EXPRESSION_NAME:
            return evaluate_name(context, expression);
        case EXPRESSION_LOOP_INFO:
        {
            if (context->loop_depth == 0)
                render_fail("@%s is only available inside {{#each}}", expression->text);
            LoopScope *loop = &context->loops[context->loop_depth - 1];
            if (strcmp(expression->text, "index") == 0)
            {
                value.type = VAL_INT;
                value.int_val = loop->index;
                return value;
            }
            if (strcmp(expression->text, "first") == 0)
                return make_bool_value(loop->index == 0);
            return make_bool_value(loop->index == loop->count - 1);
        }
        case EXPRESSION_FIELD:
        {
            Value object = evaluate(context, expression->left);
            context->current = expression->where;
            return evaluate_field(object, expression);
        }
        case EXPRESSION_INDEX:
            return evaluate_index(context, expression);
        case EXPRESSION_LENGTH:
        {
            Value object = evaluate(context, expression->left);
            context->current = expression->where;
            return evaluate_length(object);
        }
        case EXPRESSION_NOT:
        {
            Value operand = evaluate(context, expression->left);
            context->current = expression->where;
            return make_bool_value(!require_bool(operand, "the operand of '!'"));
        }
        case EXPRESSION_NEGATE:
        {
            Value operand = evaluate(context, expression->left);
            context->current = expression->where;
            if (operand.type != VAL_INT && operand.type != VAL_FLOAT)
                render_fail("the operand of unary '-' must be a number, got %s", type_describe_value(operand));
            Value zero;
            zero.type = operand.type;
            zero.int_val = 0;
            zero.float_val = 0.0;
            if (operand.type == VAL_FLOAT)
            {
                operand.float_val = -operand.float_val;
                return operand;
            }
            return apply_binary_operator(OP_SUB, "-", get_exec_error_line(), zero, operand);
        }
        case EXPRESSION_AND:
        case EXPRESSION_OR:
        {
            // Short-circuit, like Pith: the right side runs only if needed.
            int is_and = expression->kind == EXPRESSION_AND;
            int left = require_bool(evaluate(context, expression->left), is_and ? "the left side of 'and'"
                                                                                 : "the left side of 'or'");
            if (is_and ? !left : left)
                return make_bool_value(left);
            int right = require_bool(evaluate(context, expression->right), is_and ? "the right side of 'and'"
                                                                                   : "the right side of 'or'");
            return make_bool_value(right);
        }
        case EXPRESSION_BINARY:
        {
            int root_depth = gc_temp_root_depth();
            Value left = evaluate(context, expression->left);
            gc_push_value(left);
            Value right = evaluate(context, expression->right);
            gc_push_value(right);
            context->current = expression->where;
            Value result = apply_binary_operator(expression->op, expression->operator_text, get_exec_error_line(),
                                                 left, right);
            gc_pop_roots_to(root_depth);
            return result;
        }
        case EXPRESSION_HELPER_CALL:
            return evaluate_helper_call(context, expression);
    }
    return value;
}

/**
 * @brief Appends @p text to the output, HTML-escaping it when @p escape is set.
 */
static void write_text(Buffer *output, const char *text, int length, int escape)
{
    if (!escape)
    {
        buffer_append_bytes(output, text, length);
        return;
    }
    int run_start = 0;
    for (int i = 0; i < length; i++)
    {
        const char *replacement = NULL;
        switch (text[i])
        {
            case '&': replacement = "&amp;"; break;
            case '<': replacement = "&lt;"; break;
            case '>': replacement = "&gt;"; break;
            case '"': replacement = "&quot;"; break;
            case '\'': replacement = "&#39;"; break;
            default: break;
        }
        if (replacement != NULL)
        {
            buffer_append_bytes(output, text + run_start, i - run_start);
            buffer_append_bytes(output, replacement, (int) strlen(replacement));
            run_start = i + 1;
        }
    }
    buffer_append_bytes(output, text + run_start, length - run_start);
}

/**
 * @brief Writes a value's text form ({{ }} and {{raw}}).
 */
static void write_value(RenderContext *context, Value value, int escape)
{
    switch (value.type)
    {
        case VAL_STRING:
            write_text(context->output, AS_CSTRING(value), STRING_LENGTH(value), escape);
            return;
        case VAL_INT:
        case VAL_FLOAT:
        case VAL_BOOL:
        case VAL_BUFFER:
        {
            Value text = native_str(1, &value); // The same text form as str()
            write_text(context->output, AS_CSTRING(text), STRING_LENGTH(text), escape);
            return;
        }
        case VAL_VOID:
            render_fail("the value is void (nothing to output); check for void with {{#if x != void}}");
            return;
        default:
            render_fail("cannot output %s; use {{#each}} to output its items", type_describe_value(value));
    }
}

static void render_nodes(RenderContext *context, NodeList nodes);

static void render_each(RenderContext *context, Node *node)
{
    Value collection = evaluate(context, node->expression);
    context->current = node->where;
    int root_depth = gc_temp_root_depth();
    gc_push_value(collection); // Keep the list (possibly a fresh one) alive for the whole loop.

    // A map is iterated by its keys, in insertion order, like Pith's foreach.
    if (collection.type == VAL_HASHMAP)
    {
        Value keys;
        keys.type = VAL_LIST;
        keys.list = (List *) allocate_obj(sizeof(List), OBJ_LIST);
        gc_push_value(keys);
        MapEntry **entries = hashmap_entries_in_order(collection.hashmap);
        for (int i = 0; i < collection.hashmap->count; i++)
            list_add(keys.list, cstring_value(entries[i]->key));
        free(entries);
        collection = keys;
    }
    if (collection.type != VAL_LIST)
        render_fail("{{#each}} needs a list or a map, got %s", type_describe_value(collection));
    if (context->loop_depth == MAX_LOOP_NESTING)
        render_fail("{{#each}} blocks are nested more than %d deep", MAX_LOOP_NESTING);

    List *items = collection.list;
    if (items->count == 0)
        render_nodes(context, node->else_body);
    for (int i = 0; i < items->count; i++)
    {
        LoopScope *loop = &context->loops[context->loop_depth++];
        loop->name = node->variable_name;
        loop->value = items->items[i];
        loop->index = i;
        loop->count = items->count;
        render_nodes(context, node->body);
        context->loop_depth--;
    }
    gc_pop_roots_to(root_depth);
}

static void render_nodes(RenderContext *context, NodeList nodes)
{
    for (int i = 0; i < nodes.count; i++)
    {
        Node *node = nodes.nodes[i];
        context->current = node->where;
        switch (node->kind)
        {
            case NODE_TEXT:
                buffer_append_bytes(context->output, node->text, node->text_length);
                break;
            case NODE_OUTPUT:
            case NODE_RAW_OUTPUT:
            {
                Value value = evaluate(context, node->expression);
                context->current = node->where;
                gc_push_value(value);
                write_value(context, value, node->kind == NODE_OUTPUT);
                gc_pop_root();
                break;
            }
            case NODE_IF:
            {
                int branch = 0;
                for (; branch < node->branch_count; branch++)
                {
                    Value condition = evaluate(context, node->conditions[branch]);
                    context->current = node->conditions[branch]->where;
                    if (require_bool(condition, "an {{#if}} condition"))
                        break;
                }
                render_nodes(context, branch < node->branch_count ? node->bodies[branch] : node->else_body);
                break;
            }
            case NODE_EACH:
                render_each(context, node);
                break;
            case NODE_PARTIAL:
                render_nodes(context, node->body);
                break;
        }
    }
}

/**
 * @brief Renders a whole template (run under interpreter_run_protected()).
 */
static void render_body(void *context)
{
    RenderContext *render = (RenderContext *) context;
    render_nodes(render, render->compiled->nodes);
}

// --- Pith functions and methods ---

// template.load(path) -> Template
static Value native_template_load(int arg_count, Value *args)
{
    if (arg_count != 1 || args[0].type != VAL_STRING)
        report_error(get_exec_error_line(), "template.load() takes exactly one string argument (the file path).");
    long length = 0;
    char *bytes = read_file_bytes(AS_CSTRING(args[0]), &length);
    if (bytes == NULL)
        report_error(get_exec_error_line(), "template.load(): could not read '%s'.", AS_CSTRING(args[0]));
    // Hold the text as a (GC-managed) Pith string, so a compile error, which
    // does not return here, cannot leak it.
    Value text = string_value(bytes, (int) length);
    free(bytes);
    gc_push_value(text);
    Value result = make_template(AS_CSTRING(args[0]), AS_CSTRING(text), STRING_LENGTH(text));
    gc_pop_root();
    return result;
}

// template.compile(text) -> Template
static Value native_template_compile(int arg_count, Value *args)
{
    if (arg_count != 1 || args[0].type != VAL_STRING)
        report_error(get_exec_error_line(), "template.compile() takes exactly one string argument (the template text).");
    return make_template("<string>", AS_CSTRING(args[0]), STRING_LENGTH(args[0]));
}

// template.escape(text) -> string
static Value native_template_escape(int arg_count, Value *args)
{
    if (arg_count != 1 || args[0].type != VAL_STRING)
        report_error(get_exec_error_line(), "template.escape() takes exactly one string argument.");
    Value output;
    output.type = VAL_BUFFER;
    output.buffer = buffer_new(STRING_LENGTH(args[0]));
    gc_push_value(output);
    write_text(output.buffer, AS_CSTRING(args[0]), STRING_LENGTH(args[0]), 1);
    Value result = string_value(output.buffer->data, output.buffer->length);
    gc_pop_root();
    return result;
}

// page.render(data) -> string
static Value method_render(int arg_count, Value *args)
{
    TemplateData *compiled = (TemplateData *) native_object_data(args[0], template_class, "render");
    if (arg_count != 2 || args[1].type != VAL_HASHMAP)
        report_error(get_exec_error_line(), "render() takes one argument: a map of values, e.g. render({\"title\": t}).");

    Value output;
    output.type = VAL_BUFFER;
    output.buffer = buffer_new(1024);
    gc_push_value(output);

    RenderContext context;
    memset(&context, 0, sizeof(context));
    context.compiled = compiled;
    context.data = args[1].hashmap;
    context.output = output.buffer;

    char error[1024];
    if (interpreter_run_protected(render_body, &context, error, sizeof(error)) != 0)
    {
        gc_pop_root();
        report_error(get_exec_error_line(), "%s:%d:%d: %s", context.current.file, context.current.line,
                     context.current.column, error);
    }
    Value result = string_value(output.buffer->data, output.buffer->length);
    gc_pop_root();
    return result;
}

// page.helper(name, function)
static Value method_helper(int arg_count, Value *args)
{
    TemplateData *compiled = (TemplateData *) native_object_data(args[0], template_class, "helper");
    if (arg_count != 3 || args[1].type != VAL_STRING)
        report_error(get_exec_error_line(), "helper() takes a name (string) and a function.");
    if (args[2].type != VAL_FUNC && args[2].type != VAL_NATIVE_FN)
        report_error(get_exec_error_line(), "helper(): the second argument must be a function, got %s.",
                     type_describe_value(args[2]));
    hashmap_set(compiled->helpers.hashmap, AS_CSTRING(args[1]), args[2], 0);
    return (Value){VAL_VOID};
}

// --- Registration ---

void template_register_module(HashMap *modules)
{
    template_class = native_class_create("Template", template_finalize, template_mark);
    native_class_add_method(template_class, "render", method_render);
    native_class_add_method(template_class, "helper", method_helper);

    HashMap *functions = hashmap_create(VAL_STRING, VAL_NATIVE_FN);
    register_native_method(functions, "load", native_template_load);
    register_native_method(functions, "compile", native_template_compile);
    register_native_method(functions, "escape", native_template_escape);

    Value module;
    module.type = VAL_HASHMAP;
    module.hashmap = functions;
    hashmap_set(modules, "template", module, 0);
}
