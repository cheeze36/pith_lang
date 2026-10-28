/**
 * @file sys_module.c
 * @brief Implementation of the native `sys` module (see sys_module.h).
 */

// setenv(), getcwd() and chdir() are POSIX, not C99.
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

#include "sys_module.h"
#include "interpreter.h"
#include "common.h"
#include "gc.h"
#include "pith_string.h"
#include "types.h"

// Set once by main() via sys_set_args().
static const char *script_path = NULL;
static int script_argc = 0;
static char **script_argv = NULL;

void sys_set_args(const char *script, int argc, char **argv)
{
    script_path = script;
    script_argc = argc;
    script_argv = argv;
}

const char *sys_script_path(void)
{
    return script_path;
}

static void require_strings(int arg_count, Value *args, int min, int max, const char *usage)
{
    int ok = arg_count >= min && arg_count <= max;
    for (int i = 0; ok && i < arg_count; i++)
        ok = args[i].type == VAL_STRING;
    if (!ok)
        report_error(get_exec_error_line(), "%s", usage);
}

// sys.args() -> list<string>
static Value native_sys_args(int arg_count, Value *args)
{
    (void) args;
    if (arg_count != 0)
        report_error(get_exec_error_line(), "args() takes no arguments.");
    Value list;
    list.type = VAL_LIST;
    list.list = (List *) allocate_obj(sizeof(List), OBJ_LIST);
    list.list->element_spec = type_from_name("string");
    gc_push_value(list); // One string allocation per argument below.
    for (int i = 0; i < script_argc; i++)
        list_add(list.list, cstring_value(script_argv[i]));
    gc_pop_root();
    return list;
}

// sys.script() -> string
static Value native_sys_script(int arg_count, Value *args)
{
    (void) args;
    if (arg_count != 0)
        report_error(get_exec_error_line(), "script() takes no arguments.");
    return cstring_value(script_path ? script_path : "");
}

// sys.env(name [, default]) -> string
static Value native_sys_env(int arg_count, Value *args)
{
    require_strings(arg_count, args, 1, 2, "env() takes a variable name and an optional default (strings).");
    const char *value = getenv(AS_CSTRING(args[0]));
    if (value != NULL)
        return cstring_value(value);
    if (arg_count == 2)
        return args[1];
    report_error(get_exec_error_line(), "env(): environment variable '%s' is not set.", AS_CSTRING(args[0]));
    return (Value){VAL_VOID};
}

// sys.has_env(name) -> bool
static Value native_sys_has_env(int arg_count, Value *args)
{
    require_strings(arg_count, args, 1, 1, "has_env() takes exactly one string argument (the name).");
    Value v;
    v.type = VAL_BOOL;
    v.int_val = getenv(AS_CSTRING(args[0])) != NULL;
    return v;
}

// sys.set_env(name, value)
static Value native_sys_set_env(int arg_count, Value *args)
{
    require_strings(arg_count, args, 2, 2, "set_env() takes two string arguments (name, value).");
    const char *name = AS_CSTRING(args[0]);
    if (name[0] == '\0' || strchr(name, '=') != NULL)
        report_error(get_exec_error_line(), "set_env(): invalid variable name '%s'.", name);
#ifdef _WIN32
    int failed = _putenv_s(name, AS_CSTRING(args[1])) != 0;
#else
    int failed = setenv(name, AS_CSTRING(args[1]), 1) != 0;
#endif
    if (failed)
        report_error(get_exec_error_line(), "set_env(): could not set '%s': %s.", name, strerror(errno));
    return (Value){VAL_VOID};
}

// sys.cwd() -> string
static Value native_sys_cwd(int arg_count, Value *args)
{
    (void) args;
    if (arg_count != 0)
        report_error(get_exec_error_line(), "cwd() takes no arguments.");
    char buffer[4096];
#ifdef _WIN32
    char *ok = _getcwd(buffer, (int) sizeof(buffer));
#else
    char *ok = getcwd(buffer, sizeof(buffer));
#endif
    if (ok == NULL)
        report_error(get_exec_error_line(), "cwd(): could not read the current directory: %s.", strerror(errno));
    return cstring_value(buffer);
}

// sys.chdir(path)
static Value native_sys_chdir(int arg_count, Value *args)
{
    require_strings(arg_count, args, 1, 1, "chdir() takes exactly one string argument (the path).");
#ifdef _WIN32
    int failed = _chdir(AS_CSTRING(args[0])) != 0;
#else
    int failed = chdir(AS_CSTRING(args[0])) != 0;
#endif
    if (failed)
        report_error(get_exec_error_line(), "chdir(): could not change to '%s': %s.", AS_CSTRING(args[0]),
                     strerror(errno));
    return (Value){VAL_VOID};
}

// sys.platform() -> "linux" | "macos" | "windows" | "unix"
static Value native_sys_platform(int arg_count, Value *args)
{
    (void) args;
    if (arg_count != 0)
        report_error(get_exec_error_line(), "platform() takes no arguments.");
#if defined(_WIN32)
    return cstring_value("windows");
#elif defined(__APPLE__)
    return cstring_value("macos");
#elif defined(__linux__)
    return cstring_value("linux");
#else
    return cstring_value("unix");
#endif
}

// sys.exit(code)
static Value native_sys_exit(int arg_count, Value *args)
{
    if (arg_count != 1 || args[0].type != VAL_INT)
        report_error(get_exec_error_line(), "exit() takes exactly one integer argument (the exit code).");
    fflush(stdout);
    exit((int) args[0].int_val);
    return (Value){VAL_VOID}; // Unreachable
}

void sys_register_module(HashMap *modules)
{
    HashMap *funcs = hashmap_create(VAL_STRING, VAL_NATIVE_FN);
    register_native_method(funcs, "args", native_sys_args);
    register_native_method(funcs, "script", native_sys_script);
    register_native_method(funcs, "env", native_sys_env);
    register_native_method(funcs, "has_env", native_sys_has_env);
    register_native_method(funcs, "set_env", native_sys_set_env);
    register_native_method(funcs, "cwd", native_sys_cwd);
    register_native_method(funcs, "chdir", native_sys_chdir);
    register_native_method(funcs, "platform", native_sys_platform);
    register_native_method(funcs, "exit", native_sys_exit);

    Value module;
    module.type = VAL_HASHMAP;
    module.hashmap = funcs;
    hashmap_set(modules, "sys", module, 0);
}
