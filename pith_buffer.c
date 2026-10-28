/**
 * @file pith_buffer.c
 * @brief Implementation of the `buffer` type's constructor and methods (see pith_buffer.h).
 *
 * Indexing, index assignment and slicing are syntax, so they live in the
 * interpreter (interpreter.c); they use buffer_require_live() too.
 *
 * Every method that reads or changes a buffer first calls buffer_require_live(),
 * which is what turns use-after-free into an ordinary Pith error.
 */

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "pith_buffer.h"
#include "interpreter.h"
#include "common.h"
#include "gc.h"
#include "pith_string.h"
#include "types.h"

/** Largest buffer, in bytes: lengths are C ints. */
#define MAX_BUFFER_SIZE INT_MAX

// --- Core operations ---

Buffer *buffer_new(int capacity)
{
    Buffer *buffer = (Buffer *) allocate_obj(sizeof(Buffer), OBJ_BUFFER);
    if (capacity > 0)
    {
        buffer->data = malloc((size_t) capacity);
        if (buffer->data == NULL)
            report_error(get_exec_error_line(), "buffer: out of memory reserving %d bytes.", capacity);
        buffer->capacity = capacity;
    }
    return buffer;
}

void buffer_require_live(Buffer *buffer, int line)
{
    if (buffer->is_freed)
        report_error(line ? line : get_exec_error_line(),
                     "This buffer was freed with free(); it can no longer be used.");
}

/**
 * @brief Grows @p buffer so it can hold at least @p needed bytes.
 *
 * Capacity at least doubles each time, so appending n bytes one piece at a time
 * costs O(n) overall (amortized).
 */
static void buffer_reserve(Buffer *buffer, long needed)
{
    if (needed <= buffer->capacity)
        return;
    if (needed > MAX_BUFFER_SIZE)
        report_error(get_exec_error_line(), "buffer: too large (the maximum is %d bytes).", MAX_BUFFER_SIZE);
    long new_capacity = buffer->capacity > 0 ? buffer->capacity : 16;
    while (new_capacity < needed)
        new_capacity = new_capacity > MAX_BUFFER_SIZE / 2 ? MAX_BUFFER_SIZE : new_capacity * 2;
    char *grown = realloc(buffer->data, (size_t) new_capacity);
    if (grown == NULL)
        report_error(get_exec_error_line(), "buffer: out of memory growing to %ld bytes.", new_capacity);
    buffer->data = grown;
    buffer->capacity = (int) new_capacity;
}

/**
 * @brief Inserts @p length bytes from @p bytes at byte @p index (0..length).
 *
 * @p bytes may point into @p buffer itself (b.append(b)): its offset is taken
 * before growing, because growing can move the data.
 */
static void buffer_insert_bytes(Buffer *buffer, int index, const char *bytes, int length)
{
    if (length == 0)
        return;
    int source_is_self = bytes >= buffer->data && buffer->data != NULL && bytes < buffer->data + buffer->length;
    long source_offset = source_is_self ? (long) (bytes - buffer->data) : 0;

    buffer_reserve(buffer, (long) buffer->length + length);
    if (source_is_self)
        bytes = buffer->data + source_offset;

    // Open a gap at index, then copy into it. When the source is this buffer and
    // lies after index, it has just been shifted right by `length`.
    memmove(buffer->data + index + length, buffer->data + index, (size_t) (buffer->length - index));
    if (source_is_self && source_offset >= index)
        bytes += length;
    if (source_is_self && source_offset < index && source_offset + length > index)
    {
        // The source straddles the gap: copy its two parts separately.
        int before_gap = index - (int) source_offset;
        memcpy(buffer->data + index, bytes, (size_t) before_gap);
        memcpy(buffer->data + index + before_gap, buffer->data + index + length, (size_t) (length - before_gap));
    }
    else
        memcpy(buffer->data + index, bytes, (size_t) length);
    buffer->length += length;
}

void buffer_append_bytes(Buffer *buffer, const char *bytes, int length)
{
    buffer_require_live(buffer, 0);
    buffer_insert_bytes(buffer, buffer->length, bytes, length);
}

// --- Argument helpers ---

/**
 * @brief The receiver (args[0]) of a buffer method, checked to be a live buffer.
 */
static Buffer *receiver(int arg_count, Value *args, int expected_arguments, const char *method, const char *usage)
{
    if (args[0].type != VAL_BUFFER)
        report_error(get_exec_error_line(), "%s() must be called on a buffer.", method);
    Buffer *buffer = args[0].buffer;
    buffer_require_live(buffer, 0);
    if (arg_count != expected_arguments + 1)
        report_error(get_exec_error_line(), "%s() %s.", method, usage);
    return buffer;
}

/**
 * @brief Reads a "text" argument, which may be a string or a buffer.
 */
static void text_argument(Value value, const char *method, const char **bytes, int *length)
{
    if (value.type == VAL_STRING)
    {
        *bytes = AS_CSTRING(value);
        *length = STRING_LENGTH(value);
        return;
    }
    if (value.type == VAL_BUFFER)
    {
        buffer_require_live(value.buffer, 0);
        *bytes = value.buffer->data;
        *length = value.buffer->length;
        return;
    }
    report_error(get_exec_error_line(), "%s(): the text must be a string or a buffer, got %s.", method,
                 type_describe_value(value));
}

/**
 * @brief Reads a byte position argument: 0..length, or negative from the end.
 *
 * @param allow_end Whether `length` itself (one past the last byte) is valid.
 */
static int position_argument(Value value, Buffer *buffer, int allow_end, const char *method, const char *name)
{
    if (value.type != VAL_INT)
        report_error(get_exec_error_line(), "%s(): %s must be an int, got %s.", method, name,
                     type_describe_value(value));
    int64_t position = value.int_val < 0 ? value.int_val + buffer->length : value.int_val;
    int64_t last = allow_end ? buffer->length : (int64_t) buffer->length - 1;
    if (position < 0 || position > last)
        report_error(get_exec_error_line(), "%s(): %s %" PITH_INT_FMT " is out of range (the buffer has %d bytes).",
                     method, name, value.int_val, buffer->length);
    return (int) position;
}

static Value int_value(int64_t n)
{
    Value v;
    v.type = VAL_INT;
    v.int_val = n;
    return v;
}

// --- Constructor ---

// buffer(), buffer(capacity) or buffer(text) -> buffer
Value native_buffer_constructor(int arg_count, Value *args)
{
    if (arg_count > 1)
        report_error(get_exec_error_line(), "buffer() takes an optional capacity (int) or initial text (string or buffer).");
    Value result;
    result.type = VAL_BUFFER;
    if (arg_count == 0)
    {
        result.buffer = buffer_new(0);
        return result;
    }
    if (args[0].type == VAL_INT)
    {
        if (args[0].int_val < 0 || args[0].int_val > MAX_BUFFER_SIZE)
            report_error(get_exec_error_line(), "buffer(): capacity must be between 0 and %d, got %" PITH_INT_FMT ".",
                         MAX_BUFFER_SIZE, args[0].int_val);
        result.buffer = buffer_new((int) args[0].int_val);
        return result;
    }
    const char *bytes;
    int length;
    text_argument(args[0], "buffer", &bytes, &length);
    // The argument is rooted by the interpreter, so `bytes` stays valid across this allocation.
    result.buffer = buffer_new(length);
    buffer_insert_bytes(result.buffer, 0, bytes, length);
    return result;
}

// --- Methods ---

// b.append(text)
static Value method_append(int arg_count, Value *args)
{
    Buffer *buffer = receiver(arg_count, args, 1, "append", "takes exactly one argument (the text)");
    const char *bytes;
    int length;
    text_argument(args[1], "append", &bytes, &length);
    buffer_insert_bytes(buffer, buffer->length, bytes, length);
    return (Value){VAL_VOID};
}

// b.insert(index, text)
static Value method_insert(int arg_count, Value *args)
{
    Buffer *buffer = receiver(arg_count, args, 2, "insert", "takes two arguments (index, text)");
    int index = position_argument(args[1], buffer, 1, "insert", "index");
    const char *bytes;
    int length;
    text_argument(args[2], "insert", &bytes, &length);
    buffer_insert_bytes(buffer, index, bytes, length);
    return (Value){VAL_VOID};
}

// b.remove(start, end): removes bytes start..end-1
static Value method_remove(int arg_count, Value *args)
{
    Buffer *buffer = receiver(arg_count, args, 2, "remove", "takes two arguments (start, end)");
    int start = position_argument(args[1], buffer, 1, "remove", "start");
    int end = position_argument(args[2], buffer, 1, "remove", "end");
    if (end < start)
        report_error(get_exec_error_line(), "remove(): end (%d) is before start (%d).", end, start);
    if (end == start)
        return (Value){VAL_VOID}; // Nothing to remove (and data may be NULL when empty).
    memmove(buffer->data + start, buffer->data + end, (size_t) (buffer->length - end));
    buffer->length -= end - start;
    return (Value){VAL_VOID};
}

// b.clear(): empty, but keep the capacity
static Value method_clear(int arg_count, Value *args)
{
    Buffer *buffer = receiver(arg_count, args, 0, "clear", "takes no arguments");
    buffer->length = 0;
    return (Value){VAL_VOID};
}

// b.find(text) -> int
static Value method_find(int arg_count, Value *args)
{
    Buffer *buffer = receiver(arg_count, args, 1, "find", "takes exactly one argument (the text)");
    const char *needle;
    int needle_length;
    text_argument(args[1], "find", &needle, &needle_length);
    if (needle_length == 0)
        return int_value(0); // The empty text is found at the start (data may be NULL).
    for (int i = 0; i + needle_length <= buffer->length; i++)
    {
        if (memcmp(buffer->data + i, needle, (size_t) needle_length) == 0)
            return int_value(i);
    }
    return int_value(-1);
}

// b.len() -> int
static Value method_len(int arg_count, Value *args)
{
    Buffer *buffer = receiver(arg_count, args, 0, "len", "takes no arguments");
    return int_value(buffer->length);
}

// b.capacity() -> int
static Value method_capacity(int arg_count, Value *args)
{
    Buffer *buffer = receiver(arg_count, args, 0, "capacity", "takes no arguments");
    return int_value(buffer->capacity);
}

// b.to_string() -> string
static Value method_to_string(int arg_count, Value *args)
{
    Buffer *buffer = receiver(arg_count, args, 0, "to_string", "takes no arguments");
    // The receiver is rooted by the interpreter, so its data survives this allocation.
    return string_value(buffer->data, buffer->length);
}

// b.free(): release the bytes now; any later use is an error
static Value method_free(int arg_count, Value *args)
{
    Buffer *buffer = receiver(arg_count, args, 0, "free", "takes no arguments");
    free(buffer->data);
    buffer->data = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
    buffer->is_freed = 1;
    return (Value){VAL_VOID};
}

// b.is_freed() -> bool (the one method that is allowed after free())
static Value method_is_freed(int arg_count, Value *args)
{
    if (args[0].type != VAL_BUFFER || arg_count != 1)
        report_error(get_exec_error_line(), "is_freed() takes no arguments.");
    Value v;
    v.type = VAL_BOOL;
    v.int_val = args[0].buffer->is_freed;
    return v;
}

// --- Method table ---

static Value method_table = {VAL_VOID};

HashMap *buffer_methods(void)
{
    if (method_table.type != VAL_HASHMAP)
    {
        method_table.type = VAL_HASHMAP;
        method_table.hashmap = hashmap_create(VAL_STRING, VAL_NATIVE_FN);
        gc_register_value_root(&method_table);
        HashMap *methods = method_table.hashmap;
        register_native_method(methods, "append", method_append);
        register_native_method(methods, "insert", method_insert);
        register_native_method(methods, "remove", method_remove);
        register_native_method(methods, "clear", method_clear);
        register_native_method(methods, "find", method_find);
        register_native_method(methods, "len", method_len);
        register_native_method(methods, "capacity", method_capacity);
        register_native_method(methods, "to_string", method_to_string);
        register_native_method(methods, "free", method_free);
        register_native_method(methods, "is_freed", method_is_freed);
    }
    return method_table.hashmap;
}
