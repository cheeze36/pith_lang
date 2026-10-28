/**
 * @file pith_string.c
 * @brief Implementation of Pith's immutable, GC-managed strings.
 *
 * See pith_string.h for the representation and the GC rule that callers of the
 * allocating functions must follow.
 */

#include <string.h>

#include "pith_string.h"
#include "gc.h"

ObjString *string_alloc(int length)
{
    // One allocation holds the header, the bytes and the NUL terminator.
    ObjString *s = (ObjString *) allocate_obj(sizeof(ObjString) + (size_t) length + 1, OBJ_STRING);
    s->length = length;
    s->chars[length] = '\0';
    return s;
}

ObjString *string_new(const char *chars, int length)
{
    ObjString *s = string_alloc(length);
    if (length > 0)
        memcpy(s->chars, chars, (size_t) length);
    return s;
}

ObjString *string_from_cstr(const char *cstr)
{
    return string_new(cstr, (int) strlen(cstr));
}

Value string_value(const char *chars, int length)
{
    Value v;
    v.type = VAL_STRING;
    v.string = string_new(chars, length);
    return v;
}

Value cstring_value(const char *cstr)
{
    return string_value(cstr, (int) strlen(cstr));
}

ObjString *string_concat(ObjString *a, ObjString *b)
{
    ObjString *s = string_alloc(a->length + b->length);
    memcpy(s->chars, a->chars, (size_t) a->length);
    memcpy(s->chars + a->length, b->chars, (size_t) b->length);
    return s;
}

int string_equals(ObjString *a, ObjString *b)
{
    if (a == b)
        return 1;
    return a->length == b->length && memcmp(a->chars, b->chars, (size_t) a->length) == 0;
}

int string_char_count(ObjString *s)
{
    int count = 0;
    for (int i = 0; i < s->length; i++)
    {
        // UTF-8 continuation bytes look like 10xxxxxx; every other byte starts a character.
        if (((unsigned char) s->chars[i] & 0xC0) != 0x80)
            count++;
    }
    return count;
}
