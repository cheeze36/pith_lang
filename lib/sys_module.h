/**
 * @file sys_module.h
 * @brief Native `sys` module: the running process and its environment.
 *
 * @code
 *   import "sys"
 *
 *   # pith_lang server.pith 8080 --verbose
 *   list<string> args = sys.args()           # ["8080", "--verbose"]
 *   int port = int(sys.env("PORT", "8080"))  # environment variable with a default
 *   print("running on " + sys.platform() + " in " + sys.cwd())
 *   if (args.len() == 0):
 *       sys.exit(1)
 * @endcode
 *
 * ## Functions
 *
 *  - `args()`                  -> list<string>. Command-line arguments given after
 *                                 the script name.
 *  - `script()`                -> string. The path of the script being run ("" in the REPL).
 *  - `env(name)`               -> string. An environment variable; raises an error if
 *                                 it is not set.
 *  - `env(name, default)`      -> string. The variable, or @c default if it is not set.
 *  - `has_env(name)`           -> bool.
 *  - `set_env(name, value)`    Set an environment variable for this process (and
 *                                 any it starts).
 *  - `cwd()`                   -> string. The current working directory.
 *  - `chdir(path)`             Change the current working directory.
 *  - `platform()`              -> "linux", "macos", "windows", or "unix".
 *  - `exit(code)`              End the program with an exit code.
 *
 * Note: `import` finds the standard library independently of the current
 * directory, and a script's own modules next to the script; only modules found
 * via the current directory (the last place searched) are affected by chdir().
 */

#ifndef PITH_SYS_MODULE_H
#define PITH_SYS_MODULE_H

#include "value.h"

/**
 * @brief Records the script path and its command-line arguments for sys.args().
 *
 * Called by main() before the script runs.
 *
 * @param script The script path (NULL in the REPL).
 * @param argc Number of arguments after the script path.
 * @param argv Those arguments (must stay valid for the whole run).
 */
void sys_set_args(const char *script, int argc, char **argv);

/**
 * @brief The path of the script being run, as given on the command line.
 *
 * @return The path, or NULL in the REPL (no script).
 */
const char *sys_script_path(void);

/**
 * @brief Registers the `sys` module into @p modules.
 *
 * @param modules The interpreter's native-module registry.
 */
void sys_register_module(HashMap *modules);

#endif // PITH_SYS_MODULE_H
