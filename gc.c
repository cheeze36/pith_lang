#include "gc.h"
#include "interpreter.h"
#include <stdlib.h>
#include <stdio.h>

ObjHeader* objects = NULL;

void mark_object(ObjHeader* obj);
void mark_value(Value v);

void* allocate_obj(size_t size, ObjType type) {
    ObjHeader* obj = malloc(size);
    if (obj == NULL) {
        fprintf(stderr, "Fatal: Out of memory.\n");
        exit(1);
    }
    obj->type = type;
    obj->is_marked = 0;
    obj->next = objects;
    objects = obj;
    
    #ifdef DEBUG_TRACE_MEMORY
    printf("[GC] Allocated object %p of type %d\n", (void*)obj, type);
    #endif
    
    return obj;
}

void mark_value(Value v) {
    if (v.type == VAL_LIST) mark_object((ObjHeader*)v.list);
    else if (v.type == VAL_HASHMAP) mark_object((ObjHeader*)v.hashmap);
    else if (v.type == VAL_FUNC) mark_object((ObjHeader*)v.func);
    else if (v.type == VAL_MODULE) mark_object((ObjHeader*)v.module);
    else if (v.type == VAL_CLASS) mark_object((ObjHeader*)v.pith_class);
    else if (v.type == VAL_INSTANCE) mark_object((ObjHeader*)v.instance);
    else if (v.type == VAL_BOUND_METHOD) mark_object((ObjHeader*)v.bound_method);
    else if (v.type == VAL_STRUCT_DEF) mark_object((ObjHeader*)v.struct_def);
    else if (v.type == VAL_STRUCT_INSTANCE) mark_object((ObjHeader*)v.struct_instance);
}

void mark_object(ObjHeader* obj) {
    if (obj == NULL || obj->is_marked) return;
    
    #ifdef DEBUG_TRACE_MEMORY
    printf("[GC] Marking object %p of type %d\n", (void*)obj, obj->type);
    #endif

    obj->is_marked = 1;

    switch (obj->type) {
        case OBJ_LIST: {
            List* list = (List*)obj;
            for (int i = 0; i < list->count; i++) {
                mark_value(list->items[i]);
            }
            break;
        }
        case OBJ_MAP: {
            HashMap* map = (HashMap*)obj;
            for (int i = 0; i < map->bucket_count; i++) {
                MapEntry* entry = map->buckets[i];
                while (entry) {
                    mark_value(entry->value);
                    entry = entry->next;
                }
            }
            break;
        }
        case OBJ_FUNC: {
            Func* func = (Func*)obj;
            mark_object((ObjHeader*)func->env);
            break;
        }
        case OBJ_MODULE: {
            Module* module = (Module*)obj;
            mark_object((ObjHeader*)module->members);
            break;
        }
        case OBJ_CLASS: {
            PithClass* cls = (PithClass*)obj;
            mark_object((ObjHeader*)cls->methods);
            break;
        }
        case OBJ_INSTANCE: {
            PithInstance* inst = (PithInstance*)obj;
            mark_object((ObjHeader*)inst->pith_class);
            mark_object((ObjHeader*)inst->fields);
            break;
        }
        case OBJ_BOUND_METHOD: {
            BoundMethod* bm = (BoundMethod*)obj;
            mark_value(bm->receiver);
            mark_value(bm->method);
            break;
        }
        case OBJ_ENV: {
            Env* env = (Env*)obj;
            mark_value(env->val);
            mark_object((ObjHeader*)env->next);
            break;
        }
        case OBJ_STRUCT_DEF:
            // No child objects to mark
            break;
        case OBJ_STRUCT_INSTANCE: {
            StructInstance* inst = (StructInstance*)obj;
            mark_object((ObjHeader*)inst->def);
            for (int i = 0; i < inst->def->field_count; i++) {
                mark_value(inst->field_values[i]);
            }
            break;
        }
    }
}

void mark_roots() {
    // Mark the global environment
    mark_object((ObjHeader*)global_env);
    
    // Mark native registries
    mark_object((ObjHeader*)native_string_methods);
    mark_object((ObjHeader*)native_list_methods);
    mark_object((ObjHeader*)native_module_funcs);
}

void sweep() {
    ObjHeader** object = &objects;
    while (*object) {
        if (!(*object)->is_marked) {
            ObjHeader* unreached = *object;
            *object = unreached->next;
            
            #ifdef DEBUG_TRACE_MEMORY
            printf("[GC] Freeing object %p of type %d\n", (void*)unreached, unreached->type);
            #endif

            // Free specific resources
            switch (unreached->type) {
                case OBJ_LIST: {
                    List* list = (List*)unreached;
                    free(list->items);
                    break;
                }
                case OBJ_MAP: {
                    HashMap* map = (HashMap*)unreached;
                    for (int i = 0; i < map->bucket_count; i++) {
                        MapEntry* entry = map->buckets[i];
                        while (entry) {
                            MapEntry* next = entry->next;
                            free(entry->key);
                            free(entry);
                            entry = next;
                        }
                    }
                    free(map->buckets);
                    break;
                }
                case OBJ_FUNC: {
                    Func* func = (Func*)unreached;
                    free(func->name);
                    // Body is AST, not freed here
                    break;
                }
                case OBJ_MODULE: {
                    Module* mod = (Module*)unreached;
                    free(mod->name);
                    break;
                }
                case OBJ_CLASS: {
                    PithClass* cls = (PithClass*)unreached;
                    free(cls->name);
                    // fields array?
                    break;
                }
                case OBJ_ENV: {
                    Env* env = (Env*)unreached;
                    free(env->name);
                    if (env->val.type == VAL_STRING) {
                        // Strings are currently malloc'd char* owned by Value
                        // If we free the Env, we should free the string value it holds
                        free(env->val.str_val);
                    }
                    break;
                }
                // ... other types
            }
            free(unreached);
        } else {
            (*object)->is_marked = 0;
            object = &(*object)->next;
        }
    }
}

void gc_collect() {
    #ifdef DEBUG_TRACE_MEMORY
    printf("[GC] Starting collection cycle.\n");
    #endif
    
    mark_roots();
    sweep();
    
    #ifdef DEBUG_TRACE_MEMORY
    printf("[GC] Collection complete.\n");
    #endif
}

void free_all_objects() {
    // Reset all marks to 0 to ensure everything is swept
    ObjHeader* curr = objects;
    while (curr) { 
        curr->is_marked = 0; 
        curr = curr->next; 
    }
    
    // Call sweep. Since nothing is marked (and we don't call mark_roots), everything will be freed.
    sweep();
}
