/**
 * @file types.c
 * @brief Implementation of declared-type checking (see types.h for the rules).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "types.h"
#include "native_object.h"
#include "pith_buffer.h"
#include "gc.h"
#include "interpreter.h"
#include "common.h"
#include "pith_string.h"

// Interned TypeSpecs, looked up by canonical name. Types are few and live for
// the whole run, so a linked list is plenty.
typedef struct InternedType
{
    TypeSpec spec;
    struct InternedType *next;
} InternedType;

static InternedType *interned_types = NULL;

static TypeSpec *intern(TypeKind kind, TypeSpec *element, const char *class_name, const char *name)
{
    for (InternedType *t = interned_types; t != NULL; t = t->next)
    {
        if (strcmp(t->spec.name, name) == 0)
            return &t->spec;
    }
    InternedType *t = malloc(sizeof(InternedType));
    if (t == NULL)
    {
        fprintf(stderr, "Fatal: Out of memory (type table).\n");
        exit(1);
    }
    t->spec.kind = kind;
    t->spec.element = element;
    t->spec.class_name = class_name ? strdup(class_name) : NULL;
    t->spec.name = strdup(name);
    t->next = interned_types;
    interned_types = t;
    return &t->spec;
}

/**
 * @brief Copies @p text without spaces into @p out (so "map<string, int>" and
 *        "map<string,int>" intern to the same type).
 */
static void strip_spaces(const char *text, char *out, size_t size)
{
    size_t n = 0;
    for (; *text && n + 1 < size; text++)
    {
        if (*text != ' ' && *text != '\t')
            out[n++] = *text;
    }
    out[n] = '\0';
}

TypeSpec *type_from_name(const char *type_name)
{
    if (type_name == NULL)
        return NULL;

    char name[256];
    strip_spaces(type_name, name, sizeof(name));

    if (strcmp(name, "int") == 0)
        return intern(TYPE_INT, NULL, NULL, "int");
    if (strcmp(name, "float") == 0)
        return intern(TYPE_FLOAT, NULL, NULL, "float");
    if (strcmp(name, "string") == 0)
        return intern(TYPE_STRING, NULL, NULL, "string");
    if (strcmp(name, "bool") == 0)
        return intern(TYPE_BOOL, NULL, NULL, "bool");
    if (strcmp(name, "buffer") == 0)
        return intern(TYPE_BUFFER, NULL, NULL, "buffer");
    if (strcmp(name, "void") == 0)
        return intern(TYPE_VOID, NULL, NULL, "void");
    if (strcmp(name, "any") == 0)
        return intern(TYPE_ANY, NULL, NULL, "any");
    if (strcmp(name, "list") == 0)
        return intern(TYPE_LIST, type_from_name("any"), NULL, "list");
    if (strcmp(name, "map") == 0)
        return intern(TYPE_MAP, type_from_name("any"), NULL, "map");

    size_t len = strlen(name);
    if (strncmp(name, "list<", 5) == 0 && name[len - 1] == '>')
    {
        char inner[256];
        snprintf(inner, sizeof(inner), "%.*s", (int) (len - 6), name + 5);
        TypeSpec *element = type_from_name(inner);
        return intern(TYPE_LIST, element, NULL, name);
    }
    if (strncmp(name, "map<", 4) == 0 && name[len - 1] == '>')
    {
        // map<K,V>: keys are always strings; V is the value type.
        const char *comma = strchr(name, ',');
        if (comma == NULL || strncmp(name + 4, "string,", 7) != 0)
            report_error(0, "Unsupported map type '%s': map keys must be strings (map<string, T>).", name);
        char inner[256];
        snprintf(inner, sizeof(inner), "%.*s", (int) (name + len - 1 - (comma + 1)), comma + 1);
        TypeSpec *element = type_from_name(inner);
        return intern(TYPE_MAP, element, NULL, name);
    }

    // Anything else names a class; it is resolved against the value's class
    // chain when checked, so classes may be declared after their first use.
    return intern(TYPE_CLASS, NULL, name, name);
}

const char *type_describe_value(Value v)
{
    static char buffer[300];
    switch (v.type)
    {
        case VAL_LIST:
            if (v.list->element_spec && v.list->element_spec->kind != TYPE_ANY)
            {
                snprintf(buffer, sizeof(buffer), "list<%s>", v.list->element_spec->name);
                return buffer;
            }
            return "list";
        case VAL_HASHMAP:
            if (v.hashmap->value_spec && v.hashmap->value_spec->kind != TYPE_ANY)
            {
                snprintf(buffer, sizeof(buffer), "map<string,%s>", v.hashmap->value_spec->name);
                return buffer;
            }
            return "map";
        case VAL_INSTANCE:
            return v.instance->pith_class->name;
        case VAL_NATIVE_OBJECT:
            return v.native->cls->name;
        default:
            return get_value_type_name(v.type);
    }
}

static int is_instance_of(PithInstance *instance, const char *class_name)
{
    for (PithClass *c = instance->pith_class; c != NULL; c = c->parent)
    {
        if (strcmp(c->name, class_name) == 0)
            return 1;
    }
    return 0;
}

int type_check_value(TypeSpec *type, Value *v)
{
    if (type == NULL)
        return 1;

    switch (type->kind)
    {
        case TYPE_ANY:
            return 1;
        case TYPE_INT:
            return v->type == VAL_INT;
        case TYPE_FLOAT:
            if (v->type == VAL_INT)
            {
                // The one implicit conversion: int widens to float.
                v->type = VAL_FLOAT;
                v->float_val = (double) v->int_val;
            }
            return v->type == VAL_FLOAT;
        case TYPE_STRING:
            return v->type == VAL_STRING;
        case TYPE_BOOL:
            return v->type == VAL_BOOL;
        case TYPE_BUFFER:
            return v->type == VAL_BUFFER;
        case TYPE_VOID:
            return v->type == VAL_VOID;
        case TYPE_CLASS:
            return v->type == VAL_VOID || (v->type == VAL_INSTANCE && is_instance_of(v->instance, type->class_name)) ||
                   (v->type == VAL_NATIVE_OBJECT && strcmp(v->native->cls->name, type->class_name) == 0);
        case TYPE_LIST:
        {
            if (v->type != VAL_LIST)
                return 0;
            List *list = v->list;
            if (type->element->kind == TYPE_ANY || list->element_spec == type->element)
                return 1;
            if (list->element_spec != NULL && list->element_spec->kind != TYPE_ANY)
                return 0; // Already typed differently: containers are never converted.
            for (int i = 0; i < list->count; i++)
            {
                if (!type_check_value(type->element, &list->items[i]))
                    return 0;
            }
            list->element_spec = type->element; // Adopt the declared element type.
            return 1;
        }
        case TYPE_MAP:
        {
            if (v->type != VAL_HASHMAP)
                return 0;
            HashMap *map = v->hashmap;
            if (type->element->kind == TYPE_ANY || map->value_spec == type->element)
                return 1;
            if (map->value_spec != NULL && map->value_spec->kind != TYPE_ANY)
                return 0;
            for (int i = 0; i < map->bucket_count; i++)
            {
                for (MapEntry *entry = map->buckets[i]; entry != NULL; entry = entry->next)
                {
                    if (!type_check_value(type->element, &entry->value))
                        return 0;
                }
            }
            map->value_spec = type->element;
            return 1;
        }
    }
    return 0;
}

void type_require_slow(TypeSpec *type, Value *v, int line, const char *what_format, ...)
{
    if (type_check_value(type, v))
        return;
    char what[300];
    va_list args;
    va_start(args, what_format);
    vsnprintf(what, sizeof(what), what_format, args);
    va_end(args);
    report_error(line, "Type mismatch: %s expects %s, got %s.", what, type->name, type_describe_value(*v));
}

Value type_zero_value(TypeSpec *type)
{
    Value v = {VAL_VOID};
    if (type == NULL)
        return v;
    switch (type->kind)
    {
        case TYPE_INT:
            v.type = VAL_INT;
            v.int_val = 0;
            break;
        case TYPE_FLOAT:
            v.type = VAL_FLOAT;
            v.float_val = 0.0;
            break;
        case TYPE_BOOL:
            v.type = VAL_BOOL;
            v.int_val = 0;
            break;
        case TYPE_STRING:
            v = cstring_value("");
            break;
        case TYPE_BUFFER:
            v.type = VAL_BUFFER;
            v.buffer = buffer_new(0);
            break;
        case TYPE_LIST:
        {
            List *list = (List *) allocate_obj(sizeof(List), OBJ_LIST);
            list->element_spec = type->element;
            v.type = VAL_LIST;
            v.list = list;
            break;
        }
        case TYPE_MAP:
        {
            HashMap *map = hashmap_create(VAL_STRING, VAL_VOID);
            map->value_spec = type->element;
            v.type = VAL_HASHMAP;
            v.hashmap = map;
            break;
        }
        default:
            break; // Class types (and void/any) start as void.
    }
    return v;
}
