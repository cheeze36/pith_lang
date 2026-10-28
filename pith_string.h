/**
 * @file pith_string.h
 * @brief Pith's immutable, GC-managed string type (ObjString) and helpers.
 *
 * ## Representation
 *
 * A Pith string is an ObjString: a GC object holding a byte length and the bytes
 * themselves, followed by a NUL terminator. Strings can therefore hold binary
 * data (embedded NULs included), while text strings can still be passed directly
 * to C APIs through AS_CSTRING(). Strings are immutable, so values share them
 * instead of copying.
 *
 * ## Units
 *
 * Lengths, indexes and slices count **bytes** (`len()`, `s[i]`, `s[a:b]`).
 * `count()` counts UTF-8 characters (code points).
 *
 * ## GC rule
 *
 * Every function here that returns a new ObjString allocates, which may trigger
 * a collection. Any *other* string passed in (e.g. the operands of
 * string_concat()) must be reachable from a GC root for the duration of the
 * call. Arguments to native functions already are (the interpreter roots them),
 * as are both operands of a binary operator.
 *
 * The file is named pith_string.* so it never shadows the system <string.h>.
 */

#ifndef PITH_STRING_H
#define PITH_STRING_H

#include "value.h"

/** @brief The NUL-terminated bytes of a VAL_STRING Value. */
#define AS_CSTRING(value) ((value).string->chars)

/** @brief The byte length of a VAL_STRING Value. */
#define STRING_LENGTH(value) ((value).string->length)

/**
 * @brief Allocates a string of @p length bytes whose contents the caller fills in.
 *
 * The terminating NUL is already written. The caller must finish writing
 * `chars` before the string becomes visible to Pith code (strings are immutable
 * from then on).
 */
ObjString *string_alloc(int length);

/**
 * @brief Creates a new string by copying @p length bytes from @p chars.
 *
 * @p chars may contain NUL bytes and need not be NUL-terminated.
 */
ObjString *string_new(const char *chars, int length);

/**
 * @brief Creates a new string from a NUL-terminated C string.
 */
ObjString *string_from_cstr(const char *cstr);

/**
 * @brief Creates a string Value copying @p length bytes from @p chars.
 */
Value string_value(const char *chars, int length);

/**
 * @brief Creates a string Value from a NUL-terminated C string.
 */
Value cstring_value(const char *cstr);

/**
 * @brief Returns a new string holding @p a followed by @p b.
 *
 * Both operands must be GC-rooted by the caller (see the file header).
 */
ObjString *string_concat(ObjString *a, ObjString *b);

/**
 * @brief Byte-wise equality of two strings.
 *
 * @return 1 if the strings have the same length and bytes, 0 otherwise.
 */
int string_equals(ObjString *a, ObjString *b);

/**
 * @brief Counts the UTF-8 characters (code points) in a string.
 *
 * Counts every byte that is not a UTF-8 continuation byte, so invalid UTF-8
 * still produces a sensible answer instead of an error.
 */
int string_char_count(ObjString *s);

#endif // PITH_STRING_H
