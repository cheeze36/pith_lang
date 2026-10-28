/**
 * @file types.h
 * @brief Runtime representation and enforcement of Pith's declared types.
 *
 * Every place a value is stored under a declared type (variable declarations
 * and assignments, function parameters and return values, class fields, list
 * elements, map values, foreach variables) goes through type_check_value() /
 * type_require().
 *
 * ## Rules
 *
 *  - `int`, `float`, `string`, `bool`, `buffer`: exact match, with one exception: an int
 *    is accepted where a float is expected and is widened to a float. Nothing
 *    else is ever converted implicitly; use str(), int() and float().
 *  - `list<T>` / `map<string,T>`: the value must be a list/map whose elements
 *    satisfy T. An untyped list or map (e.g. a literal) is checked element by
 *    element the first time it is stored under a typed name and then *adopts*
 *    that element type, which later appends and assignments enforce. A list
 *    that already has a different element type is rejected (no container
 *    conversion). Bare `list` / `map` accept any elements.
 *  - A class name: an instance of that class or a subclass, or `void` ("no
 *    object"). Class types are the only types that may hold void.
 *  - `void`: only used as a function return type; the function returns nothing.
 *
 * Declarations without an initializer get the type's zero value: 0, 0.0, "",
 * false, an empty buffer, an empty list/map, or void for class types.
 *
 * TypeSpecs are interned and never freed, so values (lists, maps) can point at
 * them regardless of the lifetime of the AST they were parsed from.
 */

#ifndef PITH_TYPES_H
#define PITH_TYPES_H

#include <stddef.h>
#include "value.h"

typedef enum
{
    TYPE_ANY, // Unconstrained (element type of bare `list` / `map`)
    TYPE_INT,
    TYPE_FLOAT,
    TYPE_STRING,
    TYPE_BOOL,
    TYPE_BUFFER, // The mutable byte buffer (see pith_buffer.h)
    TYPE_VOID, // Function return type only
    TYPE_LIST,
    TYPE_MAP,
    TYPE_CLASS
} TypeKind;

struct TypeSpec
{
    TypeKind kind;
    struct TypeSpec *element; // TYPE_LIST: element type; TYPE_MAP: value type (keys are strings).
    char *class_name; // TYPE_CLASS: the class name.
    char *name; // Canonical spelling for messages, e.g. "list<int>".
};

/**
 * @brief Returns the interned TypeSpec for a type name such as "int",
 *        "list<float>", "map<string,int>" or a class name.
 *
 * @param type_name The type as written (the parser's canonical spelling).
 * @return The TypeSpec, or NULL if @p type_name is NULL (an untyped slot).
 */
TypeSpec *type_from_name(const char *type_name);

/**
 * @brief Describes a value's type for error messages ("int", "list<int>", "Point", ...).
 *
 * @return A string valid until the next call.
 */
const char *type_describe_value(Value v);

/**
 * @brief Checks @p v against @p type, applying the allowed conversions in place.
 *
 * Widens int to float where a float is expected, and lets untyped lists/maps
 * adopt the element type (see file header). A NULL @p type accepts anything.
 *
 * @return 1 if the value is acceptable (possibly after widening), 0 if not.
 */
int type_check_value(TypeSpec *type, Value *v);

/**
 * @brief Slow path of TYPE_REQUIRE(): full check, then a type error on mismatch.
 *
 * @param type The expected type (NULL accepts anything).
 * @param v The value; may be widened in place.
 * @param line Source line for the error.
 * @param what_format printf-style description of what is being stored, e.g.
 *        "variable '%s'" or "argument %d of '%s'", followed by its arguments.
 *        Only formatted when the check fails.
 */
void type_require_slow(TypeSpec *type, Value *v, int line, const char *what_format, ...);

/**
 * @brief Inline fast path: true when @p v trivially satisfies @p type (no
 *        conversion or container check needed).
 */
static inline int type_quick_match(TypeSpec *type, const Value *v)
{
    if (type == NULL)
        return 1;
    switch (type->kind)
    {
        case TYPE_ANY: return 1;
        case TYPE_INT: return v->type == VAL_INT;
        case TYPE_FLOAT: return v->type == VAL_FLOAT;
        case TYPE_STRING: return v->type == VAL_STRING;
        case TYPE_BOOL: return v->type == VAL_BOOL;
        case TYPE_BUFFER: return v->type == VAL_BUFFER;
        default: return 0; // Lists, maps, classes, widening: take the full check.
    }
}

/**
 * @brief Checks @p v (a Value *) against @p type like type_check_value(), and
 *        reports "Type mismatch: <what> expects T, got U." on failure.
 *
 * Runs on every typed store, so the common exact match is checked inline and
 * the message is only formatted on failure. Each argument is evaluated once.
 * Usage: TYPE_REQUIRE(spec, &value, line, "variable '%s'", name);
 */
#define TYPE_REQUIRE(type, v, line, ...)                                         \
    do                                                                           \
    {                                                                            \
        TypeSpec *type_require_type_ = (type);                                   \
        Value *type_require_value_ = (v);                                        \
        if (!type_quick_match(type_require_type_, type_require_value_))          \
            type_require_slow(type_require_type_, type_require_value_, (line), __VA_ARGS__); \
    }                                                                            \
    while (0)

/**
 * @brief Returns the zero value of @p type (0, 0.0, "", false, empty list/map, void).
 *
 * May allocate (strings, lists, maps); the caller must root the result before
 * allocating again.
 */
Value type_zero_value(TypeSpec *type);

#endif // PITH_TYPES_H
