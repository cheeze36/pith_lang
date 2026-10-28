/**
 * @file json_module.c
 * @brief Implementation of the native `json` module (see json_module.h).
 *
 * ## Memory / GC
 *
 * Encoding writes into a malloc'd byte buffer and allocates the resulting Pith
 * string once at the end; the value being encoded is an argument, so the
 * interpreter keeps it rooted throughout.
 *
 * Decoding builds lists and maps recursively. Each container is pushed as a GC
 * root while its children are parsed (parsing a child allocates), and every
 * child is linked into its container immediately after it is created, before
 * anything else is allocated.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>

#include "json_module.h"
#include "interpreter.h"
#include "common.h"
#include "gc.h"
#include "pith_string.h"

// Nesting limit for both directions: deep enough for real data, shallow enough
// that a hostile document (or a list containing itself) cannot overflow the C stack.
#define JSON_MAX_DEPTH 200

// --- Encoding ---

/** Growable text the encoder writes into (converted to a Pith string at the end). */
typedef struct
{
    char *data;
    size_t length;
    size_t capacity;
} JsonText;

static void buffer_append(JsonText *b, const char *bytes, size_t n)
{
    if (n == 0)
        return; // memcpy() with a NULL buffer is undefined even for 0 bytes.
    if (b->length + n > b->capacity)
    {
        while (b->length + n > b->capacity)
            b->capacity = b->capacity == 0 ? 256 : b->capacity * 2;
        b->data = realloc(b->data, b->capacity);
    }
    memcpy(b->data + b->length, bytes, n);
    b->length += n;
}

static void buffer_append_cstr(JsonText *b, const char *text)
{
    buffer_append(b, text, strlen(text));
}

static void encode_string(JsonText *b, const char *chars, int length)
{
    buffer_append(b, "\"", 1);
    for (int i = 0; i < length; i++)
    {
        unsigned char c = (unsigned char) chars[i];
        switch (c) {
            case '"': buffer_append_cstr(b, "\\\""); break;
            case '\\': buffer_append_cstr(b, "\\\\"); break;
            case '\n': buffer_append_cstr(b, "\\n"); break;
            case '\r': buffer_append_cstr(b, "\\r"); break;
            case '\t': buffer_append_cstr(b, "\\t"); break;
            case '\b': buffer_append_cstr(b, "\\b"); break;
            case '\f': buffer_append_cstr(b, "\\f"); break;
            default:
                if (c < 0x20)
                {
                    char escape[8];
                    snprintf(escape, sizeof(escape), "\\u%04x", c);
                    buffer_append_cstr(b, escape);
                }
                else
                    buffer_append(b, (const char *) &c, 1); // UTF-8 passes through unchanged.
        }
    }
    buffer_append(b, "\"", 1);
}

static void encode_value(JsonText *b, Value v, int depth)
{
    if (depth > JSON_MAX_DEPTH)
        report_error(get_exec_error_line(), "json.encode(): nesting is deeper than %d (is a list or map inside itself?).",
                     JSON_MAX_DEPTH);
    char number[64];
    switch (v.type)
    {
        case VAL_INT:
            snprintf(number, sizeof(number), "%" PITH_INT_FMT, v.int_val);
            buffer_append_cstr(b, number);
            break;
        case VAL_FLOAT:
            if (isnan(v.float_val) || isinf(v.float_val))
                report_error(get_exec_error_line(), "json.encode(): NaN and infinity cannot be represented in JSON.");
            // Fewest digits (up to 17) that read back as exactly the same double.
            for (int precision = 1; precision <= 17; precision++)
            {
                snprintf(number, sizeof(number), "%.*g", precision, v.float_val);
                if (strtod(number, NULL) == v.float_val)
                    break;
            }
            if (strpbrk(number, ".eE") == NULL)
                strcat(number, ".0"); // Keep it a float when decoded again.
            buffer_append_cstr(b, number);
            break;
        case VAL_BOOL:
            buffer_append_cstr(b, v.int_val ? "true" : "false");
            break;
        case VAL_VOID:
            buffer_append_cstr(b, "null");
            break;
        case VAL_STRING:
            encode_string(b, v.string->chars, v.string->length);
            break;
        case VAL_LIST:
            buffer_append(b, "[", 1);
            for (int i = 0; i < v.list->count; i++)
            {
                if (i > 0)
                    buffer_append(b, ",", 1);
                encode_value(b, v.list->items[i], depth + 1);
            }
            buffer_append(b, "]", 1);
            break;
        case VAL_HASHMAP:
        case VAL_INSTANCE:
        {
            // Instances encode as an object of their fields.
            HashMap *map = v.type == VAL_HASHMAP ? v.hashmap : v.instance->fields;
            MapEntry **entries = hashmap_entries_in_order(map);
            buffer_append(b, "{", 1);
            for (int i = 0; i < map->count; i++)
            {
                if (i > 0)
                    buffer_append(b, ",", 1);
                encode_string(b, entries[i]->key, (int) strlen(entries[i]->key));
                buffer_append(b, ":", 1);
                encode_value(b, entries[i]->value, depth + 1);
            }
            buffer_append(b, "}", 1);
            free(entries);
            break;
        }
        default:
            report_error(get_exec_error_line(), "json.encode(): cannot encode a value of type %s.",
                         get_value_type_name(v.type));
    }
}

// json.encode(value) -> string
static Value native_json_encode(int arg_count, Value *args)
{
    if (arg_count != 1)
        report_error(get_exec_error_line(), "json.encode() takes exactly one argument.");
    JsonText b = {NULL, 0, 0};
    encode_value(&b, args[0], 0);
    Value result = string_value(b.data, (int) b.length);
    free(b.data);
    return result;
}

// --- Decoding ---

typedef struct
{
    const char *text;
    int length;
    int pos;
} Parser;

static void parse_fail(Parser *p, const char *what)
{
    report_error(get_exec_error_line(), "json.decode(): %s at byte %d.", what, p->pos);
}

static void skip_whitespace(Parser *p)
{
    while (p->pos < p->length &&
           (p->text[p->pos] == ' ' || p->text[p->pos] == '\t' || p->text[p->pos] == '\n' || p->text[p->pos] == '\r'))
        p->pos++;
}

static int match_literal(Parser *p, const char *word)
{
    int n = (int) strlen(word);
    if (p->pos + n <= p->length && memcmp(p->text + p->pos, word, (size_t) n) == 0)
    {
        p->pos += n;
        return 1;
    }
    return 0;
}

static int read_hex4(Parser *p)
{
    if (p->pos + 4 > p->length)
        parse_fail(p, "incomplete \\u escape");
    int value = 0;
    for (int i = 0; i < 4; i++)
    {
        char c = p->text[p->pos++];
        value *= 16;
        if (c >= '0' && c <= '9')
            value += c - '0';
        else if (c >= 'a' && c <= 'f')
            value += c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            value += c - 'A' + 10;
        else
            parse_fail(p, "invalid \\u escape");
    }
    return value;
}

static int put_utf8(unsigned long cp, char *out)
{
    if (cp < 0x80)
    {
        out[0] = (char) cp;
        return 1;
    }
    if (cp < 0x800)
    {
        out[0] = (char) (0xC0 | (cp >> 6));
        out[1] = (char) (0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000)
    {
        out[0] = (char) (0xE0 | (cp >> 12));
        out[1] = (char) (0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char) (0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char) (0xF0 | (cp >> 18));
    out[1] = (char) (0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char) (0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char) (0x80 | (cp & 0x3F));
    return 4;
}

/**
 * @brief Parses a JSON string (at the opening quote) into a malloc'd buffer.
 *
 * @param length_out Receives the decoded byte length.
 * @return The bytes (NUL-terminated); the caller frees them.
 */
static char *parse_string_bytes(Parser *p, int *length_out)
{
    p->pos++; // opening quote
    // Decoded text is never longer than the escaped source.
    char *out = malloc((size_t) (p->length - p->pos) + 1);
    int n = 0;
    for (;;)
    {
        if (p->pos >= p->length)
        {
            free(out);
            parse_fail(p, "unterminated string");
        }
        unsigned char c = (unsigned char) p->text[p->pos++];
        if (c == '"')
            break;
        if (c < 0x20)
        {
            free(out);
            parse_fail(p, "control character in string");
        }
        if (c != '\\')
        {
            out[n++] = (char) c;
            continue;
        }
        if (p->pos >= p->length)
        {
            free(out);
            parse_fail(p, "unterminated string");
        }
        char e = p->text[p->pos++];
        switch (e) {
            case '"': out[n++] = '"'; break;
            case '\\': out[n++] = '\\'; break;
            case '/': out[n++] = '/'; break;
            case 'b': out[n++] = '\b'; break;
            case 'f': out[n++] = '\f'; break;
            case 'n': out[n++] = '\n'; break;
            case 'r': out[n++] = '\r'; break;
            case 't': out[n++] = '\t'; break;
            case 'u':
            {
                unsigned long cp = (unsigned long) read_hex4(p);
                if (cp >= 0xD800 && cp <= 0xDBFF)
                {
                    // High surrogate: must be followed by \uDC00-\uDFFF.
                    if (!match_literal(p, "\\u"))
                    {
                        free(out);
                        parse_fail(p, "unpaired surrogate in \\u escape");
                    }
                    unsigned long low = (unsigned long) read_hex4(p);
                    if (low < 0xDC00 || low > 0xDFFF)
                    {
                        free(out);
                        parse_fail(p, "unpaired surrogate in \\u escape");
                    }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                }
                else if (cp >= 0xDC00 && cp <= 0xDFFF)
                {
                    free(out);
                    parse_fail(p, "unpaired surrogate in \\u escape");
                }
                n += put_utf8(cp, out + n);
                break;
            }
            default:
                free(out);
                parse_fail(p, "invalid escape in string");
        }
    }
    out[n] = '\0';
    *length_out = n;
    return out;
}

static Value parse_value(Parser *p, int depth);

static Value parse_number(Parser *p)
{
    int start = p->pos;
    int is_integer = 1;
    if (p->text[p->pos] == '-')
        p->pos++;
    if (p->pos >= p->length || p->text[p->pos] < '0' || p->text[p->pos] > '9')
        parse_fail(p, "invalid number");
    if (p->text[p->pos] == '0')
        p->pos++; // JSON forbids leading zeros
    else
        while (p->pos < p->length && p->text[p->pos] >= '0' && p->text[p->pos] <= '9')
            p->pos++;
    if (p->pos < p->length && p->text[p->pos] == '.')
    {
        is_integer = 0;
        p->pos++;
        if (p->pos >= p->length || p->text[p->pos] < '0' || p->text[p->pos] > '9')
            parse_fail(p, "invalid number");
        while (p->pos < p->length && p->text[p->pos] >= '0' && p->text[p->pos] <= '9')
            p->pos++;
    }
    if (p->pos < p->length && (p->text[p->pos] == 'e' || p->text[p->pos] == 'E'))
    {
        is_integer = 0;
        p->pos++;
        if (p->pos < p->length && (p->text[p->pos] == '+' || p->text[p->pos] == '-'))
            p->pos++;
        if (p->pos >= p->length || p->text[p->pos] < '0' || p->text[p->pos] > '9')
            parse_fail(p, "invalid number");
        while (p->pos < p->length && p->text[p->pos] >= '0' && p->text[p->pos] <= '9')
            p->pos++;
    }

    char number[64];
    int n = p->pos - start;
    if (n >= (int) sizeof(number))
        parse_fail(p, "number is too long");
    memcpy(number, p->text + start, (size_t) n);
    number[n] = '\0';

    Value v;
    if (is_integer)
    {
        errno = 0;
        long long value = strtoll(number, NULL, 10);
        if (errno != ERANGE)
        {
            v.type = VAL_INT;
            v.int_val = (int64_t) value;
            return v;
        }
    }
    v.type = VAL_FLOAT; // Has a fraction/exponent, or is too large for an int.
    v.float_val = strtod(number, NULL);
    return v;
}

static Value parse_array(Parser *p, int depth)
{
    p->pos++; // '['
    List *list = (List *) allocate_obj(sizeof(List), OBJ_LIST);
    Value result;
    result.type = VAL_LIST;
    result.list = list;
    gc_push_value(result);

    skip_whitespace(p);
    if (p->pos < p->length && p->text[p->pos] == ']')
        p->pos++;
    else
    {
        for (;;)
        {
            list_add(list, parse_value(p, depth + 1));
            skip_whitespace(p);
            if (p->pos < p->length && p->text[p->pos] == ',')
            {
                p->pos++;
                continue;
            }
            if (p->pos < p->length && p->text[p->pos] == ']')
            {
                p->pos++;
                break;
            }
            parse_fail(p, "expected ',' or ']' in array");
        }
    }
    gc_pop_root();
    return result;
}

static Value parse_object(Parser *p, int depth)
{
    p->pos++; // '{'
    HashMap *map = hashmap_create(VAL_STRING, VAL_VOID);
    Value result;
    result.type = VAL_HASHMAP;
    result.hashmap = map;
    gc_push_value(result);

    skip_whitespace(p);
    if (p->pos < p->length && p->text[p->pos] == '}')
        p->pos++;
    else
    {
        for (;;)
        {
            skip_whitespace(p);
            if (p->pos >= p->length || p->text[p->pos] != '"')
                parse_fail(p, "expected a string key in object");
            int key_length;
            char *key = parse_string_bytes(p, &key_length);
            skip_whitespace(p);
            if (p->pos >= p->length || p->text[p->pos] != ':')
            {
                free(key);
                parse_fail(p, "expected ':' after object key");
            }
            p->pos++;
            Value value = parse_value(p, depth + 1);
            hashmap_set(map, key, value, 0); // Map keys are text: a key with \u0000 is cut at the NUL.
            free(key);
            skip_whitespace(p);
            if (p->pos < p->length && p->text[p->pos] == ',')
            {
                p->pos++;
                continue;
            }
            if (p->pos < p->length && p->text[p->pos] == '}')
            {
                p->pos++;
                break;
            }
            parse_fail(p, "expected ',' or '}' in object");
        }
    }
    gc_pop_root();
    return result;
}

static Value parse_value(Parser *p, int depth)
{
    if (depth > JSON_MAX_DEPTH)
        parse_fail(p, "nesting is too deep");
    skip_whitespace(p);
    if (p->pos >= p->length)
        parse_fail(p, "unexpected end of input");

    char c = p->text[p->pos];
    if (c == '{')
        return parse_object(p, depth);
    if (c == '[')
        return parse_array(p, depth);
    if (c == '"')
    {
        int length;
        char *bytes = parse_string_bytes(p, &length);
        Value v = string_value(bytes, length);
        free(bytes);
        return v;
    }
    if (c == '-' || (c >= '0' && c <= '9'))
        return parse_number(p);

    Value v;
    if (match_literal(p, "true"))
    {
        v.type = VAL_BOOL;
        v.int_val = 1;
        return v;
    }
    if (match_literal(p, "false"))
    {
        v.type = VAL_BOOL;
        v.int_val = 0;
        return v;
    }
    if (match_literal(p, "null"))
        return (Value){VAL_VOID};
    parse_fail(p, "unexpected character");
    return (Value){VAL_VOID};
}

// json.decode(text) -> the decoded value
static Value native_json_decode(int arg_count, Value *args)
{
    if (arg_count != 1 || args[0].type != VAL_STRING)
        report_error(get_exec_error_line(), "json.decode() takes exactly one string argument.");
    // The argument string is rooted by the interpreter, so p.text stays valid.
    Parser p = {AS_CSTRING(args[0]), STRING_LENGTH(args[0]), 0};
    Value result = parse_value(&p, 0);
    skip_whitespace(&p);
    if (p.pos != p.length)
        parse_fail(&p, "unexpected text after the JSON value");
    return result;
}

// --- Registration ---

void json_register_module(HashMap *modules)
{
    HashMap *json_funcs = hashmap_create(VAL_STRING, VAL_NATIVE_FN);
    register_native_method(json_funcs, "encode", native_json_encode);
    register_native_method(json_funcs, "decode", native_json_decode);

    Value json_module_val;
    json_module_val.type = VAL_HASHMAP;
    json_module_val.hashmap = json_funcs;
    hashmap_set(modules, "json", json_module_val, 0);
}
