/**
 * @file regex_module.c
 * @brief Implementation of the native `regex` module (see regex_module.h).
 *
 * ## Pipeline
 *
 *   pattern text --parse--> syntax tree --compile--> program --Pike VM--> match
 *
 *  - The parser is recursive descent over the UTF-8 pattern. Errors longjmp to
 *    regex_compile(), which frees everything and then raises a Pith error that
 *    names the problem and its byte offset.
 *  - The program is a list of instructions for a tiny virtual machine: match a
 *    character / class / any character, SPLIT (try two paths, in priority
 *    order), JMP, SAVE (record a capture position), assertions, MATCH.
 *  - The Pike VM runs all alternative paths ("threads") in lockstep, one input
 *    character at a time, and never runs the same instruction twice at the
 *    same position. That bounds the work by program length x input length,
 *    for every pattern and every input: no catastrophic backtracking. Thread
 *    order encodes priority, which gives the usual leftmost-first results
 *    (greedy/lazy quantifiers and alternation behave as in Perl/Python).
 *
 * ## Memory / GC
 *
 * A Regex owns its program and scratch buffers (freed by its finalizer). A
 * Match keeps its subject string and its Regex alive through the native class
 * mark hook. Native method arguments are rooted by the interpreter; lists built
 * here (find_all, split, groups) are rooted while they are filled.
 */

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "regex_module.h"
#include "interpreter.h"
#include "common.h"
#include "gc.h"
#include "native_object.h"
#include "pith_string.h"
#include "types.h"

#define RX_MAX_PROGRAM 20000 // Instructions; bounds memory and the per-character work.
#define RX_MAX_REPEAT 1000 // Largest n or m in {n,m}.
#define RX_MAX_GROUPS 1000
#define RX_MAX_NESTING 250 // Groups inside groups; bounds the parser's and compiler's recursion.

// --- UTF-8 ---

/**
 * @brief Decodes the character at @p pos.
 *
 * Invalid UTF-8 decodes as the single byte's value, so binary data can still be
 * searched (every byte is consumed exactly once).
 *
 * @return The number of bytes consumed (1-4).
 */
static int utf8_decode(const char *s, int length, int pos, uint32_t *out)
{
    unsigned char b0 = (unsigned char) s[pos];
    int need = b0 < 0x80 ? 0 : (b0 >= 0xC2 && b0 <= 0xDF) ? 1 : (b0 >= 0xE0 && b0 <= 0xEF) ? 2
               : (b0 >= 0xF0 && b0 <= 0xF4) ? 3 : -1;
    if (need <= 0 || pos + need >= length) // ASCII, an invalid lead byte, or truncated at the end
    {
        *out = b0;
        return 1;
    }
    uint32_t cp = b0 & (0x3F >> need);
    for (int i = 1; i <= need; i++)
    {
        unsigned char b = (unsigned char) s[pos + i];
        if ((b & 0xC0) != 0x80)
        {
            *out = b0;
            return 1;
        }
        cp = (cp << 6) | (b & 0x3F);
    }
    *out = cp;
    return need + 1;
}

static uint32_t fold_case(uint32_t c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static int is_word_byte(int c)
{
    return c >= 0 && (isalnum(c) || c == '_');
}

// --- Data structures ---

typedef struct
{
    uint32_t lo, hi;
} RxRange;

typedef struct
{
    RxRange *ranges;
    int count;
    int capacity;
    int negated;
} RxClass;

typedef enum
{
    RX_OP_CHAR, // c (x = 1: compare case-insensitively; c is stored folded)
    RX_OP_ANY, // any character except '\n'
    RX_OP_ANY_NL, // any character
    RX_OP_CLASS, // classes[x]
    RX_OP_MATCH,
    RX_OP_JMP, // goto x
    RX_OP_SPLIT, // try x, then y
    RX_OP_SAVE, // caps[x] = position
    RX_OP_BOL, // ^
    RX_OP_EOL, // $
    RX_OP_WORD_BOUNDARY, // \b
    RX_OP_NOT_WORD_BOUNDARY // \B
} RxOp;

typedef struct
{
    RxOp op;
    int x, y;
    uint32_t c;
} RxInst;

typedef enum
{
    RX_NODE_EMPTY,
    RX_NODE_CHAR,
    RX_NODE_ANY,
    RX_NODE_CLASS,
    RX_NODE_CONCAT,
    RX_NODE_ALT,
    RX_NODE_REPEAT,
    RX_NODE_GROUP,
    RX_NODE_ASSERT
} RxNodeKind;

typedef struct RxNode
{
    RxNodeKind kind;
    uint32_t c; // CHAR
    int index; // CLASS: class index; GROUP: capture number (-1 = non-capturing); ASSERT: RxOp
    int min, max, greedy; // REPEAT (max -1 = unbounded)
    struct RxNode *a, *b; // CONCAT/ALT: both; REPEAT/GROUP: a
} RxNode;

/** @brief Thread list for the Pike VM: program counters plus their capture slots. */
typedef struct
{
    int *pcs;
    int *caps; // count * slot_count capture positions
    int count;
} RxThreadList;

typedef struct
{
    char *pattern; // Copy of the source pattern (NUL-terminated)
    int pattern_length;
    RxInst *program;
    int program_length;
    RxClass *classes;
    int class_count;
    int group_count; // Capturing groups, not counting group 0 (the whole match)
    char **group_names; // [group_count + 1]; NULL for unnamed groups
    int multiline;

    // Matching scratch space, sized for the program and reused by every search.
    int slot_count; // 2 * (group_count + 1)
    unsigned *marks; // Per instruction: generation in which it was last added to a list
    unsigned generation;
    RxThreadList lists[2];
    int *stack; // addthread work stack (3 ints per entry)
    int *scratch_caps; // Captures for a newly started thread
} Regex;

// --- Parser ---

typedef struct
{
    const char *src;
    int length;
    int pos;
    int icase, multiline, dotall;
    int group_count;
    int nesting_depth; // Groups currently open while parsing (see RX_MAX_NESTING)
    char **names;
    int names_capacity;
    RxClass *classes;
    int class_count, class_capacity;
    RxNode **nodes; // Every node, for freeing
    int node_count, node_capacity;
    jmp_buf fail;
    char error[256];
    int error_pos;
} RxParser;

static void rx_fail(RxParser *p, const char *message)
{
    snprintf(p->error, sizeof(p->error), "%s", message);
    p->error_pos = p->pos;
    longjmp(p->fail, 1);
}

static RxNode *rx_node(RxParser *p, RxNodeKind kind)
{
    if (p->node_count == p->node_capacity)
    {
        p->node_capacity = p->node_capacity ? p->node_capacity * 2 : 64;
        p->nodes = realloc(p->nodes, sizeof(RxNode *) * (size_t) p->node_capacity);
    }
    RxNode *n = calloc(1, sizeof(RxNode));
    n->kind = kind;
    p->nodes[p->node_count++] = n;
    return n;
}

static RxNode *rx_binary(RxParser *p, RxNodeKind kind, RxNode *a, RxNode *b)
{
    RxNode *n = rx_node(p, kind);
    n->a = a;
    n->b = b;
    return n;
}

static int rx_new_class(RxParser *p, int negated)
{
    if (p->class_count == p->class_capacity)
    {
        p->class_capacity = p->class_capacity ? p->class_capacity * 2 : 8;
        p->classes = realloc(p->classes, sizeof(RxClass) * (size_t) p->class_capacity);
    }
    RxClass *cls = &p->classes[p->class_count];
    memset(cls, 0, sizeof(*cls));
    cls->negated = negated;
    return p->class_count++;
}

static void rx_class_add(RxParser *p, int index, uint32_t lo, uint32_t hi)
{
    RxClass *cls = &p->classes[index];
    if (cls->count == cls->capacity)
    {
        cls->capacity = cls->capacity ? cls->capacity * 2 : 8;
        cls->ranges = realloc(cls->ranges, sizeof(RxRange) * (size_t) cls->capacity);
    }
    cls->ranges[cls->count].lo = lo;
    cls->ranges[cls->count].hi = hi;
    cls->count++;
}

/**
 * @brief Adds the ranges of shorthand @p letter (d, w, s or their capitals) to a class.
 */
static void rx_class_add_shorthand(RxParser *p, int index, char letter)
{
    static const RxRange digit[] = {{'0', '9'}};
    static const RxRange word[] = {{'0', '9'}, {'A', 'Z'}, {'_', '_'}, {'a', 'z'}};
    static const RxRange space[] = {{'\t', '\r'}, {' ', ' '}}; // \t \n \v \f \r and space
    const RxRange *ranges;
    int count;
    switch (tolower((unsigned char) letter)) {
        case 'd': ranges = digit; count = 1; break;
        case 'w': ranges = word; count = 4; break;
        default: ranges = space; count = 2; break;
    }
    if (islower((unsigned char) letter))
    {
        for (int i = 0; i < count; i++)
            rx_class_add(p, index, ranges[i].lo, ranges[i].hi);
        return;
    }
    // Capital letter: the complement of the (sorted, disjoint) ranges.
    uint32_t next = 0;
    for (int i = 0; i < count; i++)
    {
        if (ranges[i].lo > next)
            rx_class_add(p, index, next, ranges[i].lo - 1);
        next = ranges[i].hi + 1;
    }
    rx_class_add(p, index, next, 0x10FFFF);
}

static RxNode *rx_shorthand_node(RxParser *p, char letter)
{
    RxNode *n = rx_node(p, RX_NODE_CLASS);
    n->index = rx_new_class(p, 0);
    rx_class_add_shorthand(p, n->index, letter);
    return n;
}

static int rx_hex(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/**
 * @brief Parses a character escape (after the backslash) that denotes one character.
 *
 * @return 1 and the character in *out, or 0 if the escape is not a single character.
 */
static int rx_char_escape(RxParser *p, uint32_t *out)
{
    char e = p->src[p->pos];
    switch (e) {
        case 'n': *out = '\n'; p->pos++; return 1;
        case 't': *out = '\t'; p->pos++; return 1;
        case 'r': *out = '\r'; p->pos++; return 1;
        case 'f': *out = '\f'; p->pos++; return 1;
        case 'v': *out = '\v'; p->pos++; return 1;
        case '0': *out = 0; p->pos++; return 1;
        case 'x':
        {
            int high = p->pos + 2 < p->length ? rx_hex(p->src[p->pos + 1]) : -1;
            int low = high >= 0 ? rx_hex(p->src[p->pos + 2]) : -1;
            if (low < 0)
                rx_fail(p, "\\x must be followed by two hex digits");
            *out = (uint32_t) (high * 16 + low);
            p->pos += 3;
            return 1;
        }
        case 'u':
        {
            if (p->pos + 1 >= p->length || p->src[p->pos + 1] != '{')
                rx_fail(p, "expected \\u{...}");
            int i = p->pos + 2;
            uint32_t cp = 0;
            int digits = 0;
            while (i < p->length && rx_hex(p->src[i]) >= 0 && digits < 7)
            {
                cp = cp * 16 + (uint32_t) rx_hex(p->src[i++]);
                digits++;
            }
            if (i >= p->length || p->src[i] != '}' || digits == 0 || digits > 6 || cp > 0x10FFFF)
                rx_fail(p, "invalid \\u{...} escape");
            *out = cp;
            p->pos = i + 1;
            return 1;
        }
        default:
            // Any ASCII punctuation may be escaped to match itself: \. \* \\ \/ ...
            if ((unsigned char) e < 0x80 && ispunct((unsigned char) e))
            {
                *out = (uint32_t) e;
                p->pos++;
                return 1;
            }
            return 0;
    }
}

static RxNode *rx_parse_alt(RxParser *p);

/**
 * @brief Parses a bracket expression [...] (the '[' already consumed).
 */
static RxNode *rx_parse_class(RxParser *p)
{
    int negated = 0;
    if (p->pos < p->length && p->src[p->pos] == '^')
    {
        negated = 1;
        p->pos++;
    }
    RxNode *n = rx_node(p, RX_NODE_CLASS);
    n->index = rx_new_class(p, negated);
    int first = 1;
    for (;;)
    {
        if (p->pos >= p->length)
            rx_fail(p, "missing ] to close a character class");
        char c = p->src[p->pos];
        if (c == ']' && !first)
        {
            p->pos++;
            break;
        }
        first = 0;

        uint32_t lo;
        if (c == '\\')
        {
            p->pos++;
            if (p->pos >= p->length)
                rx_fail(p, "pattern ends with a backslash");
            char e = p->src[p->pos];
            if (strchr("dDwWsS", e) != NULL && e != '\0')
            {
                rx_class_add_shorthand(p, n->index, e);
                p->pos++;
                continue;
            }
            if (!rx_char_escape(p, &lo))
                rx_fail(p, "unknown escape in character class");
        }
        else
            p->pos += utf8_decode(p->src, p->length, p->pos, &lo);

        uint32_t hi = lo;
        // A range a-z (a '-' right before ']' is a literal '-').
        if (p->pos + 1 < p->length && p->src[p->pos] == '-' && p->src[p->pos + 1] != ']')
        {
            p->pos++;
            if (p->src[p->pos] == '\\')
            {
                p->pos++;
                if (p->pos >= p->length || !rx_char_escape(p, &hi))
                    rx_fail(p, "invalid character range end");
            }
            else
                p->pos += utf8_decode(p->src, p->length, p->pos, &hi);
            if (hi < lo)
                rx_fail(p, "character range is out of order");
        }
        rx_class_add(p, n->index, lo, hi);
    }

    if (p->icase)
    {
        // Case-insensitive (ASCII): add the other case of every letter range.
        RxClass *cls = &p->classes[n->index];
        int original = cls->count;
        for (int i = 0; i < original; i++)
        {
            uint32_t lo = cls->ranges[i].lo, hi = cls->ranges[i].hi;
            uint32_t a = lo > 'a' ? lo : 'a', b = hi < 'z' ? hi : 'z';
            if (a <= b)
                rx_class_add(p, n->index, a - 32, b - 32);
            a = lo > 'A' ? lo : 'A';
            b = hi < 'Z' ? hi : 'Z';
            if (a <= b)
                rx_class_add(p, n->index, a + 32, b + 32);
            cls = &p->classes[n->index]; // rx_class_add may move the ranges
        }
    }
    return n;
}

/**
 * @brief If a {n}, {n,} or {n,m} quantifier starts at p->pos, parses it.
 *
 * @return 1 if one was parsed (bounds in *min / *max, max -1 = unbounded), else 0
 *         (then '{' is an ordinary character).
 */
static int rx_parse_braces(RxParser *p, int *min, int *max)
{
    int i = p->pos + 1;
    long lo = 0, hi;
    if (i >= p->length || !isdigit((unsigned char) p->src[i]))
        return 0;
    while (i < p->length && isdigit((unsigned char) p->src[i]))
    {
        lo = lo * 10 + (p->src[i++] - '0');
        if (lo > RX_MAX_REPEAT)
            rx_fail(p, "repetition count is too large (max 1000)");
    }
    hi = lo;
    if (i < p->length && p->src[i] == ',')
    {
        i++;
        if (i < p->length && isdigit((unsigned char) p->src[i]))
        {
            hi = 0;
            while (i < p->length && isdigit((unsigned char) p->src[i]))
            {
                hi = hi * 10 + (p->src[i++] - '0');
                if (hi > RX_MAX_REPEAT)
                    rx_fail(p, "repetition count is too large (max 1000)");
            }
        }
        else
            hi = -1;
    }
    if (i >= p->length || p->src[i] != '}')
        return 0;
    if (hi != -1 && hi < lo)
        rx_fail(p, "repetition {n,m} has m < n");
    *min = (int) lo;
    *max = (int) hi;
    p->pos = i + 1;
    return 1;
}

static int rx_at_quantifier(RxParser *p)
{
    if (p->pos >= p->length)
        return 0;
    char c = p->src[p->pos];
    if (c == '*' || c == '+' || c == '?')
        return 1;
    if (c == '{')
    {
        int saved = p->pos, min, max;
        int is_quantifier = rx_parse_braces(p, &min, &max);
        p->pos = saved;
        return is_quantifier;
    }
    return 0;
}

static RxNode *rx_parse_group(RxParser *p)
{
    if (++p->nesting_depth > RX_MAX_NESTING)
        rx_fail(p, "groups are nested too deeply (max 250 levels)");
    int capture = -1;
    if (p->pos < p->length && p->src[p->pos] == '?')
    {
        p->pos++;
        if (p->pos < p->length && p->src[p->pos] == ':')
            p->pos++;
        else if (p->pos < p->length && (p->src[p->pos] == '<' || p->src[p->pos] == 'P'))
        {
            if (p->src[p->pos] == 'P')
                p->pos++;
            if (p->pos >= p->length || p->src[p->pos] != '<' ||
                (p->pos + 1 < p->length && (p->src[p->pos + 1] == '=' || p->src[p->pos + 1] == '!')))
                rx_fail(p, "lookbehind (?<= / (?<! is not supported (it cannot be matched in linear time)");
            p->pos++;
            int start = p->pos;
            while (p->pos < p->length && (isalnum((unsigned char) p->src[p->pos]) || p->src[p->pos] == '_'))
                p->pos++;
            if (p->pos == start || p->pos >= p->length || p->src[p->pos] != '>' ||
                isdigit((unsigned char) p->src[start]))
                rx_fail(p, "invalid group name (use letters, digits and _)");
            capture = ++p->group_count;
            if (capture > RX_MAX_GROUPS)
                rx_fail(p, "too many groups");
            if (capture >= p->names_capacity)
            {
                int old = p->names_capacity;
                p->names_capacity = (capture + 1) * 2;
                p->names = realloc(p->names, sizeof(char *) * (size_t) p->names_capacity);
                memset(p->names + old, 0, sizeof(char *) * (size_t) (p->names_capacity - old));
            }
            char *name = malloc((size_t) (p->pos - start) + 1);
            memcpy(name, p->src + start, (size_t) (p->pos - start));
            name[p->pos - start] = '\0';
            for (int g = 1; g < capture; g++)
            {
                if (p->names[g] && strcmp(p->names[g], name) == 0)
                {
                    free(name);
                    rx_fail(p, "duplicate group name");
                }
            }
            p->names[capture] = name;
            p->pos++; // '>'
        }
        else if (p->pos < p->length && (p->src[p->pos] == '=' || p->src[p->pos] == '!'))
            rx_fail(p, "lookahead (?= / (?! is not supported (it cannot be matched in linear time)");
        else
            rx_fail(p, "unsupported group syntax (flags such as (?i) are only allowed at the start)");
    }
    else
    {
        capture = ++p->group_count;
        if (capture > RX_MAX_GROUPS)
            rx_fail(p, "too many groups");
    }

    RxNode *inner = rx_parse_alt(p);
    if (p->pos >= p->length || p->src[p->pos] != ')')
        rx_fail(p, "missing ) to close a group");
    p->pos++;
    p->nesting_depth--;
    RxNode *n = rx_node(p, RX_NODE_GROUP);
    n->index = capture;
    n->a = inner;
    return n;
}

static RxNode *rx_parse_atom(RxParser *p)
{
    char c = p->src[p->pos];
    RxNode *n;
    switch (c) {
        case '(':
            p->pos++;
            return rx_parse_group(p);
        case '[':
            p->pos++;
            return rx_parse_class(p);
        case '.':
            p->pos++;
            return rx_node(p, RX_NODE_ANY);
        case '^':
        case '$':
            p->pos++;
            n = rx_node(p, RX_NODE_ASSERT);
            n->index = c == '^' ? RX_OP_BOL : RX_OP_EOL;
            return n;
        case '*':
        case '+':
        case '?':
            rx_fail(p, "nothing to repeat before a quantifier");
            return NULL;
        case '\\':
        {
            p->pos++;
            if (p->pos >= p->length)
                rx_fail(p, "pattern ends with a backslash");
            char e = p->src[p->pos];
            if (e != '\0' && strchr("dDwWsS", e) != NULL)
            {
                p->pos++;
                return rx_shorthand_node(p, e);
            }
            if (e == 'b' || e == 'B')
            {
                p->pos++;
                n = rx_node(p, RX_NODE_ASSERT);
                n->index = e == 'b' ? RX_OP_WORD_BOUNDARY : RX_OP_NOT_WORD_BOUNDARY;
                return n;
            }
            if (isdigit((unsigned char) e) && e != '0')
                rx_fail(p, "backreferences are not supported (they cannot be matched in linear time)");
            n = rx_node(p, RX_NODE_CHAR);
            if (!rx_char_escape(p, &n->c))
                rx_fail(p, "unknown escape (escape punctuation with \\, or use \\d \\w \\s \\b \\n \\t ...)");
            return n;
        }
        default:
            if (c == '{' && rx_at_quantifier(p))
                rx_fail(p, "nothing to repeat before a quantifier");
            n = rx_node(p, RX_NODE_CHAR);
            p->pos += utf8_decode(p->src, p->length, p->pos, &n->c);
            return n;
    }
}

static RxNode *rx_parse_repeat(RxParser *p)
{
    RxNode *atom = rx_parse_atom(p);
    if (!rx_at_quantifier(p))
        return atom;

    int min, max;
    char c = p->src[p->pos];
    if (c == '*')
    {
        min = 0;
        max = -1;
        p->pos++;
    }
    else if (c == '+')
    {
        min = 1;
        max = -1;
        p->pos++;
    }
    else if (c == '?')
    {
        min = 0;
        max = 1;
        p->pos++;
    }
    else
        rx_parse_braces(p, &min, &max);

    int greedy = 1;
    if (p->pos < p->length && p->src[p->pos] == '?')
    {
        greedy = 0;
        p->pos++;
    }
    if (rx_at_quantifier(p))
        rx_fail(p, "multiple quantifiers in a row (use a group, e.g. (a+)*)");

    RxNode *n = rx_node(p, RX_NODE_REPEAT);
    n->a = atom;
    n->min = min;
    n->max = max;
    n->greedy = greedy;
    return n;
}

static RxNode *rx_parse_concat(RxParser *p)
{
    RxNode *result = rx_node(p, RX_NODE_EMPTY);
    while (p->pos < p->length && p->src[p->pos] != '|' && p->src[p->pos] != ')')
    {
        RxNode *item = rx_parse_repeat(p);
        result = result->kind == RX_NODE_EMPTY ? item : rx_binary(p, RX_NODE_CONCAT, result, item);
    }
    return result;
}

static RxNode *rx_parse_alt(RxParser *p)
{
    RxNode *left = rx_parse_concat(p);
    while (p->pos < p->length && p->src[p->pos] == '|')
    {
        p->pos++;
        left = rx_binary(p, RX_NODE_ALT, left, rx_parse_concat(p));
    }
    return left;
}

// --- Compiler ---

typedef struct
{
    RxInst *code;
    int length;
    int capacity;
    RxParser *parser; // For errors (program too large)
} RxCompiler;

static int rx_emit(RxCompiler *c, RxOp op, int x, int y, uint32_t ch)
{
    if (c->length >= RX_MAX_PROGRAM)
        rx_fail(c->parser, "pattern is too large (simplify large repetition counts)");
    if (c->length == c->capacity)
    {
        c->capacity = c->capacity ? c->capacity * 2 : 64;
        c->code = realloc(c->code, sizeof(RxInst) * (size_t) c->capacity);
    }
    RxInst *inst = &c->code[c->length];
    inst->op = op;
    inst->x = x;
    inst->y = y;
    inst->c = ch;
    return c->length++;
}

static void rx_compile_node(RxCompiler *c, RxNode *n)
{
    RxParser *p = c->parser;
    switch (n->kind)
    {
        case RX_NODE_EMPTY:
            break;
        case RX_NODE_CHAR:
            rx_emit(c, RX_OP_CHAR, p->icase, 0, p->icase ? fold_case(n->c) : n->c);
            break;
        case RX_NODE_ANY:
            rx_emit(c, p->dotall ? RX_OP_ANY_NL : RX_OP_ANY, 0, 0, 0);
            break;
        case RX_NODE_CLASS:
            rx_emit(c, RX_OP_CLASS, n->index, 0, 0);
            break;
        case RX_NODE_ASSERT:
            rx_emit(c, (RxOp) n->index, 0, 0, 0);
            break;
        case RX_NODE_CONCAT:
        {
            // The parser builds sequences as a left-leaning chain (((a b) c) d),
            // as deep as the pattern is long. Walk the chain with a loop and
            // compile its pieces in order, so long patterns need no deep recursion.
            int piece_count = 0;
            for (RxNode *chain = n; chain->kind == RX_NODE_CONCAT; chain = chain->a)
                piece_count++;
            RxNode **pieces = malloc(sizeof(RxNode *) * (size_t) (piece_count + 1));
            RxNode *chain = n;
            for (int i = piece_count; i > 0; i--)
            {
                pieces[i] = chain->b; // Right-hand pieces, from last to first
                chain = chain->a;
            }
            pieces[0] = chain; // The leftmost piece
            for (int i = 0; i <= piece_count; i++)
                rx_compile_node(c, pieces[i]);
            free(pieces);
            break;
        }
        case RX_NODE_ALT:
        {
            int split = rx_emit(c, RX_OP_SPLIT, 0, 0, 0);
            c->code[split].x = c->length;
            rx_compile_node(c, n->a);
            int jump = rx_emit(c, RX_OP_JMP, 0, 0, 0);
            c->code[split].y = c->length;
            rx_compile_node(c, n->b);
            c->code[jump].x = c->length;
            break;
        }
        case RX_NODE_GROUP:
            if (n->index >= 0)
                rx_emit(c, RX_OP_SAVE, 2 * n->index, 0, 0);
            rx_compile_node(c, n->a);
            if (n->index >= 0)
                rx_emit(c, RX_OP_SAVE, 2 * n->index + 1, 0, 0);
            break;
        case RX_NODE_REPEAT:
        {
            for (int i = 0; i < n->min; i++)
                rx_compile_node(c, n->a);
            if (n->max == -1)
            {
                // loop: SPLIT body, exit / body / JMP loop
                int split = rx_emit(c, RX_OP_SPLIT, 0, 0, 0);
                int body = c->length;
                rx_compile_node(c, n->a);
                rx_emit(c, RX_OP_JMP, split, 0, 0);
                int exit = c->length;
                c->code[split].x = n->greedy ? body : exit;
                c->code[split].y = n->greedy ? exit : body;
            }
            else if (n->max > n->min)
            {
                // Optional copies: each SPLIT either runs one more copy or skips to the end.
                int optional = n->max - n->min;
                int *splits = malloc(sizeof(int) * (size_t) optional);
                for (int i = 0; i < optional; i++)
                {
                    splits[i] = rx_emit(c, RX_OP_SPLIT, 0, 0, 0);
                    c->code[splits[i]].x = c->length; // Body start (patched for laziness below)
                    rx_compile_node(c, n->a);
                }
                int end = c->length;
                for (int i = 0; i < optional; i++)
                {
                    int body = c->code[splits[i]].x;
                    c->code[splits[i]].x = n->greedy ? body : end;
                    c->code[splits[i]].y = n->greedy ? end : body;
                }
                free(splits);
            }
            break;
        }
    }
}

// --- Regex lifetime ---

static void regex_free(Regex *re)
{
    if (re == NULL)
        return;
    free(re->pattern);
    free(re->program);
    for (int i = 0; i < re->class_count; i++)
        free(re->classes[i].ranges);
    free(re->classes);
    if (re->group_names)
    {
        for (int i = 0; i <= re->group_count; i++)
            free(re->group_names[i]);
        free(re->group_names);
    }
    free(re->marks);
    free(re->lists[0].pcs);
    free(re->lists[0].caps);
    free(re->lists[1].pcs);
    free(re->lists[1].caps);
    free(re->stack);
    free(re->scratch_caps);
    free(re);
}

/**
 * @brief Compiles @p pattern.
 *
 * @return The Regex, or NULL with @p error / @p error_pos set.
 */
static Regex *regex_compile(const char *pattern, int length, char *error, size_t error_size, int *error_pos)
{
    RxParser parser;
    memset(&parser, 0, sizeof(parser));
    parser.src = pattern;
    parser.length = length;
    RxCompiler compiler = {NULL, 0, 0, &parser};

    if (setjmp(parser.fail) != 0)
    {
        snprintf(error, error_size, "%s", parser.error);
        *error_pos = parser.error_pos;
        for (int i = 0; i < parser.node_count; i++)
            free(parser.nodes[i]);
        free(parser.nodes);
        for (int i = 0; i < parser.class_count; i++)
            free(parser.classes[i].ranges);
        free(parser.classes);
        for (int i = 0; i < parser.names_capacity; i++)
            free(parser.names[i]);
        free(parser.names);
        free(compiler.code);
        return NULL;
    }

    // Leading flags: (?i) (?m) (?s), or combined like (?im).
    while (parser.pos + 2 < length && pattern[parser.pos] == '(' && pattern[parser.pos + 1] == '?' &&
           strchr("ims", pattern[parser.pos + 2]) != NULL && pattern[parser.pos + 2] != '\0')
    {
        int i = parser.pos + 2;
        while (i < length && strchr("ims", pattern[i]) != NULL && pattern[i] != '\0')
        {
            if (pattern[i] == 'i')
                parser.icase = 1;
            else if (pattern[i] == 'm')
                parser.multiline = 1;
            else
                parser.dotall = 1;
            i++;
        }
        if (i >= length || pattern[i] != ')')
            break; // Not a flag group; the parser reports it.
        parser.pos = i + 1;
    }

    RxNode *root = rx_parse_alt(&parser);
    if (parser.pos < length)
        rx_fail(&parser, "unmatched )");

    // Group 0 is the whole match: SAVE 0, pattern, SAVE 1, MATCH.
    rx_emit(&compiler, RX_OP_SAVE, 0, 0, 0);
    rx_compile_node(&compiler, root);
    rx_emit(&compiler, RX_OP_SAVE, 1, 0, 0);
    rx_emit(&compiler, RX_OP_MATCH, 0, 0, 0);

    Regex *re = calloc(1, sizeof(Regex));
    re->pattern = malloc((size_t) length + 1);
    memcpy(re->pattern, pattern, (size_t) length);
    re->pattern[length] = '\0';
    re->pattern_length = length;
    re->program = compiler.code;
    re->program_length = compiler.length;
    re->classes = parser.classes;
    re->class_count = parser.class_count;
    re->group_count = parser.group_count;
    re->multiline = parser.multiline;
    re->group_names = calloc((size_t) re->group_count + 1, sizeof(char *));
    for (int g = 1; g <= re->group_count && g < parser.names_capacity; g++)
        re->group_names[g] = parser.names[g]; // Ownership moves to the Regex.
    free(parser.names);
    for (int i = 0; i < parser.node_count; i++)
        free(parser.nodes[i]);
    free(parser.nodes);

    re->slot_count = 2 * (re->group_count + 1);
    int n = re->program_length;
    re->marks = calloc((size_t) n, sizeof(unsigned));
    for (int i = 0; i < 2; i++)
    {
        re->lists[i].pcs = malloc(sizeof(int) * (size_t) n);
        re->lists[i].caps = malloc(sizeof(int) * (size_t) n * (size_t) re->slot_count);
    }
    re->stack = malloc(sizeof(int) * 3 * (size_t) (2 * n + 2));
    re->scratch_caps = malloc(sizeof(int) * (size_t) re->slot_count);
    return re;
}

// --- The Pike VM ---

static int rx_class_matches(const RxClass *cls, uint32_t c)
{
    int in = 0;
    for (int i = 0; i < cls->count && !in; i++)
        in = c >= cls->ranges[i].lo && c <= cls->ranges[i].hi;
    return cls->negated ? !in : in;
}

static int rx_assert(const Regex *re, RxOp op, const char *text, int length, int pos)
{
    switch (op)
    {
        case RX_OP_BOL:
            return pos == 0 || (re->multiline && text[pos - 1] == '\n');
        case RX_OP_EOL:
            return pos == length || (re->multiline && text[pos] == '\n');
        default:
        {
            int before = pos > 0 ? is_word_byte((unsigned char) text[pos - 1]) : 0;
            int after = pos < length ? is_word_byte((unsigned char) text[pos]) : 0;
            int boundary = before != after;
            return op == RX_OP_WORD_BOUNDARY ? boundary : !boundary;
        }
    }
}

/**
 * @brief Adds the thread at @p pc (with captures @p caps) to @p list, following
 *        JMP / SPLIT / SAVE / assertions immediately (they consume no input).
 *
 * Uses an explicit stack instead of recursion so long patterns cannot overflow
 * the C stack. Entries are (pc, slot, old): slot -1 means "visit pc"; otherwise
 * "restore caps[slot] = old" after the higher-priority path has been explored.
 * Each instruction is visited at most once per generation, which is what keeps
 * matching linear.
 */
static void rx_add_thread(Regex *re, RxThreadList *list, int pc, int *caps, const char *text, int length, int pos)
{
    int *stack = re->stack;
    int top = 0;
    stack[top++] = pc;
    stack[top++] = -1;
    stack[top++] = 0;
    while (top > 0)
    {
        int old = stack[--top];
        int slot = stack[--top];
        int at = stack[--top];
        if (slot >= 0)
        {
            caps[slot] = old;
            continue;
        }
        if (re->marks[at] == re->generation)
            continue;
        re->marks[at] = re->generation;
        RxInst *inst = &re->program[at];
        switch (inst->op)
        {
            case RX_OP_JMP:
                stack[top++] = inst->x;
                stack[top++] = -1;
                stack[top++] = 0;
                break;
            case RX_OP_SPLIT:
                // Push the lower-priority branch first so the higher one runs first.
                stack[top++] = inst->y;
                stack[top++] = -1;
                stack[top++] = 0;
                stack[top++] = inst->x;
                stack[top++] = -1;
                stack[top++] = 0;
                break;
            case RX_OP_SAVE:
                stack[top++] = 0;
                stack[top++] = inst->x;
                stack[top++] = caps[inst->x];
                caps[inst->x] = pos;
                stack[top++] = at + 1;
                stack[top++] = -1;
                stack[top++] = 0;
                break;
            case RX_OP_BOL:
            case RX_OP_EOL:
            case RX_OP_WORD_BOUNDARY:
            case RX_OP_NOT_WORD_BOUNDARY:
                if (rx_assert(re, inst->op, text, length, pos))
                {
                    stack[top++] = at + 1;
                    stack[top++] = -1;
                    stack[top++] = 0;
                }
                break;
            default: // Consumes input (or MATCH): becomes a thread.
            {
                int t = list->count++;
                list->pcs[t] = at;
                memcpy(list->caps + (size_t) t * (size_t) re->slot_count, caps, sizeof(int) * (size_t) re->slot_count);
                break;
            }
        }
    }
}

/**
 * @brief Starts a new generation of instruction marks.
 *
 * The counter advances once per input character, so a long-running program
 * can wrap it; on wrap-around the marks are cleared so no stale mark can match.
 */
static void rx_next_generation(Regex *re)
{
    if (++re->generation == 0)
    {
        memset(re->marks, 0, sizeof(unsigned) * (size_t) re->program_length);
        re->generation = 1;
    }
}

/**
 * @brief Finds the leftmost match starting at or after byte @p start.
 *
 * @param forbid_empty_at A position where an *empty* match is not accepted (-1
 *        for none). Iterating callers pass the end of a previous empty match, so
 *        the next match may start at the same place only if it is non-empty
 *        (the rule Python 3.7+, Go and Rust use; nothing is skipped or repeated).
 * @param out_caps Receives slot_count capture offsets (-1 for groups that did not take part).
 * @return 1 if a match was found.
 */
static int rx_search(Regex *re, const char *text, int length, int start, int forbid_empty_at, int *out_caps)
{
    RxThreadList *current = &re->lists[0];
    RxThreadList *next = &re->lists[1];
    current->count = 0;
    int matched = 0;
    rx_next_generation(re);

    for (int pos = start;;)
    {
        if (!matched)
        {
            // Start a new (lowest-priority) attempt at this position.
            for (int i = 0; i < re->slot_count; i++)
                re->scratch_caps[i] = -1;
            rx_add_thread(re, current, 0, re->scratch_caps, text, length, pos);
        }
        if (current->count == 0 && (matched || pos >= length))
            break;

        uint32_t c = 0;
        int width = 0;
        if (pos < length)
            width = utf8_decode(text, length, pos, &c);

        next->count = 0;
        rx_next_generation(re);
        for (int t = 0; t < current->count; t++)
        {
            int pc = current->pcs[t];
            int *caps = current->caps + (size_t) t * (size_t) re->slot_count;
            RxInst *inst = &re->program[pc];
            int advance = 0;
            switch (inst->op)
            {
                case RX_OP_MATCH:
                    if (caps[0] == forbid_empty_at && caps[1] == forbid_empty_at)
                        break; // Rejected empty match: lower-priority threads may still match.
                    memcpy(out_caps, caps, sizeof(int) * (size_t) re->slot_count);
                    matched = 1;
                    t = current->count; // Lower-priority threads can no longer win.
                    break;
                case RX_OP_CHAR:
                    advance = pos < length && (inst->x ? fold_case(c) == inst->c : c == inst->c);
                    break;
                case RX_OP_ANY:
                    advance = pos < length && c != '\n';
                    break;
                case RX_OP_ANY_NL:
                    advance = pos < length;
                    break;
                case RX_OP_CLASS:
                    advance = pos < length && rx_class_matches(&re->classes[inst->x], c);
                    break;
                default:
                    break;
            }
            if (advance)
                rx_add_thread(re, next, pc + 1, caps, text, length, pos + width);
        }
        if (pos >= length)
            break;
        RxThreadList *swap = current;
        current = next;
        next = swap;
        pos += width;
    }
    return matched;
}

// --- Pith types: Regex and Match ---

static NativeClass *regex_class;
static NativeClass *match_class;

typedef struct
{
    Value subject; // The string that was searched (kept alive by match_mark)
    Value regex; // The Regex (for group names; kept alive by match_mark)
    int *caps; // slot_count offsets
} MatchData;

static void regex_finalize(void *data)
{
    regex_free((Regex *) data);
}

static void match_finalize(void *data)
{
    MatchData *m = (MatchData *) data;
    free(m->caps);
    free(m);
}

static void match_mark(void *data)
{
    MatchData *m = (MatchData *) data;
    mark_value(m->subject);
    mark_value(m->regex);
}

/**
 * @brief Creates a Match for @p caps. @p subject and @p regex_value must be rooted.
 */
static Value make_match(Value regex_value, Value subject, const int *caps)
{
    Regex *re = (Regex *) regex_value.native->data;
    MatchData *m = malloc(sizeof(MatchData));
    m->subject = subject;
    m->regex = regex_value;
    m->caps = malloc(sizeof(int) * (size_t) re->slot_count);
    memcpy(m->caps, caps, sizeof(int) * (size_t) re->slot_count);
    return native_object_new(match_class, m);
}

static Regex *regex_receiver(int arg_count, Value *args, int expected, const char *name, const char *usage)
{
    Regex *re = (Regex *) native_object_data(args[0], regex_class, name);
    if (arg_count != expected + 1)
        report_error(get_exec_error_line(), "%s() %s.", name, usage);
    for (int i = 1; i <= expected; i++)
    {
        if (args[i].type != VAL_STRING)
            report_error(get_exec_error_line(), "%s(): argument %d must be a string, got %s.", name, i,
                         type_describe_value(args[i]));
    }
    return re;
}

static Value make_string_list(void)
{
    Value list;
    list.type = VAL_LIST;
    list.list = (List *) allocate_obj(sizeof(List), OBJ_LIST);
    list.list->element_spec = type_from_name("string");
    return list;
}

// regex.compile(pattern) -> Regex
static Value native_regex_compile(int arg_count, Value *args)
{
    if (arg_count != 1 || args[0].type != VAL_STRING)
        report_error(get_exec_error_line(), "regex.compile() takes exactly one string argument (the pattern).");
    char error[256];
    int error_pos = 0;
    Regex *re = regex_compile(AS_CSTRING(args[0]), STRING_LENGTH(args[0]), error, sizeof(error), &error_pos);
    if (re == NULL)
        report_error(get_exec_error_line(), "regex.compile(): %s (at offset %d in \"%s\").", error, error_pos,
                     AS_CSTRING(args[0]));
    return native_object_new(regex_class, re);
}

// regex.escape(text) -> string
static Value native_regex_escape(int arg_count, Value *args)
{
    if (arg_count != 1 || args[0].type != VAL_STRING)
        report_error(get_exec_error_line(), "regex.escape() takes exactly one string argument.");
    const char *text = AS_CSTRING(args[0]);
    int length = STRING_LENGTH(args[0]);
    char *out = malloc((size_t) length * 2 + 1);
    int n = 0;
    for (int i = 0; i < length; i++)
    {
        unsigned char c = (unsigned char) text[i];
        if (c < 0x80 && ispunct(c))
            out[n++] = '\\';
        out[n++] = (char) c;
    }
    Value v = string_value(out, n);
    free(out);
    return v;
}

// Regex.test(text) -> bool
static Value native_regex_test(int arg_count, Value *args)
{
    Regex *re = regex_receiver(arg_count, args, 1, "test", "takes exactly one argument (the text)");
    int *caps = malloc(sizeof(int) * (size_t) re->slot_count);
    Value v;
    v.type = VAL_BOOL;
    v.int_val = rx_search(re, AS_CSTRING(args[1]), STRING_LENGTH(args[1]), 0, -1, caps);
    free(caps);
    return v;
}

// Regex.find(text) -> Match or void
static Value native_regex_find(int arg_count, Value *args)
{
    Regex *re = regex_receiver(arg_count, args, 1, "find", "takes exactly one argument (the text)");
    int *caps = malloc(sizeof(int) * (size_t) re->slot_count);
    Value result = {VAL_VOID};
    if (rx_search(re, AS_CSTRING(args[1]), STRING_LENGTH(args[1]), 0, -1, caps))
        result = make_match(args[0], args[1], caps);
    free(caps);
    return result;
}

// Regex.find_all(text) -> list<Match>
static Value native_regex_find_all(int arg_count, Value *args)
{
    Regex *re = regex_receiver(arg_count, args, 1, "find_all", "takes exactly one argument (the text)");
    const char *text = AS_CSTRING(args[1]);
    int length = STRING_LENGTH(args[1]);
    Value list;
    list.type = VAL_LIST;
    list.list = (List *) allocate_obj(sizeof(List), OBJ_LIST);
    list.list->element_spec = type_from_name("Match");
    gc_push_value(list);
    int *caps = malloc(sizeof(int) * (size_t) re->slot_count);
    int start = 0, forbid = -1;
    while (start <= length && rx_search(re, text, length, start, forbid, caps))
    {
        list_add(list.list, make_match(args[0], args[1], caps));
        start = caps[1];
        forbid = caps[0] == caps[1] ? caps[1] : -1; // No second empty match at the same place
    }
    free(caps);
    gc_pop_root();
    return list;
}

typedef struct
{
    char *data;
    size_t length, capacity;
} RxBuffer;

static void rx_buffer_append(RxBuffer *b, const char *bytes, size_t n)
{
    if (n == 0)
        return; // Also avoids memcpy() with a NULL buffer (undefined even for 0 bytes).
    if (b->length + n > b->capacity)
    {
        while (b->length + n > b->capacity)
            b->capacity = b->capacity ? b->capacity * 2 : 256;
        b->data = realloc(b->data, b->capacity);
    }
    memcpy(b->data + b->length, bytes, n);
    b->length += n;
}

/**
 * @brief Resolves a group reference (a number or a name) to its group number, or -1.
 */
static int rx_group_number(const Regex *re, const char *ref, size_t ref_length)
{
    if (ref_length > 0 && isdigit((unsigned char) ref[0]))
    {
        int n = 0;
        for (size_t i = 0; i < ref_length; i++)
        {
            if (!isdigit((unsigned char) ref[i]))
                return -1;
            n = n * 10 + (ref[i] - '0');
            if (n > re->group_count)
                return -1;
        }
        return n;
    }
    for (int g = 1; g <= re->group_count; g++)
    {
        const char *name = re->group_names[g];
        if (name && strlen(name) == ref_length && memcmp(name, ref, ref_length) == 0)
            return g;
    }
    return -1;
}

/**
 * @brief Appends @p repl to @p out, expanding $0..$9, ${n} / ${name} and $$.
 */
static void rx_expand_replacement(RxBuffer *out, const Regex *re, const char *text, const int *caps,
                                  const char *repl, int repl_length)
{
    for (int i = 0; i < repl_length; i++)
    {
        if (repl[i] != '$')
        {
            rx_buffer_append(out, repl + i, 1);
            continue;
        }
        if (i + 1 < repl_length && repl[i + 1] == '$')
        {
            rx_buffer_append(out, "$", 1);
            i++;
            continue;
        }
        int group = -1;
        if (i + 1 < repl_length && isdigit((unsigned char) repl[i + 1]))
        {
            group = rx_group_number(re, repl + i + 1, 1); // $0..$9: one digit
            i += 1;
        }
        else if (i + 1 < repl_length && (isalpha((unsigned char) repl[i + 1]) || repl[i + 1] == '_'))
        {
            int end = i + 1; // $name: the longest run of name characters
            while (end < repl_length && (isalnum((unsigned char) repl[end]) || repl[end] == '_'))
                end++;
            group = rx_group_number(re, repl + i + 1, (size_t) (end - i - 1));
            i = end - 1;
        }
        else if (i + 1 < repl_length && repl[i + 1] == '{')
        {
            const char *close = memchr(repl + i + 2, '}', (size_t) (repl_length - i - 2));
            if (close != NULL)
            {
                group = rx_group_number(re, repl + i + 2, (size_t) (close - (repl + i + 2)));
                i = (int) (close - repl);
            }
        }
        if (group < 0)
        {
            free(out->data);
            report_error(get_exec_error_line(),
                         "replace(): invalid group reference in the replacement (use $1, ${12}, $name, ${name}, "
                         "or $$ for a literal $).");
        }
        if (caps[2 * group] >= 0)
            rx_buffer_append(out, text + caps[2 * group], (size_t) (caps[2 * group + 1] - caps[2 * group]));
    }
}

// Regex.replace(text, replacement) -> string
static Value native_regex_replace(int arg_count, Value *args)
{
    Regex *re = regex_receiver(arg_count, args, 2, "replace", "takes two arguments (text, replacement)");
    const char *text = AS_CSTRING(args[1]);
    int length = STRING_LENGTH(args[1]);
    RxBuffer out = {NULL, 0, 0};
    int *caps = malloc(sizeof(int) * (size_t) re->slot_count);
    int copied = 0; // Bytes of `text` already copied to the output
    int start = 0, forbid = -1;
    while (start <= length && rx_search(re, text, length, start, forbid, caps))
    {
        rx_buffer_append(&out, text + copied, (size_t) (caps[0] - copied));
        rx_expand_replacement(&out, re, text, caps, AS_CSTRING(args[2]), STRING_LENGTH(args[2]));
        copied = caps[1];
        start = caps[1];
        forbid = caps[0] == caps[1] ? caps[1] : -1;
    }
    rx_buffer_append(&out, text + copied, (size_t) (length - copied));
    free(caps);
    Value v = string_value(out.data ? out.data : "", (int) out.length);
    free(out.data);
    return v;
}

// Regex.split(text) -> list<string>
static Value native_regex_split(int arg_count, Value *args)
{
    Regex *re = regex_receiver(arg_count, args, 1, "split", "takes exactly one argument (the text)");
    const char *text = AS_CSTRING(args[1]);
    int length = STRING_LENGTH(args[1]);
    Value list = make_string_list();
    gc_push_value(list);
    int *caps = malloc(sizeof(int) * (size_t) re->slot_count);
    int piece_start = 0;
    int start = 0, forbid = -1;
    while (start <= length && rx_search(re, text, length, start, forbid, caps))
    {
        start = caps[1];
        forbid = caps[0] == caps[1] ? caps[1] : -1;
        if (caps[1] == caps[0] && (caps[0] == 0 || caps[0] == length))
            continue; // An empty match at either end would only add an empty piece.
        list_add(list.list, string_value(text + piece_start, caps[0] - piece_start));
        piece_start = caps[1];
    }
    list_add(list.list, string_value(text + piece_start, length - piece_start));
    free(caps);
    gc_pop_root();
    return list;
}

// Regex.pattern() -> string
static Value native_regex_pattern(int arg_count, Value *args)
{
    Regex *re = regex_receiver(arg_count, args, 0, "pattern", "takes no arguments");
    return string_value(re->pattern, re->pattern_length);
}

// Regex.group_count() -> int
static Value native_regex_group_count(int arg_count, Value *args)
{
    Regex *re = regex_receiver(arg_count, args, 0, "group_count", "takes no arguments");
    Value v;
    v.type = VAL_INT;
    v.int_val = re->group_count;
    return v;
}

// --- Match methods ---

/**
 * @brief Reads the optional group argument (number or name) of a Match method.
 */
static int match_group_arg(MatchData *m, int arg_count, Value *args, const char *name)
{
    Regex *re = (Regex *) m->regex.native->data;
    if (arg_count == 1)
        return 0;
    if (arg_count != 2)
        report_error(get_exec_error_line(), "%s() takes an optional group number or name.", name);
    if (args[1].type == VAL_INT)
    {
        if (args[1].int_val < 0 || args[1].int_val > re->group_count)
            report_error(get_exec_error_line(), "%s(): group %" PITH_INT_FMT " does not exist (the pattern has %d).",
                         name, args[1].int_val, re->group_count);
        return (int) args[1].int_val;
    }
    if (args[1].type == VAL_STRING)
    {
        int group = rx_group_number(re, AS_CSTRING(args[1]), (size_t) STRING_LENGTH(args[1]));
        if (group < 0 || isdigit((unsigned char) AS_CSTRING(args[1])[0]))
            report_error(get_exec_error_line(), "%s(): the pattern has no group named '%s'.", name,
                         AS_CSTRING(args[1]));
        return group;
    }
    report_error(get_exec_error_line(), "%s(): the group must be a number or a name, got %s.", name,
                 type_describe_value(args[1]));
    return 0;
}

static Value match_group_text(MatchData *m, int group)
{
    if (m->caps[2 * group] < 0)
        return cstring_value(""); // The group did not take part in the match.
    return string_value(AS_CSTRING(m->subject) + m->caps[2 * group], m->caps[2 * group + 1] - m->caps[2 * group]);
}

// Match.text() -> string
static Value native_match_text(int arg_count, Value *args)
{
    MatchData *m = (MatchData *) native_object_data(args[0], match_class, "text");
    if (arg_count != 1)
        report_error(get_exec_error_line(), "text() takes no arguments.");
    return match_group_text(m, 0);
}

// Match.group([n | name]) -> string
static Value native_match_group(int arg_count, Value *args)
{
    MatchData *m = (MatchData *) native_object_data(args[0], match_class, "group");
    return match_group_text(m, match_group_arg(m, arg_count, args, "group"));
}

// Match.start([n | name]) -> int
static Value native_match_start(int arg_count, Value *args)
{
    MatchData *m = (MatchData *) native_object_data(args[0], match_class, "start");
    Value v;
    v.type = VAL_INT;
    v.int_val = m->caps[2 * match_group_arg(m, arg_count, args, "start")];
    return v;
}

// Match.end([n | name]) -> int
static Value native_match_end(int arg_count, Value *args)
{
    MatchData *m = (MatchData *) native_object_data(args[0], match_class, "end");
    Value v;
    v.type = VAL_INT;
    v.int_val = m->caps[2 * match_group_arg(m, arg_count, args, "end") + 1];
    return v;
}

// Match.groups() -> list<string> of groups 1..n
static Value native_match_groups(int arg_count, Value *args)
{
    MatchData *m = (MatchData *) native_object_data(args[0], match_class, "groups");
    if (arg_count != 1)
        report_error(get_exec_error_line(), "groups() takes no arguments.");
    Regex *re = (Regex *) m->regex.native->data;
    Value list = make_string_list();
    gc_push_value(list);
    for (int g = 1; g <= re->group_count; g++)
        list_add(list.list, match_group_text(m, g));
    gc_pop_root();
    return list;
}

// Match.named() -> map<string, string>
static Value native_match_named(int arg_count, Value *args)
{
    MatchData *m = (MatchData *) native_object_data(args[0], match_class, "named");
    if (arg_count != 1)
        report_error(get_exec_error_line(), "named() takes no arguments.");
    Regex *re = (Regex *) m->regex.native->data;
    Value map;
    map.type = VAL_HASHMAP;
    map.hashmap = hashmap_create(VAL_STRING, VAL_VOID);
    map.hashmap->value_spec = type_from_name("string");
    gc_push_value(map);
    for (int g = 1; g <= re->group_count; g++)
    {
        if (re->group_names[g])
            hashmap_set(map.hashmap, re->group_names[g], match_group_text(m, g), 0);
    }
    gc_pop_root();
    return map;
}

// --- Registration ---

void regex_register_module(HashMap *modules)
{
    regex_class = native_class_create("Regex", regex_finalize, NULL);
    native_class_add_method(regex_class, "test", native_regex_test);
    native_class_add_method(regex_class, "find", native_regex_find);
    native_class_add_method(regex_class, "find_all", native_regex_find_all);
    native_class_add_method(regex_class, "replace", native_regex_replace);
    native_class_add_method(regex_class, "split", native_regex_split);
    native_class_add_method(regex_class, "pattern", native_regex_pattern);
    native_class_add_method(regex_class, "group_count", native_regex_group_count);

    match_class = native_class_create("Match", match_finalize, match_mark);
    native_class_add_method(match_class, "text", native_match_text);
    native_class_add_method(match_class, "group", native_match_group);
    native_class_add_method(match_class, "start", native_match_start);
    native_class_add_method(match_class, "end", native_match_end);
    native_class_add_method(match_class, "groups", native_match_groups);
    native_class_add_method(match_class, "named", native_match_named);

    HashMap *funcs = hashmap_create(VAL_STRING, VAL_NATIVE_FN);
    register_native_method(funcs, "compile", native_regex_compile);
    register_native_method(funcs, "escape", native_regex_escape);

    Value module;
    module.type = VAL_HASHMAP;
    module.hashmap = funcs;
    hashmap_set(modules, "regex", module, 0);
}
