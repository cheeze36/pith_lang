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
#include <stdlib.h>
#include <stdio.h>

// Linked list of all allocated objects
ObjHeader *objects = NULL;

// --- Temporary Root Stack ---
// Used to protect objects from GC while they are being constructed or used on the C stack.
#define MAX_TEMP_ROOTS 256
ObjHeader *temp_roots[MAX_TEMP_ROOTS];
int temp_root_count = 0;

// --- Allocation Tracking ---
size_t bytes_allocated = 0;
size_t next_gc_threshold = 1024 * 1024; // Start at 1MB

// Forward declarations
void mark_object(ObjHeader *obj);

void mark_value(Value v);

/**
 * @brief Pushes an object onto the temporary root stack.
 *
 * This protects the object from being collected during a GC cycle that might occur
 * before the object is linked into the main object graph (e.g., during complex object creation).
 *
 * @param obj The object to protect.
 */
void gc_push_root(ObjHeader *obj)
{
    if (temp_root_count >= MAX_TEMP_ROOTS)
    {
        fprintf(stderr, "Fatal: GC temp root stack overflow.\n");
        exit(1);
    }
    temp_roots[temp_root_count++] = obj;
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
    // Check if we need to collect
    if (bytes_allocated > next_gc_threshold)
    {
        gc_collect();
    }

    ObjHeader *obj = malloc(size);
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
    if (v.type == VAL_LIST)
        mark_object((ObjHeader *) v.list);
    else if (v.type == VAL_HASHMAP)
        mark_object((ObjHeader *) v.hashmap);
    else if (v.type == VAL_FUNC)
        mark_object((ObjHeader *) v.func);
    else if (v.type == VAL_MODULE)
        mark_object((ObjHeader *) v.module);
    else if (v.type == VAL_CLASS)
        mark_object((ObjHeader *) v.pith_class);
    else if (v.type == VAL_INSTANCE)
        mark_object((ObjHeader *) v.instance);
    else if (v.type == VAL_BOUND_METHOD)
        mark_object((ObjHeader *) v.bound_method);
    else if (v.type == VAL_STRUCT_DEF)
        mark_object((ObjHeader *) v.struct_def);
    else
        if (v.type == VAL_STRUCT_INSTANCE)
            mark_object((ObjHeader *) v.struct_instance);
}

/**
 * @brief Recursively marks an object and its references as reachable.
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
            break;
        }
        case OBJ_STRUCT_DEF:
            // No child objects to mark
            break;
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
    mark_object((ObjHeader *) native_module_funcs);

    // Mark temporary roots (from C stack)
    for (int i = 0; i < temp_root_count; i++)
    {
        mark_object(temp_roots[i]);
    }
}

/**
 * @brief Frees content owned by a Value (e.g., strings).
 *
 * Note: This only frees the raw C-string if it's a VAL_STRING.
 * Other object types are managed by the GC object list.
 *
 * @param v The value to free content for.
 */
void free_value_content(Value v)
{
    if (v.type == VAL_STRING && v.str_val != NULL)
    {
#ifdef DEBUG_TRACE_ADVANCED_MEMORY
        printf("[ADV_MEMORY] Freeing string content '%s' at %p\n", v.str_val, (void *) v.str_val);
#endif
        free(v.str_val);
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
                    for (int i = 0; i < list->count; i++)
                    {
                        free_value_content(list->items[i]);
                    }
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
                            free_value_content(entry->value);
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
                    free(env->name);
                    free_value_content(env->val);
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
                    BoundMethod *bm = (BoundMethod *) unreached;
                    free_value_content(bm->receiver);
                    free_value_content(bm->method);
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
                    for (int i = 0; i < inst->def->field_count; i++)
                    {
                        free_value_content(inst->field_values[i]);
                    }
                    free(inst->field_values);
                    bytes_allocated -= sizeof(StructInstance);
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
