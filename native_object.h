/**
 * @file native_object.h
 * @brief Native objects: Pith values backed by C data (e.g. a compiled Regex).
 *
 * A native module defines a NativeClass (a type name, methods implemented in C,
 * and optional GC hooks) and wraps its C data in NativeObjects. To Pith code
 * they behave like class instances:
 *
 *  - declared by type name:   `Regex r = regex.compile(r"\d+")`
 *  - methods via dot syntax:  `r.find(text)` (the native function receives the
 *    object as args[0], like string and list methods)
 *  - nullable like classes:   a function may return void ("no object"), tested
 *    with `m == void`
 *  - compared by identity with == / !=
 *
 * The garbage collector frees a native object when it becomes unreachable and
 * calls the class's `finalize` hook first, so C resources (memory, handles) are
 * released automatically. If the C data holds Pith values (e.g. a Match keeps
 * its subject string alive), the class's `mark` hook must mark them.
 */

#ifndef PITH_NATIVE_OBJECT_H
#define PITH_NATIVE_OBJECT_H

#include "value.h"

/** @brief Releases the C data of a native object (called by the GC). */
typedef void (*NativeFinalizeFn)(void *data);

/** @brief Marks Pith values referenced by a native object's C data (called by the GC). */
typedef void (*NativeMarkFn)(void *data);

/**
 * @brief A native type: its Pith-visible name, methods and GC hooks.
 *
 * Classes are created once at start-up and live for the whole run.
 */
typedef struct NativeClass
{
    const char *name; // Type name used in declarations and messages, e.g. "Regex".
    Value methods; // VAL_HASHMAP of VAL_NATIVE_FN; registered as a GC root.
    NativeFinalizeFn finalize; // May be NULL.
    NativeMarkFn mark; // May be NULL.
} NativeClass;

/**
 * @brief Creates a native class.
 *
 * @param name The type name (a string literal; it is not copied).
 * @param finalize Called with the object's data when it is collected (may be NULL).
 * @param mark Marks Pith values the data references (may be NULL).
 * @return The class (never freed).
 */
NativeClass *native_class_create(const char *name, NativeFinalizeFn finalize, NativeMarkFn mark);

/**
 * @brief Looks up a native class by its type name.
 *
 * Type checks compare type names, so native type names are reserved: the
 * interpreter refuses a Pith `class` with the same name (otherwise a variable
 * declared as that type would accept both kinds of object).
 *
 * @return The class, or NULL if no native class has that name.
 */
NativeClass *native_class_find(const char *name);

/**
 * @brief Adds a method; it is called with the object as args[0].
 */
void native_class_add_method(NativeClass *cls, const char *name, NativeFn function);

/**
 * @brief Wraps @p data in a new native object of class @p cls.
 *
 * Allocates (may collect): if @p data references Pith values, they must still
 * be rooted by the caller until this returns.
 */
Value native_object_new(NativeClass *cls, void *data);

/**
 * @brief Returns the C data of @p v, raising a Pith error unless @p v is a
 *        native object of class @p cls.
 *
 * @param fn_name The calling function, for the error message.
 */
void *native_object_data(Value v, NativeClass *cls, const char *fn_name);

#endif // PITH_NATIVE_OBJECT_H
