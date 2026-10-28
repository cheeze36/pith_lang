/**
 * @file value.h
 * @brief Header file defining data structures for Pith values and objects.
 *
 * This file contains the definitions for all value types supported by the language,
 * including primitives (int, float, bool, string) and complex objects (lists, maps, classes, functions).
 * It also defines the structures for garbage-collected objects.
 */

#ifndef PITH_VALUE_H
#define PITH_VALUE_H

#include <stdint.h>
#include <inttypes.h>
#include "parser.h" // For ASTNode

// --- Forward Declarations ---
typedef struct Value Value;
typedef struct Env Env;
typedef struct StructDef StructDef;
typedef struct StructInstance StructInstance;
typedef struct List List;
typedef struct MapEntry MapEntry;
typedef struct HashMap HashMap;
typedef struct Module Module;
typedef struct Func Func;
typedef struct PithClass PithClass;
typedef struct PithInstance PithInstance;
typedef struct BoundMethod BoundMethod;
typedef struct ObjString ObjString;
typedef struct NativeObject NativeObject;
typedef struct Buffer Buffer;

/**
 * @brief Function pointer type for native built-in functions.
 * @param arg_count Number of arguments passed.
 * @param args Array of argument values.
 * @return The result of the function call.
 */
typedef Value (*NativeFn)(int arg_count, Value *args);

/**
 * @brief Enumeration of all possible value types in the Pith language.
 */
// printf format for a Pith int, e.g. printf("n=%" PITH_INT_FMT "\n", v.int_val).
#define PITH_INT_FMT PRId64

typedef enum
{
    VAL_INT, // Integer value
    VAL_FLOAT, // Floating-point value
    VAL_STRING, // String value
    VAL_BOOL, // Boolean value
    VAL_VOID, // Void/Null value
    VAL_NATIVE_FN, // Native C function
    VAL_FUNC, // User-defined function
    VAL_MODULE, // Imported module
    VAL_STRUCT_DEF, // Struct definition (blueprint)
    VAL_STRUCT_INSTANCE, // Instance of a struct
    VAL_LIST, // Dynamic list or array
    VAL_HASHMAP, // Key-value map
    VAL_CLASS, // Class definition
    VAL_INSTANCE, // Instance of a class
    VAL_BOUND_METHOD, // Method bound to an instance
    VAL_NATIVE_OBJECT, // C-backed object with a native class (see native_object.h)
    VAL_BUFFER, // Mutable byte buffer (see pith_buffer.h)
    VAL_BREAK, // Internal: Break signal
    VAL_CONTINUE, // Internal: Continue signal
    VAL_RETURN_VOID // Internal: `return` with no value (or a void value); becomes void when the call ends
} ValueType;

// --- Garbage Collection Types ---

/**
 * @brief Enumeration of object types managed by the Garbage Collector.
 */
typedef enum
{
    OBJ_LIST,
    OBJ_MAP,
    OBJ_FUNC,
    OBJ_MODULE,
    OBJ_CLASS,
    OBJ_INSTANCE,
    OBJ_BOUND_METHOD,
    OBJ_STRUCT_DEF,
    OBJ_STRUCT_INSTANCE,
    OBJ_ENV,
    OBJ_STRING,
    OBJ_NATIVE,
    OBJ_BUFFER
} ObjType;

/**
 * @brief Header for all garbage-collected objects.
 *
 * This struct must be the first member of any GC-managed struct.
 */
typedef struct ObjHeader
{
    ObjType type; // Type of the object
    unsigned char is_marked; // Mark flag for GC
    struct ObjHeader *next; // Pointer to the next object in the global list
} ObjHeader;

/**
 * @brief The main Value struct, which can hold any Pith type.
 *
 * Uses a tagged union to store the actual data.
 */
struct Value
{
    ValueType type;

    union
    {
        int64_t int_val; // VAL_INT (64-bit) and VAL_BOOL (0/1)
        double float_val; // VAL_FLOAT (64-bit IEEE double)
        ObjString *string; // VAL_STRING; see AS_CSTRING / string_new() in pith_string.h
        NativeObject *native; // VAL_NATIVE_OBJECT; see native_object.h
        Buffer *buffer; // VAL_BUFFER; see pith_buffer.h
        NativeFn native_fn;
        Func *func;
        Module *module;
        StructDef *struct_def;
        StructInstance *struct_instance;
        List *list;
        HashMap *hashmap;
        PithClass *pith_class;
        PithInstance *instance;
        BoundMethod *bound_method;
    };
};

// --- Heap Allocated Objects (GC Managed) ---

/**
 * @brief A mutable, growable sequence of bytes: Pith's `buffer` type (see pith_buffer.h).
 *
 * `length` bytes of `data` are in use and `capacity` are allocated. After
 * `buffer.free()` the bytes are released and `is_freed` is set, but this object
 * itself stays valid (it is only reclaimed by the GC once unreachable), so any
 * later use through any reference is detected and reported instead of touching
 * released memory.
 */
struct Buffer
{
    ObjHeader obj;
    char *data; // NULL when capacity is 0 or after free()
    int length; // Bytes in use
    int capacity; // Bytes allocated
    int is_freed; // Set by free(); every operation checks it
};

/**
 * @brief A Pith value backed by C data, e.g. a compiled Regex (see native_object.h).
 */
struct NativeObject
{
    ObjHeader obj;
    struct NativeClass *cls; // Type name, methods and GC hooks.
    void *data; // Owned by the object; released by cls->finalize.
};

/**
 * @brief An immutable string: a GC-managed, length-prefixed byte sequence.
 *
 * Strings may contain any bytes, including NUL, so `length` (not strlen) is
 * authoritative. `chars` is still always followed by a terminating NUL, so text
 * strings can be handed straight to C APIs that expect a C string.
 *
 * Strings are never modified after creation, which lets values share one
 * ObjString freely (assignment does not copy).
 */
struct ObjString
{
    ObjHeader obj;
    int length; // Number of bytes, excluding the terminating NUL.
    char chars[]; // length bytes + '\0'.
};

/**
 * @brief A user-defined function.
 *
 * Captures the environment where it was defined (closure).
 */
struct Func
{
    ObjHeader obj;
    char *name;
    ASTNode *body;
    Env *env; // The environment where the function was defined
    PithClass *owner_class; // The class this function is a method of, if any
};

/**
 * @brief A module, representing a collection of exported members.
 */
struct Module
{
    ObjHeader obj;
    char *name;
    HashMap *members;
};

/**
 * @brief Definition of a struct (the "blueprint").
 */
struct StructDef
{
    ObjHeader obj;
    char *name;
    char **fields;
    int field_count;
};

/**
 * @brief An instance of a struct.
 */
struct StructInstance
{
    ObjHeader obj;
    StructDef *def;
    Value *field_values;
};

/**
 * @brief A dynamic list or fixed-size array.
 */
struct List
{
    ObjHeader obj;
    Value *items;
    int count;
    int capacity;
    int is_fixed; // Flag to indicate if the list is fixed-size
    TypeSpec *element_spec; // Declared element type (see types.h); NULL = not yet typed
};

/**
 * @brief An entry in a hashmap (part of a linked list for collision handling).
 */
struct MapEntry
{
    char *key;
    Value value;
    MapEntry *next;
    long order; // Insertion sequence number; keys(), foreach and print use this order.
};

/**
 * @brief A hashmap, containing buckets of entries.
 */
struct HashMap
{
    ObjHeader obj;
    MapEntry **buckets;
    int bucket_count;
    int count; // Number of entries.
    long next_order; // Sequence number for the next inserted key.
    ValueType key_type;
    ValueType value_type; // Internal tables only (e.g. VAL_NATIVE_FN); VAL_VOID = unchecked
    TypeSpec *value_spec; // Pith maps: declared value type (see types.h); NULL = not yet typed
};

/**
 * @brief A class definition.
 */
struct PithClass
{
    ObjHeader obj;
    char *name;
    HashMap *methods;
    char **fields;
    TypeSpec **field_types; // Declared type of each field (parallel to `fields`)
    int field_count;
    struct PithClass *parent; // The parent class, for inheritance
};

/**
 * @brief An instance of a class.
 */
struct PithInstance
{
    ObjHeader obj;
    PithClass *pith_class;
    HashMap *fields;
};

/**
 * @brief A method bound to a specific instance.
 *
 * When called, 'this' is implicitly set to the receiver.
 */
struct BoundMethod
{
    ObjHeader obj;
    Value receiver; // The instance ('this')
    Value method; // The function
};

/**
 * @brief Environment (Scope) for variable storage.
 *
 * An Env node serves two roles:
 *
 *  1. A *name binding* (the classic form): `name`/`val` hold a single binding and
 *     `next` links to the rest of the chain. Globals, modules, natives, and all
 *     top-level code use these. Lookups are by name (see env_get/env_assign).
 *
 *  2. A *function frame*: `slots` holds an array of local variables addressed by
 *     index, `slot_count` is its length, and `enclosing` points at the frame of
 *     the lexically enclosing function (for closures). Frame nodes have
 *     `name == NULL`; their `next` points at the captured name environment
 *     (the closure's globals/module scope) so name-based fallback still works.
 *
 * The resolver (resolver.c) assigns a (depth, slot) to every local variable
 * reference so the interpreter can index `slots` directly instead of walking the
 * chain and comparing names. References it cannot place statically fall back to
 * name-based lookup over `next`.
 */
struct Env
{
    ObjHeader obj;
    char *name; // Binding name (NULL for function-frame nodes).
    Value val; // Binding value (name nodes only).
    TypeSpec *declared_type; // Declared type of the binding (name nodes); NULL = untyped.
    struct Env *next; // Next link: chain for name nodes; captured name env for frames.

    // Function-frame fields (slots == NULL for plain name-binding nodes):
    Value *slots; // Array of local variable slots.
    TypeSpec **slot_types; // Declared type per slot (owned by the function's AST); NULL entries = untyped.
    int slot_count; // Number of slots.
    struct Env *enclosing; // Enclosing function frame (for closures / depth > 0).
};

/**
 * @brief Prints a textual representation of a Pith value to stdout.
 * @param v The value to print.
 */
void print_value(Value v);

#endif //PITH_VALUE_H
