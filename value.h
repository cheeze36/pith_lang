/**
 * @file value.h
 * @brief This file defines the data structures for values in the Pith language.
 * It includes definitions for all value types, as well as structs for functions, modules, and other complex types.
 */

#ifndef PITH_VALUE_H
#define PITH_VALUE_H

#include "parser.h" // For ASTNode

// Forward Declarations
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

// A C function pointer type for our native built-in functions
typedef Value (*NativeFn)(int arg_count, Value* args);

// Enum for all possible value types in the Pith language
typedef enum {
    VAL_INT,
    VAL_FLOAT,
    VAL_STRING,
    VAL_BOOL,
    VAL_VOID,
    VAL_NATIVE_FN,
    VAL_FUNC,
    VAL_MODULE,
    VAL_STRUCT_DEF,
    VAL_STRUCT_INSTANCE,
    VAL_LIST,
    VAL_HASHMAP,
    VAL_CLASS,
    VAL_INSTANCE,
    VAL_BOUND_METHOD,
    VAL_BREAK,
    VAL_CONTINUE
} ValueType;

// --- Garbage Collection Types ---

typedef enum {
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

typedef struct ObjHeader {
    ObjType type;
    unsigned char is_marked;
    struct ObjHeader *next;
} ObjHeader;

// The main Value struct, which can hold any Pith type
struct Value {
    ValueType type;
    union {
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

// A user-defined function, now with a captured environment
struct Func {
    ObjHeader obj;
    char *name;
    ASTNode *body;
    Env *env; // The environment where the function was defined
};

// A module, which is essentially a hashmap of its exported members
struct Module {
    ObjHeader obj;
    char *name;
    HashMap *members;
};

// Definition of a struct (the "blueprint")
struct StructDef {
    ObjHeader obj;
    char *name;
    char **fields;
    int field_count;
};

// An instance of a struct
struct StructInstance {
    ObjHeader obj;
    StructDef *def;
    Value *field_values;
};

// A dynamic list
struct List {
    ObjHeader obj;
    Value *items;
    int count;
    int capacity;
    int is_fixed; // Flag to indicate if the list is fixed-size
};

// An entry in a hashmap (part of a linked list for collision handling)
struct MapEntry {
    char *key;
    Value value;
    MapEntry *next;
};

// The hashmap itself, containing buckets of entries
struct HashMap {
    ObjHeader obj;
    MapEntry **buckets;
    int bucket_count;
    ValueType key_type;
    ValueType value_type;
};

// A class definition
struct PithClass {
    ObjHeader obj;
    char *name;
    HashMap *methods;
    char **fields;
    int field_count;
};

// An instance of a class
struct PithInstance {
    ObjHeader obj;
    PithClass *pith_class;
    HashMap *fields;
};

// A method bound to an instance
struct BoundMethod {
    ObjHeader obj;
    Value receiver; // The instance ('this')
    Value method;   // The function
};

// Environment (Scope)
struct Env {
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
