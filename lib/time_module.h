/**
 * @file time_module.h
 * @brief Native `time` module for Pith: wall-clock time, timers and sleeping.
 *
 * A typical Pith program using this module looks like:
 * @code
 *   import "time"
 *
 *   float start = time.perf()
 *   time.sleep(0.5)                       # pause for half a second
 *   time.sleep_ms(250)                    # ...or a quarter second, in ms
 *   print(time.perf() - start)            # ~0.75
 *
 *   print(time.now())                     # Unix timestamp, e.g. 1792000000
 *   print(time.format("%Y-%m-%d %H:%M:%S"))  # local time, strftime-style
 * @endcode
 *
 * ## Functions
 *
 *  - `time.sleep(seconds)`   Pause for an int or float number of seconds.
 *  - `time.sleep_ms(ms)`     Pause for an int number of milliseconds.
 *  - `time.now()`            Current Unix time in whole seconds (int).
 *  - `time.millis()`         Milliseconds since the program started (int, monotonic).
 *  - `time.perf()`           Seconds since the program started (float, monotonic);
 *                            intended for measuring short durations.
 *  - `time.format(fmt)`      Format the current local time with strftime().
 *  - `time.format(fmt, t)`   Format the Unix timestamp @c t as local time.
 *
 * ## Timers
 *
 * `perf()` and `millis()` count from interpreter start-up using a monotonic
 * clock, so they are unaffected by changes to the system time. Pith ints are
 * 64-bit, so `now()` is fine past 2038 and `millis()` does not wrap.
 *
 * The file is named time_module.* rather than time.* so it never shadows the
 * system <time.h> header.
 */

#ifndef PITH_TIME_MODULE_H
#define PITH_TIME_MODULE_H

#include "value.h"

/**
 * @brief Registers the `time` module and all of its functions.
 *
 * Builds the module's function table and installs it into @p modules under the
 * key "time". Also records the start instant that `perf()` and `millis()`
 * measure from. Called once at start-up from register_all_native_modules().
 *
 * @param modules The interpreter's native-module registry
 *                (its global `native_module_funcs` hashmap).
 */
void time_register_module(HashMap *modules);

/**
 * @brief Reads a monotonic clock that never jumps when the system time changes.
 *
 * Also used by other native modules (e.g. http timeouts).
 *
 * @return Seconds since an arbitrary, fixed point in the past.
 */
double time_monotonic_seconds(void);

#endif // PITH_TIME_MODULE_H
