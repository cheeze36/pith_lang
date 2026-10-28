/**
 * @file gc.c
 * @brief Implementation of the Garbage Collector.
 *
 * This file implements a simple mark-and-sweep garbage collector.
 * It manages memory allocation for all dynamic objects in the language (lists, maps, instances, etc.).
 * The GC tracks allocated bytes and triggers a collection cycle when a threshold is reached.
 */

#include "gc.h"
#include "interpreter.h"
#include "native_object.h"
#include <stdlib.h>
#include <stdio.h>

// Linked list of all allocated objects
ObjHeader *objects = NULL;

// --- Temporary Root Stack ---
// Protects objects that are only referenced from the C stack: objects under
// construction, and intermediate values during evaluation (the left operand of
// a binary operator, call arguments, ...). It grows on demand because deep Pith
// recursion keeps a few entries alive per active call.
// The push/pop fast paths are inline functions in gc.h.
ObjHeader **temp_roots = NULL;
int temp_root_capacity = 0;
int temp_root_count = 0;

// --- Active Environment Root Stack ---
// Holds the addresses of the C local variables that point at the environment
// currently being executed (one per active call frame / block scope). The sizing
// mirrors C recursion depth, since each nested interpreter call registers one
// slot; the tree-walker would overflow the C stack before this fills.
#define MAX_ENV_ROOTS 100000
Env **env_roots[MAX_ENV_ROOTS];
int env_root_count = 0;

// --- Registered Value Roots ---
// Addresses of C globals that hold Pith values (e.g. the http module's route
// handlers). Registered once and marked on every collection.
#define MAX_VALUE_ROOTS 64
static Value *value_roots[MAX_VALUE_ROOTS];
static int value_root_count = 0;

// --- Allocation Tracking ---
size_t bytes_allocated = 0;
size_t next_gc_threshold = 1024 * 1024; // Start at 1MB

// Forward declarations
void mark_object(ObjHeader *obj);

void mark_value(Value v);

void gc_grow_temp_roots(void)
{
    temp_root_capacity = temp_root_capacity == 0 ? 256 : temp_root_capacity * 2;
    temp_roots = realloc(temp_roots, sizeof(ObjHeader *) * (size_t) temp_root_capacity);
    if (temp_roots == NULL)
    {
        fprintf(stderr, "Fatal: Out of memory (GC temp root stack).\n");
        exit(1);
    }
}

/**
 * @brief Pops the last object from the temporary root stack.
 */
void gc_pop_root()
{
    if (temp_root_count > 0)
    {
        temp_root_count--;
    }
    else
    {
        fprintf(stderr, "Fatal: GC temp root stack underflow.\n");
        exit(1);
    }
}

void gc_push_env_root(Env **slot)
{
    if (env_root_count >= MAX_ENV_ROOTS)
    {
        fprintf(stderr, "Fatal: GC environment root stack overflow (recursion too deep).\n");
        exit(1);
    }
    env_roots[env_root_count++] = slot;
}

void gc_pop_env_root()
{
    if (env_root_count > 0)
    {
        env_root_count--;
    }
    else
    {
        fprintf(stderr, "Fatal: GC environment root stack underflow.\n");
        exit(1);
    }
}

void gc_register_value_root(Value *slot)
{
    if (value_root_count >= MAX_VALUE_ROOTS)
    {
        fprintf(stderr, "Fatal: too many registered GC value roots.\n");
        exit(1);
    }
    value_roots[value_root_count++] = slot;
}

void gc_reset_roots(void)
{
    // The REPL recovers from errors with longjmp, which unwinds the C stack
    // without running the matching gc_pop_* calls. Those stacks would then hold
    // stale slots (env_roots even point at dead stack frames). Reset both to a
    // clean state at the REPL's recovery point before the next command runs.
    temp_root_count = 0;
    env_root_count = 0;
}

GcRootSnapshot gc_root_snapshot(void)
{
    GcRootSnapshot snapshot;
    snapshot.temp_root_count = temp_root_count;
    snapshot.env_root_count = env_root_count;
    return snapshot;
}

void gc_restore_root_snapshot(GcRootSnapshot snapshot)
{
    temp_root_count = snapshot.temp_root_count;
    env_root_count = snapshot.env_root_count;
}

/**
 * @brief Allocates memory for a new garbage-collected object.
 *
 * Triggers a GC cycle if the allocation threshold is exceeded.
 *
 * @param size The size of the object in bytes.
 * @param type The type of the object.
 * @return A pointer to the allocated memory.
 */
void *allocate_obj(size_t size, ObjType type)
{
    // Check if we need to collect. DEBUG_STRESS_GC collects on every allocation
    // so that any value missing a GC root is freed (and caught) immediately.
#ifdef DEBUG_STRESS_GC
    gc_collect();
#else
    if (bytes_allocated > next_gc_threshold)
    {
        gc_collect();
    }
#endif

    // Zeroed so that an object pushed as a root before all of its fields are set
    // never exposes garbage pointers to the marker.
    ObjHeader *obj = calloc(1, size);
    if (obj == NULL)
    {
        fprintf(stderr, "Fatal: Out of memory.\n");
        exit(1);
    }
    obj->type = type;
    obj->is_marked = 0;
    obj->next = objects;
    objects = obj;

    bytes_allocated += size;

#ifdef DEBUG_TRACE_MEMORY
    printf("[GC] Allocated object %p of type %d. Total bytes: %zu\n", (void *) obj, type, bytes_allocated);
#endif

    return obj;
}

/**
 * @brief Marks a value as reachable.
 *
 * If the value contains a pointer to a heap object, that object is marked.
 *
 * @param v The value to mark.
 */
void mark_value(Value v)
{
    mark_object(value_object(v));
}

// --- Gray Stack ---
// Marking is iterative ("tri-color" marking): mark_object() marks an object and
// pushes it here ("gray": reachable, references not yet traced), and
// trace_gray_objects() pops objects and marks what they reference until the
// stack is empty. Recursing into children instead would use C stack space
// proportional to the depth of the data, and a deeply nested structure (e.g. a
// 200,000-level nested list) would overflow the C stack mid-collection.
static ObjHeader **gray_stack = NULL;
static int gray_count = 0;
static int gray_capacity = 0;

/**
 * @brief Marks an object as reachable and queues it so its references get traced.
 *
 * Does nothing for NULL or already-marked objects, so cycles terminate.
 *
 * @param obj The object to mark.
 */
void mark_object(ObjHeader *obj)
{
    if (obj == NULL || obj->is_marked)
        return;

#ifdef DEBUG_TRACE_MEMORY
    printf("[GC] Marking object %p of type %d\n", (void *) obj, obj->type);
#endif

    obj->is_marked = 1;
    if (gray_count == gray_capacity)
    {
        gray_capacity = gray_capacity == 0 ? 256 : gray_capacity * 2;
        gray_stack = realloc(gray_stack, sizeof(ObjHeader *) * (size_t) gray_capacity);
        if (gray_stack == NULL)
        {
            fprintf(stderr, "Fatal: Out of memory (GC gray stack).\n");
            exit(1);
        }
    }
    gray_stack[gray_count++] = obj;
}

/**
 * @brief Marks every object directly referenced by @p obj.
 *
 * @param obj An object that has already been marked.
 */
static void trace_references(ObjHeader *obj)
{
    switch (obj->type)
    {
        case OBJ_LIST:
        {
            List *list = (List *) obj;
            for (int i = 0; i < list->count; i++)
            {
                mark_value(list->items[i]);
            }
            break;
        }
        case OBJ_MAP:
        {
            HashMap *map = (HashMap *) obj;
            for (int i = 0; i < map->bucket_count; i++)
            {
                MapEntry *entry = map->buckets[i];
                while (entry)
                {
                    mark_value(entry->value);
                    entry = entry->next;
                }
            }
            break;
        }
        case OBJ_FUNC:
        {
            Func *func = (Func *) obj;
            mark_object((ObjHeader *) func->env);
            break;
        }
        case OBJ_MODULE:
        {
            Module *module = (Module *) obj;
            mark_object((ObjHeader *) module->members);
            break;
        }
        case OBJ_CLASS:
        {
            PithClass *cls = (PithClass *) obj;
            mark_object((ObjHeader *) cls->methods);
            if (cls->parent)
            {
                mark_object((ObjHeader *) cls->parent);
            }
            break;
        }
        case OBJ_INSTANCE:
        {
            PithInstance *inst = (PithInstance *) obj;
            mark_object((ObjHeader *) inst->pith_class);
            mark_object((ObjHeader *) inst->fields);
            break;
        }
        case OBJ_BOUND_METHOD:
        {
            BoundMethod *bm = (BoundMethod *) obj;
            mark_value(bm->receiver);
            mark_value(bm->method);
            break;
        }
        case OBJ_ENV:
        {
            Env *env = (Env *) obj;
            mark_value(env->val);
            mark_object((ObjHeader *) env->next);
            // Function-frame nodes also own local slots and an enclosing frame.
            for (int i = 0; i < env->slot_count; i++)
                mark_value(env->slots[i]);
            mark_object((ObjHeader *) env->enclosing);
            break;
        }
        case OBJ_STRUCT_DEF:
        case OBJ_STRING:
        case OBJ_BUFFER:
            // No child objects to mark
            break;
        case OBJ_NATIVE:
        {
            // The class knows which Pith values its C data references.
            NativeObject *native = (NativeObject *) obj;
            if (native->cls->mark)
                native->cls->mark(native->data);
            break;
        }
        case OBJ_STRUCT_INSTANCE:
        {
            StructInstance *inst = (StructInstance *) obj;
            mark_object((ObjHeader *) inst->def);
            for (int i = 0; i < inst->def->field_count; i++)
            {
                mark_value(inst->field_values[i]);
            }
            break;
        }
    }
}

/**
 * @brief Traces queued objects until everything reachable has been marked.
 */
static void trace_gray_objects(void)
{
    while (gray_count > 0)
        trace_references(gray_stack[--gray_count]);
}

/**
 * @brief Marks all root objects.
 *
 * Roots include the global environment, native registries, and the temporary root stack.
 */
void mark_roots()
{
    // Mark the global environment
    mark_object((ObjHeader *) global_env);

    // Mark native registries
    mark_object((ObjHeader *) native_string_methods);
    mark_object((ObjHeader *) native_list_methods);
    mark_object((ObjHeader *) native_map_methods);
    mark_object((ObjHeader *) native_module_funcs);

    // Mark C globals registered with gc_register_value_root()
    for (int i = 0; i < value_root_count; i++)
        mark_value(*value_roots[i]);

    // Mark temporary roots (from C stack)
    for (int i = 0; i < temp_root_count; i++)
    {
        mark_object(temp_roots[i]);
    }

    // Mark the environment chain of every active call frame / block scope.
    // Each slot points at a live C local, so we read the current head via *slot;
    // marking an Env also marks its parent chain (see trace_references, OBJ_ENV).
    for (int i = 0; i < env_root_count; i++)
    {
        mark_object((ObjHeader *) *env_roots[i]);
    }
}

/**
 * @brief Sweeps through the object list and frees unmarked objects.
 *
 * Also resets the `is_marked` flag for surviving objects.
 */
void sweep()
{
    ObjHeader **object = &objects;
    while (*object)
    {
        if (!(*object)->is_marked)
        {
            ObjHeader *unreached = *object;
            *object = unreached->next;

#ifdef DEBUG_TRACE_MEMORY
            printf("[GC] Freeing object %p of type %d\n", (void *) unreached, unreached->type);
#endif

            // Free specific resources
            switch (unreached->type)
            {
                case OBJ_LIST:
                {
                    List *list = (List *) unreached;
                    free(list->items);
                    bytes_allocated -= sizeof(List);
                    break;
                }
                case OBJ_MAP:
                {
                    HashMap *map = (HashMap *) unreached;
                    for (int i = 0; i < map->bucket_count; i++)
                    {
                        MapEntry *entry = map->buckets[i];
                        while (entry)
                        {
                            MapEntry *next = entry->next;
                            free(entry->key);
                            free(entry);
                            entry = next;
                        }
                    }
                    free(map->buckets);
                    bytes_allocated -= sizeof(HashMap);
                    break;
                }
                case OBJ_FUNC:
                {
                    Func *func = (Func *) unreached;
                    free(func->name);
                    // Body is AST, not freed here
                    bytes_allocated -= sizeof(Func);
                    break;
                }
                case OBJ_MODULE:
                {
                    Module *mod = (Module *) unreached;
                    free(mod->name);
                    bytes_allocated -= sizeof(Module);
                    break;
                }
                case OBJ_CLASS:
                {
                    PithClass *cls = (PithClass *) unreached;
                    free(cls->name);
                    if (cls->fields)
                    {
                        for (int i = 0; i < cls->field_count; i++)
                        {
                            free(cls->fields[i]);
                        }
                        free(cls->fields);
                    }
                    bytes_allocated -= sizeof(PithClass);
                    break;
                }
                case OBJ_ENV:
                {
                    Env *env = (Env *) unreached;
                    free(env->name); // NULL for frame nodes; free(NULL) is safe.
                    free(env->slots);
                    bytes_allocated -= sizeof(Env);
                    break;
                }
                case OBJ_INSTANCE:
                {
                    // PithInstance doesn't own extra raw memory, just pointers to other GC objects
                    bytes_allocated -= sizeof(PithInstance);
                    break;
                }
                case OBJ_BOUND_METHOD:
                {
                    bytes_allocated -= sizeof(BoundMethod);
                    break;
                }
                case OBJ_STRUCT_DEF:
                {
                    StructDef *def = (StructDef *) unreached;
                    free(def->name);
                    if (def->fields)
                    {
                        for (int i = 0; i < def->field_count; i++)
                        {
                            free(def->fields[i]);
                        }
                        free(def->fields);
                    }
                    bytes_allocated -= sizeof(StructDef);
                    break;
                }
                case OBJ_STRUCT_INSTANCE:
                {
                    StructInstance *inst = (StructInstance *) unreached;
                    free(inst->field_values);
                    bytes_allocated -= sizeof(StructInstance);
                    break;
                }
                case OBJ_BUFFER:
                {
                    // free() may already have released the bytes (data is then NULL).
                    Buffer *buffer = (Buffer *) unreached;
                    free(buffer->data);
                    bytes_allocated -= sizeof(Buffer);
                    break;
                }
                case OBJ_NATIVE:
                {
                    NativeObject *native = (NativeObject *) unreached;
                    if (native->cls->finalize)
                        native->cls->finalize(native->data);
                    bytes_allocated -= sizeof(NativeObject);
                    break;
                }
                case OBJ_STRING:
                {
                    // Header, bytes and NUL were one allocation (see string_new).
                    ObjString *str = (ObjString *) unreached;
                    bytes_allocated -= sizeof(ObjString) + (size_t) str->length + 1;
                    break;
                }
            }
            free(unreached);
        }
        else
        {
            (*object)->is_marked = 0;
            object = &(*object)->next;
        }
    }
}

/**
 * @brief Performs a full garbage collection cycle.
 *
 * Marks all reachable objects and sweeps (frees) unreachable ones.
 * Adjusts the next GC threshold based on remaining allocated bytes.
 */
void gc_collect()
{
#ifdef DEBUG_TRACE_MEMORY
    printf("[GC] Starting collection cycle. Bytes allocated: %zu\n", bytes_allocated);
#endif

    mark_roots();
    trace_gray_objects();
    sweep();

    next_gc_threshold = bytes_allocated * 2;
    if (next_gc_threshold < 1024 * 1024)
        next_gc_threshold = 1024 * 1024; // Min 1MB

#ifdef DEBUG_TRACE_MEMORY
    printf("[GC] Collection complete. Bytes allocated: %zu. Next threshold: %zu\n", bytes_allocated, next_gc_threshold);
#endif
}

/**
 * @brief Frees all allocated objects.
 *
 * Intended to be called at program exit to clean up memory.
 */
void free_all_objects()
{
    // Reset all marks to 0 to ensure everything is swept
    ObjHeader *curr = objects;
    while (curr)
    {
        curr->is_marked = 0;
        curr = curr->next;
    }

    // Call sweep. Since nothing is marked (and we don't call mark_roots), everything will be freed.
    sweep();
}

/**
 * @brief Prints current GC statistics to stdout.
 */
void print_gc_stats()
{
    printf("GC Stats: %zu bytes allocated, threshold %zu\n", bytes_allocated, next_gc_threshold);
}
