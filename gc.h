#ifndef PITH_GC_H
#define PITH_GC_H

#include <stddef.h>
#include "value.h"

// Allocates a new object on the heap and tracks it for garbage collection.
void* allocate_obj(size_t size, ObjType type);

// Triggers a garbage collection cycle.
void gc_collect();

// Frees all allocated objects (called at interpreter exit).
void free_all_objects();

// Debugging helper to print GC stats
void print_gc_stats();

#endif //PITH_GC_H
