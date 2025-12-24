#include "interpreter.h"
#include "value.h"
#include "parser.h"
#include "debug.h"
#include "common.h"
#include "gc.h" // Include GC
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include <time.h>
#include <ctype.h>

static PithClass *last_defined_class = NULL;

// --- Environment ---
// Env struct definition moved to value.h

Env *global_env = NULL;

// --- Native Registries ---
HashMap* native_string_methods;
HashMap* native_list_methods;
HashMap* native_module_funcs;

// --- Forward Declarations ---
Value eval(ASTNode *node, Env *env);
Value exec(ASTNode *node, Env **env_ptr);
Value exec_block(ASTNode *node, Env **env_ptr);
HashMap* hashmap_create(ValueType key_type, ValueType value_type);
void hashmap_set(HashMap *map, const char *key, Value value, int line_num);
Value hashmap_get(HashMap *map, const char *key);
void interpret(ASTNode *root);
char* read_file_content(const char* filename);
Value value_copy(Value v);
void define_all_natives_in_env(Env **env_ptr);
void register_all_native_methods();
void register_all_native_modules();
ValueType get_type_from_name(const char* type_name);
void exec_module(ASTNode *root, Env **env_ptr);
const char* get_value_type_name(ValueType type);
void default_report_error(int line, const char *format, ...);

// --- Error Handling ---
static error_reporter_t current_error_reporter = default_report_error;

void default_report_error(int line, const char *format, ...) {
    fprintf(stderr, "[line %d] Error: ", line);
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr, "\n");
    exit(1);
}

void set_error_reporter(error_reporter_t reporter) {
    current_error_reporter = reporter;
}

void report_error(int line, const char *format, ...) {
    va_list args;
    va_start(args, format);
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    current_error_reporter(line, "%s", buffer);
}


Value value_copy(Value v) {
    #ifdef DEBUG_TRACE_MEMORY
    printf("[MEMORY] Copying value of type %s\n", get_value_type_name(v.type));
    #endif
    #ifdef DEBUG_TRACE_ADVANCED_MEMORY
    if (v.type == VAL_STRING) printf("[ADV_MEMORY] Copying string at %p\n", (void*)v.str_val);
    #endif

    if (v.type == VAL_STRING) {
        Value new_v;
        new_v.type = VAL_STRING;
        new_v.str_val = strdup(v.str_val);
        return new_v;
    }
    return v;
}

void env_define(Env **env_ptr, const char *name, Value val) {
    #ifdef DEBUG_TRACE_ENVIRONMENT
    printf("[ENV] Defining variable '%s'\n", name);
    #endif
    #ifdef DEBUG_TRACE_ENVIRONMENT_ADV
    printf("[ENV_ADV] Defining '%s' with type %s in env %p\n", name, get_value_type_name(val.type), (void*)*env_ptr);
    #endif
    #ifdef DEBUG_DEEP_DIVE_INTERP
    printf("[DDI_ENV] Defining variable '%s'\n", name);
    #endif

    // Use GC allocator for Env nodes
    Env *new_entry = (Env*)allocate_obj(sizeof(Env), OBJ_ENV);
    new_entry->name = strdup(name);
    new_entry->val = value_copy(val);
    new_entry->next = *env_ptr;
    *env_ptr = new_entry;
}

void env_assign(Env *env, const char *name, Value val, int line) {
    #ifdef DEBUG_TRACE_ENVIRONMENT
    printf("[ENV] Assigning to variable '%s'\n", name);
    #endif
    #ifdef DEBUG_TRACE_ENVIRONMENT_ADV
    printf("[ENV_ADV] Assigning '%s' with type %s in env %p\n", name, get_value_type_name(val.type), (void*)env);
    #endif
    #ifdef DEBUG_DEEP_DIVE_INTERP
    printf("[DDI_ENV] Assigning variable '%s'\n", name);
    #endif

    while (env) {
        if (strcmp(env->name, name) == 0) {
            env->val = value_copy(val);
            return;
        }
        env = env->next;
    }
    Env *g = global_env;
    while (g) {
        if (strcmp(g->name, name) == 0) {
            g->val = value_copy(val);
            return;
        }
        g = g->next;
    }
    report_error(line, "Undefined variable '%s'.", name);
}

Value env_get(Env *env, const char *name, int line) {
    #ifdef DEBUG_TRACE_ENVIRONMENT
    printf("[ENV] Getting variable '%s'\n", name);
    #endif
    #ifdef DEBUG_TRACE_ENVIRONMENT_ADV
    printf("[ENV_ADV] Getting '%s' from env %p\n", name, (void*)env);
    #endif
    #ifdef DEBUG_DEEP_DIVE_INTERP
    printf("[DDI_ENV] Getting variable '%s'\n", name);
    #endif

    while (env) {
        if (strcmp(env->name, name) == 0) return env->val;
        env = env->next;
    }
    Env *g = global_env;
    while (g) {
        if (strcmp(g->name, name) == 0) return g->val;
        g = g->next;
    }
    report_error(line, "Undefined variable '%s'.", name);
    return (Value){VAL_VOID};
}

const char* get_value_type_name(ValueType type) {
    switch (type) {
        case VAL_INT: return "int";
        case VAL_FLOAT: return "float";
        case VAL_STRING: return "string";
        case VAL_BOOL: return "bool";
        case VAL_VOID: return "void";
        case VAL_NATIVE_FN: return "native_function";
        case VAL_FUNC: return "function";
        case VAL_MODULE: return "module";
        case VAL_CLASS: return "class";
        case VAL_INSTANCE: return "instance";
        case VAL_LIST: return "list";
        case VAL_HASHMAP: return "hashmap";
        default: return "unknown";
    }
}

void print_value(Value v) {
    if (v.type == VAL_INT) printf("%d", v.int_val);
    else if (v.type == VAL_FLOAT) printf("%f", v.float_val);
    else if (v.type == VAL_STRING) printf("%s", v.str_val);
    else if (v.type == VAL_BOOL) printf("%s", v.int_val ? "true" : "false");
    else if (v.type == VAL_FUNC) printf("<function %s>", v.func->name);
    else if (v.type == VAL_NATIVE_FN) printf("<native fn>");
    else if (v.type == VAL_VOID) printf("void");
    else if (v.type == VAL_MODULE) printf("<module %s>", v.module->name);
    else if (v.type == VAL_CLASS) printf("<class %s>", v.pith_class->name);
    else if (v.type == VAL_INSTANCE) printf("<instance of %s>", v.instance->pith_class->name);
    else if (v.type == VAL_BOUND_METHOD) printf("<bound method>");
    else if (v.type == VAL_LIST) {
        printf("[");
        for (int i = 0; i < v.list->count; i++) {
            print_value(v.list->items[i]);
            if (i < v.list->count - 1) printf(", ");
        }
        printf("]");
    }
    else if (v.type == VAL_HASHMAP) {
        printf("{");
        int first = 1;
        for (int i = 0; i < v.hashmap->bucket_count; i++) {
            MapEntry* entry = v.hashmap->buckets[i];
            while (entry) {
                if (!first) printf(", ");
                printf("%s: ", entry->key);
                print_value(entry->value);
                first = 0;
                entry = entry->next;
            }
        }
        printf("}");
    }
}

// --- Native Functions & Methods ---
Value native_clock(int arg_count, Value* args) { 
    #ifdef DEBUG_TRACE_NATIVE
    printf("[NATIVE] Calling clock()\n");
    #endif
    #ifdef DEBUG_DEEP_DIVE_INTERP
    printf("[DDI_NATIVE_METHOD] Calling clock()\n");
    #endif
    Value v; v.type = VAL_FLOAT; v.float_val = (float)clock() / CLOCKS_PER_SEC; return v; 
}

Value native_input(int arg_count, Value* args) { 
    #ifdef DEBUG_TRACE_NATIVE
    printf("[NATIVE] Calling input()\n");
    #endif
    #ifdef DEBUG_DEEP_DIVE_INTERP
    printf("[DDI_NATIVE_METHOD] Calling input()\n");
    #endif
    if (arg_count > 0) { for (int i = 0; i < arg_count; i++) print_value(args[i]); } fflush(stdout); char buffer[1024]; fgets(buffer, sizeof(buffer), stdin); buffer[strcspn(buffer, "\n")] = 0; Value v; v.type = VAL_STRING; v.str_val = strdup(buffer); return v; 
}

void list_add(List *list, Value item) { if (list->count >= list->capacity) { if (list->is_fixed) { report_error(0, "Cannot append to a fixed-size list."); return; } list->capacity = list->capacity == 0 ? 4 : list->capacity * 2; list->items = realloc(list->items, list->capacity * sizeof(Value)); } list->items[list->count++] = item; }

Value native_list_append(int arg_count, Value* args) {
    #ifdef DEBUG_TRACE_NATIVE
    printf("[NATIVE] Calling list.append()\n");
    #endif
    #ifdef DEBUG_DEEP_DIVE_INTERP
    printf("[DDI_NATIVE_METHOD] Calling list.append()\n");
    #endif
    if (arg_count != 2) report_error(0, "append() takes exactly one argument.");
    if (args[0].type != VAL_LIST) report_error(0, "append() must be called on a list.");
    list_add(args[0].list, args[1]);
    return (Value){VAL_VOID};
}

Value native_len(int arg_count, Value* args) {
    #ifdef DEBUG_TRACE_NATIVE
    printf("[NATIVE] Calling len()\n");
    #endif
    #ifdef DEBUG_DEEP_DIVE_INTERP
    printf("[DDI_NATIVE_METHOD] Calling len()\n");
    #endif
    if (arg_count != 1) report_error(0, "len() takes no arguments.");
    Value self = args[0];
    Value v;
    v.type = VAL_INT;
    if (self.type == VAL_STRING) { v.int_val = strlen(self.str_val); }
    else if (self.type == VAL_LIST) { v.int_val = self.list->count; }
    else { report_error(0, "len() can only be called on a string or a list."); }
    return v;
}

// --- Math Module Native Functions ---
Value native_math_sqrt(int arg_count, Value* args) {
    if (arg_count != 1) report_error(0, "sqrt() takes exactly one argument.");
    if (args[0].type != VAL_FLOAT && args[0].type != VAL_INT) report_error(0, "sqrt() argument must be a number.");
    double num = (args[0].type == VAL_INT) ? (double)args[0].int_val : args[0].float_val;
    Value v;
    v.type = VAL_FLOAT;
    v.float_val = sqrt(num);
    return v;
}

Value native_math_sin(int arg_count, Value* args) {
    if (arg_count != 1) report_error(0, "sin() takes exactly one argument.");
    if (args[0].type != VAL_FLOAT && args[0].type != VAL_INT) report_error(0, "sin() argument must be a number.");
    double num = (args[0].type == VAL_INT) ? (double)args[0].int_val : args[0].float_val;
    Value v; v.type = VAL_FLOAT; v.float_val = sin(num); return v;
}

Value native_math_cos(int arg_count, Value* args) {
    if (arg_count != 1) report_error(0, "cos() takes exactly one argument.");
    if (args[0].type != VAL_FLOAT && args[0].type != VAL_INT) report_error(0, "cos() argument must be a number.");
    double num = (args[0].type == VAL_INT) ? (double)args[0].int_val : args[0].float_val;
    Value v; v.type = VAL_FLOAT; v.float_val = cos(num); return v;
}

Value native_math_tan(int arg_count, Value* args) {
    if (arg_count != 1) report_error(0, "tan() takes exactly one argument.");
    if (args[0].type != VAL_FLOAT && args[0].type != VAL_INT) report_error(0, "tan() argument must be a number.");
    double num = (args[0].type == VAL_INT) ? (double)args[0].int_val : args[0].float_val;
    Value v; v.type = VAL_FLOAT; v.float_val = tan(num); return v;
}

Value native_math_floor(int arg_count, Value* args) {
    if (arg_count != 1) report_error(0, "floor() takes exactly one argument.");
    if (args[0].type != VAL_FLOAT && args[0].type != VAL_INT) report_error(0, "floor() argument must be a number.");
    double num = (args[0].type == VAL_INT) ? (double)args[0].int_val : args[0].float_val;
    Value v; v.type = VAL_FLOAT; v.float_val = floor(num); return v;
}

Value native_math_ceil(int arg_count, Value* args) {
    if (arg_count != 1) report_error(0, "ceil() takes exactly one argument.");
    if (args[0].type != VAL_FLOAT && args[0].type != VAL_INT) report_error(0, "ceil() argument must be a number.");
    double num = (args[0].type == VAL_INT) ? (double)args[0].int_val : args[0].float_val;
    Value v; v.type = VAL_FLOAT; v.float_val = ceil(num); return v;
}

Value native_math_log(int arg_count, Value* args) {
    if (arg_count != 1) report_error(0, "log() takes exactly one argument.");
    if (args[0].type != VAL_FLOAT && args[0].type != VAL_INT) report_error(0, "log() argument must be a number.");
    double num = (args[0].type == VAL_INT) ? (double)args[0].int_val : args[0].float_val;
    Value v; v.type = VAL_FLOAT; v.float_val = log(num); return v;
}

// --- IO Module Native Functions ---
Value native_io_read_file(int arg_count, Value* args) {
    if (arg_count != 1 || args[0].type != VAL_STRING) {
        report_error(0, "read_file() takes exactly one string argument (the path).");
    }
    char* content = read_file_content(args[0].str_val);
    if (content == NULL) {
        return (Value){VAL_VOID};
    }
    Value v;
    v.type = VAL_STRING;
    v.str_val = content;
    return v;
}

Value native_io_write_file(int arg_count, Value* args) {
    if (arg_count != 2 || args[0].type != VAL_STRING || args[1].type != VAL_STRING) {
        report_error(0, "write_file() takes two string arguments (path, content).");
    }
    FILE* file = fopen(args[0].str_val, "w");
    if (file == NULL) {
        Value v; v.type = VAL_BOOL; v.int_val = 0; return v;
    }
    fprintf(file, "%s", args[1].str_val);
    fclose(file);
    Value v; v.type = VAL_BOOL; v.int_val = 1; return v;
}

// --- Sys Module Native Functions ---
Value native_sys_exit(int arg_count, Value* args) {
    if (arg_count != 1 || args[0].type != VAL_INT) {
        report_error(0, "exit() takes exactly one integer argument (the exit code).");
    }
    exit(args[0].int_val);
    return (Value){VAL_VOID}; // Unreachable
}


Value native_string_trim(int arg_count, Value* args) {
    #ifdef DEBUG_TRACE_NATIVE
    printf("[NATIVE] Calling string.trim()\n");
    #endif
    #ifdef DEBUG_DEEP_DIVE_INTERP
    printf("[DDI_NATIVE_METHOD] Calling string.trim()\n");
    #endif
    if (arg_count != 1) report_error(0, "trim() takes no arguments.");
    if (args[0].type != VAL_STRING) report_error(0, "trim() must be called on a string.");
    
    char* original = args[0].str_val;
    char* start = original;
    while (isspace(*start)) start++;

    char* end = original + strlen(original) - 1;
    while (end > start && isspace(*end)) end--;
    
    char* new_str = malloc(end - start + 2);
    strncpy(new_str, start, end - start + 1);
    new_str[end - start + 1] = '\0';

    Value v;
    v.type = VAL_STRING;
    v.str_val = new_str;
    return v;
}

Value native_string_split(int arg_count, Value* args) {
    #ifdef DEBUG_TRACE_NATIVE
    printf("[NATIVE] Calling string.split()\n");
    #endif
    #ifdef DEBUG_DEEP_DIVE_INTERP
    printf("[DDI_NATIVE_METHOD] Calling string.split()\n");
    #endif
    if (arg_count != 2) report_error(0, "split() takes exactly one argument (the delimiter).");
    if (args[0].type != VAL_STRING || args[1].type != VAL_STRING) report_error(0, "split() requires a string object and a string delimiter.");

    // Use GC allocator for List
    List* list = (List*)allocate_obj(sizeof(List), OBJ_LIST);
    list->count = 0;
    list->capacity = 4;
    list->is_fixed = 0;
    list->items = malloc(list->capacity * sizeof(Value));

    char* str_copy = strdup(args[0].str_val);
    char* delim = args[1].str_val;
    char* token = strtok(str_copy, delim);

    while (token != NULL) {
        Value token_val;
        token_val.type = VAL_STRING;
        token_val.str_val = strdup(token);
        list_add(list, token_val);
        token = strtok(NULL, delim);
    }
    
    free(str_copy);
    Value v;
    v.type = VAL_LIST;
    v.list = list;
    return v;
}

Value native_list_join(int arg_count, Value* args) {
    #ifdef DEBUG_TRACE_NATIVE
    printf("[NATIVE] Calling list.join()\n");
    #endif
    #ifdef DEBUG_DEEP_DIVE_INTERP
    printf("[DDI_NATIVE_METHOD] Calling list.join()\n");
    #endif
    if (arg_count != 2) report_error(0, "join() takes exactly one argument (the delimiter).");
    if (args[0].type != VAL_LIST || args[1].type != VAL_STRING) report_error(0, "join() requires a list object and a string delimiter.");

    List* list = args[0].list;
    char* delim = args[1].str_val;
    
    int total_len = 0;
    for (int i = 0; i < list->count; i++) {
        if (list->items[i].type != VAL_STRING) report_error(0, "join() can only be called on a list of strings.");
        total_len += strlen(list->items[i].str_val);
    }
    total_len += strlen(delim) * (list->count - 1);

    char* result_str = malloc(total_len + 1);
    result_str[0] = '\0';

    for (int i = 0; i < list->count; i++) {
        strcat(result_str, list->items[i].str_val);
        if (i < list->count - 1) {
            strcat(result_str, delim);
        }
    }

    Value v;
    v.type = VAL_STRING;
    v.str_val = result_str;
    return v;
}


void define_native_in_env(Env **env_ptr, const char* name, NativeFn function) {
    Value v; v.type = VAL_NATIVE_FN; v.native_fn = function;
    env_define(env_ptr, name, v);
}

void define_all_natives_in_env(Env **env_ptr) {
    define_native_in_env(env_ptr, "clock", native_clock);
    define_native_in_env(env_ptr, "input", native_input);
}

void register_native_method(HashMap* method_map, const char* name, NativeFn function) {
    Value method_val;
    method_val.type = VAL_NATIVE_FN;
    method_val.native_fn = function;
    hashmap_set(method_map, name, method_val, 0);
}

void register_all_native_methods() {
    native_string_methods = hashmap_create(VAL_STRING, VAL_NATIVE_FN);
    register_native_method(native_string_methods, "len", native_len);
    register_native_method(native_string_methods, "trim", native_string_trim);
    register_native_method(native_string_methods, "split", native_string_split);

    native_list_methods = hashmap_create(VAL_STRING, VAL_NATIVE_FN);
    register_native_method(native_list_methods, "len", native_len);
    register_native_method(native_list_methods, "append", native_list_append);
    register_native_method(native_list_methods, "join", native_list_join);
}

void register_all_native_modules() {
    native_module_funcs = hashmap_create(VAL_STRING, VAL_HASHMAP);
    
    // Math module
    HashMap* math_funcs = hashmap_create(VAL_STRING, VAL_NATIVE_FN);
    register_native_method(math_funcs, "sqrt", native_math_sqrt);
    register_native_method(math_funcs, "sin", native_math_sin);
    register_native_method(math_funcs, "cos", native_math_cos);
    register_native_method(math_funcs, "tan", native_math_tan);
    register_native_method(math_funcs, "floor", native_math_floor);
    register_native_method(math_funcs, "ceil", native_math_ceil);
    register_native_method(math_funcs, "log", native_math_log);
    
    Value math_module_val;
    math_module_val.type = VAL_HASHMAP;
    math_module_val.hashmap = math_funcs;
    hashmap_set(native_module_funcs, "math", math_module_val, 0);

    // IO module
    HashMap* io_funcs = hashmap_create(VAL_STRING, VAL_NATIVE_FN);
    register_native_method(io_funcs, "read_file", native_io_read_file);
    register_native_method(io_funcs, "write_file", native_io_write_file);

    Value io_module_val;
    io_module_val.type = VAL_HASHMAP;
    io_module_val.hashmap = io_funcs;
    hashmap_set(native_module_funcs, "io", io_module_val, 0);

    // Sys module
    HashMap* sys_funcs = hashmap_create(VAL_STRING, VAL_NATIVE_FN);
    register_native_method(sys_funcs, "exit", native_sys_exit);

    Value sys_module_val;
    sys_module_val.type = VAL_HASHMAP;
    sys_module_val.hashmap = sys_funcs;
    hashmap_set(native_module_funcs, "sys", sys_module_val, 0);
}

Value exec_block(ASTNode *node, Env **env_ptr) {
    #ifdef DEBUG_TRACE_EXECUTION
    printf("[EXEC] Entering new block scope.\n");
    #endif
    #ifdef DEBUG_DEEP_DIVE_INTERP
    printf("[DDI_BLOCK] Entering block scope.\n");
    #endif

    Env *original_env = *env_ptr;
    Env *block_env = original_env; 

    for (int i = 0; i < node->children_count; i++) {
        Value result = exec(node->children[i], &block_env);
        if (result.type != VAL_VOID) {
            *env_ptr = original_env; 
            #ifdef DEBUG_TRACE_EXECUTION
            printf("[EXEC] Exiting block scope (with return).\n");
            #endif
            #ifdef DEBUG_DEEP_DIVE_INTERP
            printf("[DDI_BLOCK] Exiting block scope (with return).\n");
            #endif
            return result;
        }
    }

    *env_ptr = original_env; 
    #ifdef DEBUG_TRACE_EXECUTION
    printf("[EXEC] Exiting block scope.\n");
    #endif
    #ifdef DEBUG_DEEP_DIVE_INTERP
    printf("[DDI_BLOCK] Exiting block scope.\n");
    #endif
    return (Value){VAL_VOID};
}

unsigned long hash_string(const char *str) { unsigned long hash = 5381; int c; while ((c = *str++)) hash = ((hash << 5) + hash) + c; return hash; }

HashMap* hashmap_create(ValueType key_type, ValueType value_type) { 
    // Use GC allocator for HashMap
    HashMap *map = (HashMap*)allocate_obj(sizeof(HashMap), OBJ_MAP); 
    map->bucket_count = 16; 
    map->buckets = calloc(map->bucket_count, sizeof(MapEntry*)); 
    map->key_type = key_type;
    map->value_type = value_type;
    return map; 
}

void hashmap_set(HashMap *map, const char *key, Value value, int line_num) { 
    if (map->value_type != VAL_VOID && value.type != map->value_type) {
        report_error(line_num, "Type mismatch: Cannot set value of type '%s' in a hashmap expecting type '%s'.", get_value_type_name(value.type), get_value_type_name(map->value_type));
    }
    
    #ifdef DEBUG_TRACE_MEMORY
    printf("[MEMORY] Set key '%s' in hashmap.\n", key);
    #endif
    #ifdef DEBUG_TRACE_ADVANCED_MEMORY
    printf("[ADV_MEMORY] Setting key '%s' in hashmap at %p\n", key, (void*)map);
    #endif

    unsigned long hash = hash_string(key); 
    int index = hash % map->bucket_count; 
    MapEntry *entry = map->buckets[index]; 
    while (entry) { 
        if (strcmp(entry->key, key) == 0) { 
            entry->value = value; 
            return; 
        } 
        entry = entry->next; 
    } 
    MapEntry *new_entry = malloc(sizeof(MapEntry)); 
    new_entry->key = strdup(key); 
    new_entry->value = value; 
    new_entry->next = map->buckets[index]; 
    map->buckets[index] = new_entry; 
}

Value hashmap_get(HashMap *map, const char *key) { unsigned long hash = hash_string(key); int index = hash % map->bucket_count; MapEntry *entry = map->buckets[index]; while (entry) { if (strcmp(entry->key, key) == 0) return entry->value; entry = entry->next; } return (Value){VAL_VOID}; }

ValueType get_type_from_name(const char* type_name) {
    if (strcmp(type_name, "int") == 0) return VAL_INT;
    if (strcmp(type_name, "string") == 0) return VAL_STRING;
    if (strcmp(type_name, "float") == 0) return VAL_FLOAT;
    if (strcmp(type_name, "bool") == 0) return VAL_BOOL;
    return VAL_VOID; // Default/unknown
}

Value eval(ASTNode *node, Env *env) {
    if (!node) return (Value){VAL_VOID};

    #ifdef DEBUG_TRACE_EXECUTION
    printf("[EXEC] Evaluating expression node type %d\n", node->type);
    #endif
    #ifdef DEBUG_DEEP_DIVE_INTERP
    printf("[DDI_EVAL] Evaluating node type %d\n", node->type);
    #endif

    Value result;

    switch(node->type) {
        case AST_INT_LITERAL: { result.type = VAL_INT; result.int_val = atoi(node->value); break; }
        case AST_FLOAT_LITERAL: { result.type = VAL_FLOAT; result.float_val = atof(node->value); break; }
        case AST_STRING_LITERAL: { result.type = VAL_STRING; result.str_val = strdup(node->value); break; }
        case AST_BOOL_LITERAL: { result.type = VAL_BOOL; result.int_val = (strcmp(node->value, "true") == 0); break; }
        case AST_LIST_LITERAL: {
            // Use GC allocator for List
            List *list = (List*)allocate_obj(sizeof(List), OBJ_LIST);
            list->count = 0; list->capacity = node->children_count;
            list->is_fixed = 0;
            list->items = malloc(list->capacity * sizeof(Value));
            for (int i = 0; i < node->children_count; i++) list_add(list, eval(node->children[i], env));
            result.type = VAL_LIST; result.list = list; break;
        }
        case AST_HASHMAP_LITERAL: {
            HashMap *map = hashmap_create(VAL_STRING, VAL_VOID);
            for (int i = 0; i < node->children_count; i += 2) {
                Value key = eval(node->children[i], env);
                Value val = eval(node->children[i+1], env);
                if (key.type != VAL_STRING) report_error(node->children[i]->line_num, "Hashmap keys must be strings.");
                hashmap_set(map, key.str_val, val, node->line_num);
            }
            result.type = VAL_HASHMAP; result.hashmap = map; break;
        }
        case AST_VAR_REF: result = value_copy(env_get(env, node->value, node->line_num)); break;
        case AST_UNARY_OP: {
            #ifdef DEBUG_DEEP_DIVE_INTERP
            printf("[DDI_UNARY_OP] Unary op '%s'\n", node->value);
            #endif
            Value operand = eval(node->children[0], env);
            if (strcmp(node->value, "-") == 0) {
                if (operand.type == VAL_INT) {
                    result.type = VAL_INT;
                    result.int_val = -operand.int_val;
                } else if (operand.type == VAL_FLOAT) {
                    result.type = VAL_FLOAT;
                    result.float_val = -operand.float_val;
                } else {
                    report_error(node->line_num, "Operand for unary '-' must be a number.");
                }
            } else if (strcmp(node->value, "!") == 0) {
                if (operand.type == VAL_BOOL) {
                    result.type = VAL_BOOL;
                    result.int_val = !operand.int_val;
                } else {
                    report_error(node->line_num, "Operand for '!' must be a boolean.");
                }
            }
            break;
        }
        case AST_NEW_EXPR: {
            ASTNode *call_node = node->children[0];
            Value class_val = eval(call_node->children[0], env);
            if (class_val.type != VAL_CLASS) {
                report_error(node->line_num, "Cannot instantiate non-class type.");
            }
            
            // Use GC allocator for PithInstance
            PithInstance *instance = (PithInstance*)allocate_obj(sizeof(PithInstance), OBJ_INSTANCE);
            instance->pith_class = class_val.pith_class;
            instance->fields = hashmap_create(VAL_STRING, VAL_VOID);
            
            for (int i = 0; i < instance->pith_class->field_count; i++) {
                hashmap_set(instance->fields, instance->pith_class->fields[i], (Value){VAL_VOID}, node->line_num);
            }

            Value instance_val;
            instance_val.type = VAL_INSTANCE;
            instance_val.instance = instance;
            
            Value init_method_val = hashmap_get(instance->pith_class->methods, "init");
            if (init_method_val.type != VAL_VOID) {
                Func* init_method = init_method_val.func;
                int arg_count = call_node->children_count - 1;
                Value* args = malloc(arg_count * sizeof(Value));
                for (int i = 0; i < arg_count; i++) {
                    args[i] = eval(call_node->children[i+1], env);
                }

                Env *arg_env = NULL;
                env_define(&arg_env, "this", instance_val);
                for (int i = 0; i < arg_count; i++) {
                    env_define(&arg_env, init_method->body->args[i], args[i]);
                }

                Env *exec_env = arg_env;
                if (exec_env) {
                    Env* tail = exec_env;
                    while (tail->next != NULL) tail = tail->next;
                    tail->next = init_method->env;
                } else {
                    exec_env = init_method->env;
                }
                
                exec_block(init_method->body->children[0], &exec_env);
                free(args);
            }
            result = instance_val; break;
        }
        case AST_FIELD_ACCESS: {
            Value object = eval(node->children[0], env);
            if (object.type == VAL_INSTANCE) {
                Value field = hashmap_get(object.instance->fields, node->value);
                if (field.type != VAL_VOID) return field;
                
                Value method_val = hashmap_get(object.instance->pith_class->methods, node->value);
                if (method_val.type != VAL_VOID) {
                    // Use GC allocator for BoundMethod
                    BoundMethod *bound = (BoundMethod*)allocate_obj(sizeof(BoundMethod), OBJ_BOUND_METHOD);
                    bound->receiver = object;
                    bound->method = method_val;
                    result.type = VAL_BOUND_METHOD;
                    result.bound_method = bound;
                    return result;
                }
            } 
            else if (object.type == VAL_MODULE) {
                #ifdef DEBUG_TRACE_IMPORT
                printf("[IMPORT] Accessing member '%s' of module '%s'\n", node->value, object.module->name);
                #endif
                #ifdef DEBUG_DEEP_DIVE_INTERP
                printf("[DDI_MODULE_ACCESS] Accessing member '%s' of module '%s'\n", node->value, object.module->name);
                #endif
                return hashmap_get(object.module->members, node->value);
            }
            else if (object.type == VAL_STRING) {
                Value method_val = hashmap_get(native_string_methods, node->value);
                if (method_val.type != VAL_VOID) {
                    // Use GC allocator for BoundMethod
                    BoundMethod *bound = (BoundMethod*)allocate_obj(sizeof(BoundMethod), OBJ_BOUND_METHOD);
                    bound->receiver = object;
                    bound->method = method_val;
                    result.type = VAL_BOUND_METHOD;
                    result.bound_method = bound;
                    return result;
                }
            }
            else if (object.type == VAL_LIST) {
                Value method_val = hashmap_get(native_list_methods, node->value);
                if (method_val.type != VAL_VOID) {
                    // Use GC allocator for BoundMethod
                    BoundMethod *bound = (BoundMethod*)allocate_obj(sizeof(BoundMethod), OBJ_BOUND_METHOD);
                    bound->receiver = object;
                    bound->method = method_val;
                    result.type = VAL_BOUND_METHOD;
                    result.bound_method = bound;
                    return result;
                }
            }
            report_error(node->line_num, "Value of type '%s' has no field or method named '%s'.", get_value_type_name(object.type), node->value);
            break;
        }
        case AST_INDEX_ACCESS: {
            Value collection = eval(node->children[0], env);
            Value index_val = eval(node->children[1], env);
            if (collection.type == VAL_LIST) {
                if (index_val.type != VAL_INT) report_error(node->line_num, "List index must be an integer.");
                int index = index_val.int_val;
                if (index < 0 || index >= collection.list->count) report_error(node->line_num, "Index out of bounds.");
                result = collection.list->items[index]; break;
            } else if (collection.type == VAL_HASHMAP) {
                if (index_val.type != VAL_STRING) report_error(node->line_num, "Hashmap index must be a string.");
                result = hashmap_get(collection.hashmap, index_val.str_val); break;
            }
            report_error(node->line_num, "Not an indexable type.");
            break;
        }
        case AST_BINARY_OP: {
            Value left = eval(node->children[0], env);
            Value right = eval(node->children[1], env);
            
            #ifdef DEBUG_DEEP_DIVE_INTERP
            printf("[DDI_BINOP] %s %s %s\n", get_value_type_name(left.type), node->value, get_value_type_name(right.type));
            #endif

            Value res = {VAL_VOID};

            if (left.type == VAL_INT && right.type == VAL_INT) {
                if (strcmp(node->value, "+") == 0) { res.type = VAL_INT; res.int_val = left.int_val + right.int_val; }
                else if (strcmp(node->value, "-") == 0) { res.type = VAL_INT; res.int_val = left.int_val - right.int_val; }
                else if (strcmp(node->value, "*") == 0) { res.type = VAL_INT; res.int_val = left.int_val * right.int_val; }
                else if (strcmp(node->value, "/") == 0) { res.type = VAL_INT; res.int_val = left.int_val / right.int_val; }
                else if (strcmp(node->value, "%") == 0) { res.type = VAL_INT; res.int_val = left.int_val % right.int_val; }
                else if (strcmp(node->value, "^") == 0) { res.type = VAL_INT; res.int_val = (int)pow(left.int_val, right.int_val); }
                else if (strcmp(node->value, "<") == 0) { res.type = VAL_BOOL; res.int_val = left.int_val < right.int_val; }
                else if (strcmp(node->value, ">") == 0) { res.type = VAL_BOOL; res.int_val = left.int_val > right.int_val; }
                else if (strcmp(node->value, "<=") == 0) { res.type = VAL_BOOL; res.int_val = left.int_val <= right.int_val; }
                else if (strcmp(node->value, ">=") == 0) { res.type = VAL_BOOL; res.int_val = left.int_val >= right.int_val; }
                else if (strcmp(node->value, "==") == 0) { res.type = VAL_BOOL; res.int_val = left.int_val == right.int_val; }
                else if (strcmp(node->value, "!=") == 0) { res.type = VAL_BOOL; res.int_val = left.int_val != right.int_val; }
            } else if ((left.type == VAL_FLOAT || left.type == VAL_INT) && (right.type == VAL_FLOAT || right.type == VAL_INT)) {
                double left_d = (left.type == VAL_INT) ? left.int_val : left.float_val;
                double right_d = (right.type == VAL_INT) ? right.int_val : right.float_val;
                if (strcmp(node->value, "+") == 0) { res.type = VAL_FLOAT; res.float_val = left_d + right_d; }
                else if (strcmp(node->value, "-") == 0) { res.type = VAL_FLOAT; res.float_val = left_d - right_d; }
                else if (strcmp(node->value, "*") == 0) { res.type = VAL_FLOAT; res.float_val = left_d * right_d; }
                else if (strcmp(node->value, "/") == 0) { res.type = VAL_FLOAT; res.float_val = left_d / right_d; }
                else if (strcmp(node->value, "^") == 0) { res.type = VAL_FLOAT; res.float_val = pow(left_d, right_d); }
                else if (strcmp(node->value, "<") == 0) { res.type = VAL_BOOL; res.int_val = left_d < right_d; }
                else if (strcmp(node->value, ">") == 0) { res.type = VAL_BOOL; res.int_val = left_d > right_d; }
                else if (strcmp(node->value, "<=") == 0) { res.type = VAL_BOOL; res.int_val = left_d <= right_d; }
                else if (strcmp(node->value, ">=") == 0) { res.type = VAL_BOOL; res.int_val = left_d >= right_d; }
                else if (strcmp(node->value, "==") == 0) { res.type = VAL_BOOL; res.int_val = left_d == right_d; }
                else if (strcmp(node->value, "!=") == 0) { res.type = VAL_BOOL; res.int_val = left_d != right_d; }
            } else if (left.type == VAL_STRING && right.type == VAL_STRING) {
                if (strcmp(node->value, "+") == 0) {
                    char *new_str = malloc(strlen(left.str_val) + strlen(right.str_val) + 1);
                    strcpy(new_str, left.str_val);
                    strcat(new_str, right.str_val);
                    res.type = VAL_STRING;
                    res.str_val = new_str;
                }
                else if (strcmp(node->value, "==") == 0) { res.type = VAL_BOOL; res.int_val = strcmp(left.str_val, right.str_val) == 0; }
                else if (strcmp(node->value, "!=") == 0) { res.type = VAL_BOOL; res.int_val = strcmp(left.str_val, right.str_val) != 0; }
            } else if (left.type == VAL_BOOL && right.type == VAL_BOOL) {
                if (strcmp(node->value, "and") == 0) { res.type = VAL_BOOL; res.int_val = left.int_val && right.int_val; }
                else if (strcmp(node->value, "or") == 0) { res.type = VAL_BOOL; res.int_val = left.int_val || right.int_val; }
            }
            
            result = res;
            break;
        }
        case AST_FUNC_CALL: {
            Value callee = eval(node->children[0], env);
            
            int arg_offset = (callee.type == VAL_BOUND_METHOD) ? 1 : 0;
            int arg_count = (node->children_count - 1) + arg_offset;
            Value* args = malloc(arg_count * sizeof(Value));

            if (callee.type == VAL_BOUND_METHOD) {
                args[0] = callee.bound_method->receiver;
            }
            for (int i = 1; i < node->children_count; i++) {
                args[i - 1 + arg_offset] = eval(node->children[i], env);
            }

            if (callee.type == VAL_BOUND_METHOD) {
                if (callee.bound_method->method.type == VAL_NATIVE_FN) {
                    #ifdef DEBUG_TRACE_NATIVE
                    printf("[NATIVE] Calling native method.\n");
                    #endif
                    #ifdef DEBUG_DEEP_DIVE_INTERP
                    printf("[DDI_NATIVE_METHOD] Calling native method.\n");
                    #endif
                    result = callee.bound_method->method.native_fn(arg_count, args);
                } else { 
                    Func* method = callee.bound_method->method.func;
                    Env *arg_env = NULL;
                    env_define(&arg_env, "this", callee.bound_method->receiver);
                    for (int i = 0; i < (arg_count - arg_offset); i++) {
                        env_define(&arg_env, method->body->args[i], args[i + arg_offset]);
                    }
                    Env *exec_env = arg_env;
                    if (exec_env) {
                        Env* tail = exec_env;
                        while (tail->next != NULL) tail = tail->next;
                        tail->next = method->env;
                    } else {
                        exec_env = method->env;
                    }
                    result = exec_block(method->body->children[0], &exec_env);
                }
                free(args);
            } else if (callee.type == VAL_FUNC) {
                Func* func = callee.func;
                Env* arg_env = NULL;
                for (int i = 0; i < arg_count; i++) {
                    env_define(&arg_env, func->body->args[i], args[i]);
                }
                Env* exec_env = arg_env;
                if (exec_env) {
                    Env* tail = exec_env;
                    while (tail->next != NULL) tail = tail->next;
                    tail->next = func->env;
                } else {
                    exec_env = func->env;
                }
                result = exec_block(func->body->children[0], &exec_env);
                free(args);
            } else if (callee.type == VAL_NATIVE_FN) {
                result = callee.native_fn(arg_count, args);
                free(args);
            } else {
                report_error(node->line_num, "Expression is not callable.");
            }
            break;
        }
        default:
            result = (Value){VAL_VOID};
            break;
    }

    return result;
}

void exec_module(ASTNode *root, Env **env_ptr) {
    for (int i = 0; i < root->children_count; i++) {
        exec(root->children[i], env_ptr);
    }
}

Value exec(ASTNode *node, Env **env_ptr) {
    if (!node) return (Value){VAL_VOID};

    #ifdef DEBUG_TRACE_EXECUTION
    printf("[EXEC] Executing statement node type %d\n", node->type);
    #endif
    #ifdef DEBUG_DEEP_DIVE_INTERP
    printf("[DDI_EXEC] Executing node type %d\n", node->type);
    #endif

    if (node->type != AST_FUNC_DEF && node->type != AST_CLASS_DEF) {
        last_defined_class = NULL;
    }

    switch(node->type) {
        case AST_CLASS_DEF: {
            // Use GC allocator for PithClass
            PithClass *pith_class = (PithClass*)allocate_obj(sizeof(PithClass), OBJ_CLASS);
            pith_class->name = strdup(node->value);
            pith_class->methods = hashmap_create(VAL_STRING, VAL_FUNC);
            pith_class->field_count = 0;
            
            #ifdef DEBUG_TRACE_FUNCTION_DEFINING
            printf("[FUNC_DEF] Defining class '%s'\n", pith_class->name);
            #endif
            #ifdef DEBUG_DEEP_DIVE_INTERP
            printf("[DDI_CLASS_DEF] Defining class '%s'\n", pith_class->name);
            #endif
            
            Value class_val;
            class_val.type = VAL_CLASS;
            class_val.pith_class = pith_class;
            env_define(env_ptr, pith_class->name, class_val);
            
            last_defined_class = pith_class;
            break;
        }
        case AST_FUNC_DEF: {
            // Use GC allocator for Func
            Func *func = (Func*)allocate_obj(sizeof(Func), OBJ_FUNC);
            func->name = strdup(node->value);
            func->body = node;
            func->env = *env_ptr;
            Value func_val;
            func_val.type = VAL_FUNC;
            func_val.func = func;

            if (last_defined_class) {
                #ifdef DEBUG_TRACE_FUNCTION_DEFINING
                printf("[FUNC_DEF] Attaching method '%s' to class '%s'\n", func->name, last_defined_class->name);
                #endif
                #ifdef DEBUG_DEEP_DIVE_INTERP
                printf("[DDI_CLASS_METHOD] Attaching method '%s' to class '%s'\n", func->name, last_defined_class->name);
                #endif
                hashmap_set(last_defined_class->methods, func->name, func_val, node->line_num);
            } else {
                #ifdef DEBUG_TRACE_FUNCTION_DEFINING
                printf("[FUNC_DEF] Defining global function '%s'\n", func->name);
                #endif
                env_define(env_ptr, func->name, func_val);
            }
            break;
        }
        case AST_PRINT: {
            for (int i = 0; i < node->children_count; i++) {
                Value val = eval(node->children[i], *env_ptr);
                print_value(val);
                if (i < node->children_count - 1) printf(" ");
            }
            printf("\n"); fflush(stdout);
            break;
        }
        case AST_VAR_DECL: {
            if (node->children_count > 0 && node->children[0]->type == AST_ARRAY_SPECIFIER) {
                ASTNode* array_spec = node->children[0];
                if (array_spec->children_count == 0) { 
                     env_define(env_ptr, node->value, (Value){.type = VAL_LIST, .list = NULL});
                } else {
                    Value size_val = eval(array_spec->children[0], *env_ptr);
                    int size = size_val.int_val;
                    
                    #ifdef DEBUG_TRACE_MEMORY
                    printf("[MEMORY] Initializing fixed-size array '%s' of size %d\n", node->value, size);
                    #endif
                    #ifdef DEBUG_DEEP_DIVE_INTERP
                    printf("[DDI_ARRAY_INIT] Initializing fixed-size array '%s' of size %d\n", node->value, size);
                    #endif

                    // Use GC allocator for List
                    List *list = (List*)allocate_obj(sizeof(List), OBJ_LIST);
                    list->count = size;
                    list->capacity = size;
                    list->is_fixed = 1;
                    list->items = calloc(size, sizeof(Value));

                    Value list_val;
                    list_val.type = VAL_LIST;
                    list_val.list = list;
                    env_define(env_ptr, node->value, list_val);
                }
            }
            else if (strncmp(node->type_name, "map<", 4) == 0) {
                char key_type_name[32], value_type_name[32];
                sscanf(node->type_name, "map<%31[^,],%31[^>]>", key_type_name, value_type_name);
                
                ValueType key_type = get_type_from_name(key_type_name);
                ValueType value_type = get_type_from_name(value_type_name);

                Value map_val;
                map_val.type = VAL_HASHMAP;
                map_val.hashmap = hashmap_create(key_type, value_type);
                
                if (node->children_count > 0) {
                    ASTNode* literal = node->children[0];
                    for (int i = 0; i < literal->children_count; i += 2) {
                        Value key = eval(literal->children[i], *env_ptr);
                        Value val = eval(literal->children[i+1], *env_ptr);
                        hashmap_set(map_val.hashmap, key.str_val, val, literal->line_num);
                    }
                }
                env_define(env_ptr, node->value, map_val);
            } else {
                Value val = (node->children_count > 0) ? eval(node->children[0], *env_ptr) : (Value){VAL_VOID};
                env_define(env_ptr, node->value, val);
            }
            break;
        }
        case AST_ASSIGNMENT: {
            ASTNode *target = node->children[0];
            Value val_to_assign = eval(node->children[1], *env_ptr);
            if (target->type == AST_VAR_REF) {
                env_assign(*env_ptr, target->value, val_to_assign, target->line_num);
            } else if (target->type == AST_FIELD_ACCESS) {
                Value object = eval(target->children[0], *env_ptr);
                if (object.type == VAL_INSTANCE) {
                    hashmap_set(object.instance->fields, target->value, val_to_assign, target->line_num);
                } else {
                    report_error(target->line_num, "Cannot assign to a field on a value of type '%s'.", get_value_type_name(object.type));
                }
            } 
            else if (target->type == AST_INDEX_ACCESS) {
                Value collection = eval(target->children[0], *env_ptr);
                Value index_val = eval(target->children[1], *env_ptr);

                #ifdef DEBUG_DEEP_DIVE_INTERP
                printf("[DDI_ASSIGN_INDEX] Assigning to index\n");
                #endif

                if (collection.type == VAL_HASHMAP) {
                    if (index_val.type != VAL_STRING) report_error(target->line_num, "Hashmap index must be a string.");
                    hashmap_set(collection.hashmap, index_val.str_val, val_to_assign, target->line_num);
                } else if (collection.type == VAL_LIST) {
                    if (index_val.type != VAL_INT) report_error(target->line_num, "List or array index must be an integer.");
                    int index = index_val.int_val;
                    if (index < 0 || index >= collection.list->count) report_error(target->line_num, "Index out of bounds.");
                    collection.list->items[index] = val_to_assign;
                } else {
                    report_error(target->line_num, "Index assignment is only supported for lists, arrays, and hashmaps.");
                }
            }
            break;
        }
        case AST_IF: {
            Value cond = eval(node->children[0], *env_ptr);
            if (cond.int_val) {
                return exec_block(node->children[1], env_ptr);
            } else if (node->children_count > 2) {
                ASTNode *else_node = node->children[2];
                if (else_node->type == AST_IF) {
                    return exec(else_node, env_ptr);
                } else {
                    return exec_block(else_node, env_ptr);
                }
            }
            break;
        }
        case AST_WHILE: {
            while (eval(node->children[0], *env_ptr).int_val) {
                Value result = exec_block(node->children[1], env_ptr);
                if (result.type == VAL_BREAK) break;
                if (result.type == VAL_CONTINUE) continue;
                if (result.type != VAL_VOID) return result;
            }
            break;
        }
        case AST_FOREACH: {
            Value collection = eval(node->children[0], *env_ptr);
            if (collection.type != VAL_LIST) {
                report_error(node->line_num, "foreach loop can only iterate over a list or array.");
            }

            List* list = collection.list;
            for (int i = 0; i < list->count; i++) {
                Env* loop_env = *env_ptr;
                env_define(&loop_env, node->value, list->items[i]);
                
                #ifdef DEBUG_TRACE_EXECUTION
                printf("[EXEC] foreach loop iteration %d: defining '%s'\n", i, node->value);
                #endif
                #ifdef DEBUG_DEEP_DIVE_INTERP
                printf("[DDI_FOREACH_LOOP] Iteration %d: defining '%s'\n", i, node->value);
                #endif

                Value result = exec_block(node->children[1], &loop_env);
                if (result.type == VAL_BREAK) break;
                if (result.type == VAL_CONTINUE) continue;
                if (result.type != VAL_VOID) return result;
            }
            break;
        }
        case AST_FOR: {
            Env* for_env = *env_ptr;
            #ifdef DEBUG_TRACE_EXECUTION
            printf("[EXEC] for loop initializer\n");
            #endif
            #ifdef DEBUG_DEEP_DIVE_INTERP
            printf("[DDI_FOR_LOOP] Initializer\n");
            #endif
            exec(node->children[0], &for_env);

            while (1) {
                #ifdef DEBUG_TRACE_EXECUTION
                printf("[EXEC] for loop condition check\n");
                #endif
                #ifdef DEBUG_DEEP_DIVE_INTERP
                printf("[DDI_FOR_LOOP] Condition check\n");
                #endif
                Value condition = eval(node->children[1], for_env);
                if (!condition.int_val) break;

                Value result = exec_block(node->children[3], &for_env);
                if (result.type == VAL_BREAK) break;
                if (result.type == VAL_CONTINUE) {
                    #ifdef DEBUG_TRACE_EXECUTION
                    printf("[EXEC] for loop increment\n");
                    #endif
                    #ifdef DEBUG_DEEP_DIVE_INTERP
                    printf("[DDI_FOR_LOOP] Increment\n");
                    #endif
                    exec(node->children[2], &for_env);
                    continue;
                }
                if (result.type != VAL_VOID) return result;

                #ifdef DEBUG_TRACE_EXECUTION
                printf("[EXEC] for loop increment\n");
                #endif
                #ifdef DEBUG_DEEP_DIVE_INTERP
                printf("[DDI_FOR_LOOP] Increment\n");
                #endif
                exec(node->children[2], &for_env);
            }
            break;
        }
        case AST_DO_WHILE: {
            do {
                Value result = exec_block(node->children[0], env_ptr);
                if (result.type == VAL_BREAK) break;
                if (result.type == VAL_CONTINUE) continue;
                if (result.type != VAL_VOID) return result;
            } while (eval(node->children[1], *env_ptr).int_val);
            break;
        }
        case AST_SWITCH: {
            Value expr_val = eval(node->children[0], *env_ptr);
            int matched = 0;
            
            for (int i = 1; i < node->children_count; i++) {
                ASTNode *case_node = node->children[i];
                if (case_node->type == AST_CASE) {
                    Value case_val = eval(case_node->children[0], *env_ptr);
                    
                    if (matched || (expr_val.type == case_val.type && 
                                   ((expr_val.type == VAL_INT && expr_val.int_val == case_val.int_val) ||
                                    (expr_val.type == VAL_STRING && strcmp(expr_val.str_val, case_val.str_val) == 0)))) {
                        matched = 1;
                        if (case_node->children_count > 1) {
                            Value result = exec_block(case_node->children[1], env_ptr);
                            if (result.type == VAL_BREAK) return (Value){VAL_VOID};
                            if (result.type != VAL_VOID) return result;
                        }
                    }
                } else if (case_node->type == AST_DEFAULT) {
                    if (matched) {
                         if (case_node->children_count > 0) {
                            Value result = exec_block(case_node->children[0], env_ptr);
                            if (result.type == VAL_BREAK) return (Value){VAL_VOID};
                            if (result.type != VAL_VOID) return result;
                        }
                    }
                }
            }
            
            if (!matched) {
                for (int i = 1; i < node->children_count; i++) {
                    if (node->children[i]->type == AST_DEFAULT) {
                        ASTNode *default_node = node->children[i];
                        if (default_node->children_count > 0) {
                             Value result = exec_block(default_node->children[0], env_ptr);
                             if (result.type == VAL_BREAK) return (Value){VAL_VOID};
                             if (result.type != VAL_VOID) return result;
                        }
                    }
                }
            }
            break;
        }
        case AST_BREAK:
            return (Value){VAL_BREAK};
        case AST_CONTINUE:
            return (Value){VAL_CONTINUE};
        case AST_IMPORT: {
            #ifdef DEBUG_TRACE_IMPORT
            printf("[IMPORT] Importing module '%s'\n", node->value);
            #endif
            #ifdef DEBUG_DEEP_DIVE_INTERP
            printf("[DDI_IMPORT] Importing module '%s'\n", node->value);
            #endif

            char module_name[256];
            snprintf(module_name, sizeof(module_name), "stdlib/%s.pith", node->value);
            char* source = read_file_content(module_name);
            if (!source) {
                snprintf(module_name, sizeof(module_name), "%s.pith", node->value);
                source = read_file_content(module_name);
            }

            Env* module_env = NULL;
            Value native_mod_val = hashmap_get(native_module_funcs, node->value);
            if (native_mod_val.type == VAL_HASHMAP) {
                HashMap* funcs = native_mod_val.hashmap;
                for (int i = 0; i < funcs->bucket_count; i++) {
                    for (MapEntry* entry = funcs->buckets[i]; entry != NULL; entry = entry->next) {
                        env_define(&module_env, entry->key, entry->value);
                    }
                }
            }

            if (source) {
                TokenizerState t_state;
                tokenize(source, &t_state);
                ParserState p_state = {&t_state, 0};
                ASTNode* module_ast = parse_program(&p_state);
                exec_module(module_ast, &module_env);
                free(source);
            }

            // Use GC allocator for Module
            Module* module = (Module*)allocate_obj(sizeof(Module), OBJ_MODULE);
            module->name = strdup(node->value);
            module->members = hashmap_create(VAL_STRING, VAL_VOID);

            for (Env* e = module_env; e != NULL; e = e->next) {
                hashmap_set(module->members, e->name, e->val, node->line_num);
            }

            Value module_val;
            module_val.type = VAL_MODULE;
            module_val.module = module;

            env_define(env_ptr, node->value, module_val);
            break;
        }
        case AST_RETURN:
            return eval(node->children[0], *env_ptr);
        default:
            eval(node, *env_ptr);
            break;
    }

    return (Value){VAL_VOID};
}

void interpret(ASTNode *root) {
    if (!root) {
        printf("AST root is NULL!\n");
        return;
    }
    define_all_natives_in_env(&global_env);
    register_all_native_methods();
    register_all_native_modules();

    for (int i = 0; i < root->children_count; i++) {
        exec(root->children[i], &global_env);
    }
}

char* read_file_content(const char* filename) {
    FILE *file = fopen(filename, "rb");
    if (file == NULL) return NULL;
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *buffer = malloc(length + 1);
    if (buffer == NULL) {
        fclose(file); return NULL;
    }
    fread(buffer, 1, length, file);
    buffer[length] = '\0';
    fclose(file);
    return buffer;
}
