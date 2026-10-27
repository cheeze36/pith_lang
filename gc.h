/**
 * @file gc.h
 * @brief Header file for the Garbage Collector.
 *
 * Defines the interface for memory allocation and garbage collection.
 */

#ifndef PITH_GC_H
#define PITH_GC_H

#include <stddef.h>
#include "value.h"

/**
 * @brief Allocates a new object on the heap and tracks it for garbage collection.
 *
 * If the allocation threshold is exceeded, a GC cycle is triggered before allocation.
 *
 * @param size The size of the object in bytes.
 * @param type The type of the object (used for marking).
 * @return A pointer to the allocated memory.
 */
void *allocate_obj(size_t size, ObjType type);

/**
 * @brief Triggers a garbage collection cycle.
 *
 * Marks all reachable objects starting from roots and frees unreachable ones.
 */
void gc_collect();

/**
 * @brief Frees all allocated objects.
 *
 * Should be called at interpreter exit to clean up all memory.
 */
void free_all_objects();

/**
 * @brief Prints current GC statistics (allocated bytes, threshold).
 */
void print_gc_stats();

// --- Temporary Root Management ---
//
// These run on every operator and call, so the fast paths are inline.

extern ObjHeader **temp_roots;
extern int temp_root_count;
extern int temp_root_capacity;

/**
 * @brief Grows the temporary root stack (slow path of gc_push_root()).
 */
void gc_grow_temp_roots(void);

/**
 * @brief Pushes an object onto the temporary root stack.
 *
 * Protects the object from being collected if a GC cycle occurs while the object
 * is only referenced from the C stack (and not yet linked into the object graph).
 *
 * @param obj The object to protect (NULL is allowed and ignored by the marker).
 */
static inline void gc_push_root(ObjHeader *obj)
{
    if (temp_root_count >= temp_root_capacity)
        gc_grow_temp_roots();
    temp_roots[temp_root_count++] = obj;
}

/**
 * @brief Pops the last object from the temporary root stack.
 */
void gc_pop_root();

/**
 * @brief Returns the heap object a value refers to, or NULL for non-heap values.
 */
static inline ObjHeader *value_object(Value v)
{
    switch (v.type)
    {
        case VAL_STRING: return (ObjHeader *) v.string;
        case VAL_LIST: return (ObjHeader *) v.list;
        case VAL_HASHMAP: return (ObjHeader *) v.hashmap;
        case VAL_FUNC: return (ObjHeader *) v.func;
        case VAL_MODULE: return (ObjHeader *) v.module;
        case VAL_CLASS: return (ObjHeader *) v.pith_class;
        case VAL_INSTANCE: return (ObjHeader *) v.instance;
        case VAL_BOUND_METHOD: return (ObjHeader *) v.bound_method;
        case VAL_STRUCT_DEF: return (ObjHeader *) v.struct_def;
        case VAL_STRUCT_INSTANCE: return (ObjHeader *) v.struct_instance;
        case VAL_NATIVE_OBJECT: return (ObjHeader *) v.native;
        default: return NULL; // Numbers, bools, void, native functions: nothing to collect.
    }
}

/**
 * @brief Pushes a value onto the temporary root stack.
 *
 * Use for intermediate values that live only in C locals while further
 * allocation may happen (operands, call arguments, a collection being iterated).
 * Non-heap values push a NULL placeholder, so pushes and pops always balance.
 *
 * @param v The value to protect.
 */
static inline void gc_push_value(Value v)
{
    gc_push_root(value_object(v));
}

/**
 * @brief Pushes a value only if it refers to a heap object.
 *
 * Cheaper than gc_push_value() for numbers and bools, but the number of pushes
 * then varies, so it may only be paired with gc_temp_root_depth() /
 * gc_pop_roots_to(), never with gc_pop_root().
 *
 * @param v The value to protect.
 */
static inline void gc_push_value_if_heap(Value v)
{
    ObjHeader *obj = value_object(v);
    if (obj != NULL)
        gc_push_root(obj);
}

/**
 * @brief Returns the current depth of the temporary root stack.
 *
 * Pair with gc_pop_roots_to() to release everything pushed since, which is
 * simpler than counting pops when several values were pushed.
 */
static inline int gc_temp_root_depth(void)
{
    return temp_root_count;
}

/**
 * @brief Pops the temporary root stack back to @p depth.
 *
 * @param depth A depth previously returned by gc_temp_root_depth().
 */
static inline void gc_pop_roots_to(int depth)
{
    temp_root_count = depth;
}

// --- Active Environment Root Management ---

/**
 * @brief Registers a live environment variable as a GC root.
 *
 * Call frames and block scopes are held only in C local variables while they
 * execute. Without this, a GC cycle triggered mid-execution (e.g. by allocation
 * during deep recursion) would collect the active frame's bindings. Because the
 * caller passes the *address* of its `Env *` local, the collector always marks
 * the current head of the chain even as new bindings are prepended to it.
 *
 * Each call must be paired with gc_pop_env_root().
 *
 * @param slot Address of the C local holding the active environment.
 */
void gc_push_env_root(Env **slot);

/**
 * @brief Unregisters the most recently registered active-environment root.
 */
void gc_pop_env_root();

/**
 * @brief Marks a value (and everything it references) as reachable.
 *
 * Only for use inside a NativeMarkFn hook (see native_object.h), i.e. during
 * a collection's mark phase.
 *
 * @param v The value to mark.
 */
void mark_value(Value v);

/**
 * @brief Registers a C global holding a Pith value as a permanent GC root.
 *
 * For values that live outside any Pith variable for the rest of the run, such
 * as callbacks stored by a native module. The slot is read on every collection,
 * so it may be reassigned freely after registration.
 *
 * @param slot Address of the global (must stay valid for the whole run).
 */
void gc_register_value_root(Value *slot);

/**
 * @brief Clears the temporary and active-environment root stacks.
 *
 * Intended for the REPL's error-recovery path: longjmp unwinds the C stack
 * without running the paired gc_pop_* calls, so the stacks must be reset to
 * avoid stale (and dangling) root entries.
 */
void gc_reset_roots(void);

/**
 * @brief Saved depths of the temporary and active-environment root stacks.
 */
typedef struct
{
    int temp_root_count;
    int env_root_count;
} GcRootSnapshot;

/**
 * @brief Records the current depth of both root stacks.
 *
 * Paired with gc_restore_root_snapshot() by `try` statements: an error raised
 * inside the try body longjmps past the gc_pop_* calls of every frame it unwinds,
 * so the stacks are truncated back to the depth they had when the try began.
 *
 * @return The current stack depths.
 */
GcRootSnapshot gc_root_snapshot(void);

/**
 * @brief Truncates both root stacks back to a previously recorded depth.
 *
 * @param snapshot Depths returned by an earlier gc_root_snapshot().
 */
void gc_restore_root_snapshot(GcRootSnapshot snapshot);

#endif //PITH_GC_H
