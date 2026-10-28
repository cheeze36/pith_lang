/**
 * @file native_object.c
 * @brief Implementation of native objects (see native_object.h).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native_object.h"
#include "interpreter.h"
#include "common.h"
#include "gc.h"
#include "types.h"

// Every native class ever created (they live for the whole run), for lookups by name.
static NativeClass **all_classes = NULL;
static int class_count = 0;

NativeClass *native_class_find(const char *name)
{
    for (int i = 0; i < class_count; i++)
    {
        if (strcmp(all_classes[i]->name, name) == 0)
            return all_classes[i];
    }
    return NULL;
}

NativeClass *native_class_create(const char *name, NativeFinalizeFn finalize, NativeMarkFn mark)
{
    NativeClass *cls = calloc(1, sizeof(NativeClass));
    if (cls == NULL)
    {
        fprintf(stderr, "Fatal: Out of memory (native class).\n");
        exit(1);
    }
    cls->name = name;
    cls->finalize = finalize;
    cls->mark = mark;
    cls->methods.type = VAL_HASHMAP;
    cls->methods.hashmap = hashmap_create(VAL_STRING, VAL_NATIVE_FN);
    gc_register_value_root(&cls->methods); // The class (and its method table) lives for the whole run.

    all_classes = realloc(all_classes, sizeof(NativeClass *) * (size_t) (class_count + 1));
    all_classes[class_count++] = cls;
    return cls;
}

void native_class_add_method(NativeClass *cls, const char *name, NativeFn function)
{
    register_native_method(cls->methods.hashmap, name, function);
}

Value native_object_new(NativeClass *cls, void *data)
{
    NativeObject *object = (NativeObject *) allocate_obj(sizeof(NativeObject), OBJ_NATIVE);
    object->cls = cls;
    object->data = data;
    Value v;
    v.type = VAL_NATIVE_OBJECT;
    v.native = object;
    return v;
}

void *native_object_data(Value v, NativeClass *cls, const char *fn_name)
{
    if (v.type != VAL_NATIVE_OBJECT || v.native->cls != cls)
        report_error(get_exec_error_line(), "%s(): expected a %s, got %s.", fn_name, cls->name,
                     type_describe_value(v));
    return v.native->data;
}
