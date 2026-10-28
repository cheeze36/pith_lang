/**
 * @file pith_buffer.h
 * @brief Pith's built-in `buffer` type: a mutable, growable sequence of bytes.
 *
 * Strings are immutable, so building a long string with `+` in a loop copies
 * the whole string on every step (quadratic time). A buffer is the mutable
 * counterpart: append to it as often as needed, then take an immutable string
 * with to_string(). It holds bytes, so it works for text and binary data alike.
 *
 * @code
 *   buffer out = buffer()                # empty
 *   buffer sized = buffer(4096)          # empty, 4096 bytes reserved
 *   buffer greeting = buffer("Hello")    # starts with a copy of the text
 *
 *   out.append("<ul>")
 *   foreach (string item in items):
 *       out.append("<li>")
 *       out.append(item)
 *       out.append("</li>")
 *   out.append("</ul>")
 *   string html = out.to_string()
 *
 *   string first = greeting[0]           # "H" (a one-byte string)
 *   greeting[0] = "J"                    # "Jello"
 *   string tail = greeting[1:]           # "ello" (a string copy)
 *
 *   out.free()                           # release the memory now
 *   out.append("x")                      # error: this buffer was freed
 * @endcode
 *
 * ## Creating
 *
 *  - `buffer()`               An empty buffer.
 *  - `buffer(capacity)`       An empty buffer with room for @c capacity bytes
 *                             (an optimization; it grows as needed anyway).
 *  - `buffer(text)`           A buffer holding a copy of @c text (a string or buffer).
 *  - `buffer b` (a declaration without `=`) is an empty buffer, like the other
 *    non-class types' zero values.
 *
 * ## Methods
 *
 *  - `append(text)`           Add a string or buffer at the end.
 *  - `insert(index, text)`    Insert a string or buffer before byte @c index (0..len()).
 *  - `remove(start, end)`     Remove bytes start..end-1.
 *  - `clear()`                Remove all bytes, keeping the allocated capacity for reuse.
 *  - `find(text)`             -> int. Byte index of the first occurrence of @c text, or -1.
 *  - `len()`                  -> int. Bytes in use.
 *  - `capacity()`             -> int. Bytes allocated.
 *  - `to_string()`            -> string. An immutable copy of the contents
 *                             (`str(b)` does the same).
 *  - `free()`                 Release the memory now (see below).
 *  - `is_freed()`             -> bool. Whether free() has been called.
 *
 * ## Indexing and slicing (byte positions, like strings)
 *
 *  - `b[i]`                   -> string of one byte. Negative indexes count from the end.
 *  - `b[i] = "x"`             Replace one byte; the value must be exactly one byte.
 *  - `b[start:end]`           -> string copy of a range (bounds as for string slices).
 *
 * Methods that take positions (insert, remove) also accept negative positions,
 * counted from the end, but never clamp: an out-of-range position is an error.
 *
 * ## Releasing memory: free()
 *
 * Unused buffers are reclaimed by the garbage collector automatically, so
 * free() is optional. It exists for releasing a large buffer's memory at a
 * known point. After free(), *every* use of that buffer, through any variable
 * that refers to it, raises "this buffer was freed" (except is_freed()). This
 * is safe by construction: free() releases only the bytes; the buffer object
 * stays valid and remembers that it was freed, so a stale reference can never
 * read memory that has since been reused for something else.
 *
 * ## Typing
 *
 * `buffer` is its own type: no implicit conversion to or from `string`. Pass
 * `b.to_string()` where a string is expected. Two buffers compare equal with ==
 * only if they are the same buffer (like lists); compare contents with
 * `a.to_string() == b.to_string()`.
 */

#ifndef PITH_BUFFER_H
#define PITH_BUFFER_H

#include "value.h"

/**
 * @brief Creates an empty buffer with @p capacity bytes reserved.
 *
 * Allocates a GC object (may collect).
 */
Buffer *buffer_new(int capacity);

/**
 * @brief Raises the "this buffer was freed" error if free() was called on @p buffer.
 *
 * @param line Source line for the error (0 = current native call's line).
 */
void buffer_require_live(Buffer *buffer, int line);

/**
 * @brief Appends @p length bytes to a live buffer, growing it as needed.
 *
 * For native code that builds output in a buffer (e.g. the template renderer):
 * because the buffer is a GC object, its memory is reclaimed even if an error
 * interrupts the work.
 */
void buffer_append_bytes(Buffer *buffer, const char *bytes, int length);

/**
 * @brief The native `buffer(...)` constructor (registered as a global function).
 */
Value native_buffer_constructor(int arg_count, Value *args);

/**
 * @brief The method table for buffer values (append, len, ...), created on first use.
 *
 * Kept alive as a registered GC root.
 */
HashMap *buffer_methods(void);

#endif // PITH_BUFFER_H
