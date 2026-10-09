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

/**
 * @brief Pushes an object onto the temporary root stack.
 *
 * Protects the object from being collected if a GC cycle occurs while the object
 * is only referenced from the C stack (and not yet linked into the object graph).
 *
 * @param obj The object to protect.
 */
void gc_push_root(ObjHeader *obj);

/**
 * @brief Pops the last object from the temporary root stack.
 */
void gc_pop_root();

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
 * @brief Clears the temporary and active-environment root stacks.
 *
 * Intended for the REPL's error-recovery path: longjmp unwinds the C stack
 * without running the paired gc_pop_* calls, so the stacks must be reset to
 * avoid stale (and dangling) root entries.
 */
void gc_reset_roots(void);

#endif //PITH_GC_H
