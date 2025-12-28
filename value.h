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
    VAL_BREAK, // Internal: Break signal
    VAL_CONTINUE // Internal: Continue signal
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
    OBJ_ENV
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
        int int_val;
        float float_val;
        char *str_val;
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
    ValueType element_type; // VAL_VOID means unknown / not enforced
};

/**
 * @brief An entry in a hashmap (part of a linked list for collision handling).
 */
struct MapEntry
{
    char *key;
    Value value;
    MapEntry *next;
};

/**
 * @brief A hashmap, containing buckets of entries.
 */
struct HashMap
{
    ObjHeader obj;
    MapEntry **buckets;
    int bucket_count;
    ValueType key_type;
    ValueType value_type;
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
 * Implemented as a linked list of variable bindings.
 */
struct Env
{
    ObjHeader obj;
    char *name;
    Value val;
    struct Env *next;
};

/**
 * @brief Prints a textual representation of a Pith value to stdout.
 * @param v The value to print.
 */
void print_value(Value v);

#endif //PITH_VALUE_H
