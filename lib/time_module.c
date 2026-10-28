/**
 * @file time_module.c
 * @brief Implementation of the native `time` module.
 *
 * See time_module.h for the Pith-facing API and a usage example. Sleeping and
 * the monotonic clock are OS-specific, so those two pieces have a Win32 and a
 * POSIX implementation; everything else uses only standard C <time.h>.
 *
 * None of these functions allocate Pith heap objects except time.format(),
 * which makes a single string allocation, so no GC root handling is needed.
 */

// clock_gettime()/nanosleep() are POSIX, not C99; request them explicitly so
// the file builds under a strict -std=c99 as well as the default gnu99.
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <errno.h>
#endif

#include "time_module.h"
#include "interpreter.h"
#include "common.h"
#include "pith_string.h"

// Monotonic instant recorded at module registration; perf()/millis() are
// measured from here (see time_module.h for why they are relative).
static double start_seconds;

// --- Platform helpers ---

/**
 * @brief Reads a monotonic clock that never jumps when the system time changes.
 *
 * @return Seconds since an arbitrary, fixed point in the past.
 */
double time_monotonic_seconds(void)
{
#ifdef _WIN32
    static LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    if (frequency.QuadPart == 0)
        QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return (double) counter.QuadPart / (double) frequency.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
#endif
}

/**
 * @brief Blocks the interpreter for the given number of seconds.
 *
 * @param seconds Non-negative duration; fractional seconds are honoured.
 */
static void sleep_seconds(double seconds)
{
#ifdef _WIN32
    // Sleep() takes a DWORD of milliseconds; loop so huge values can't overflow it.
    double remaining_ms = seconds * 1000.0;
    while (remaining_ms > 0)
    {
        DWORD chunk = remaining_ms > 1e9 ? (DWORD) 1e9 : (DWORD) (remaining_ms + 0.5);
        Sleep(chunk);
        remaining_ms -= chunk;
        if (chunk == 0)
            break;
    }
#else
    struct timespec request;
    request.tv_sec = (time_t) seconds;
    request.tv_nsec = (long) ((seconds - (double) request.tv_sec) * 1e9);
    // nanosleep() returns early if a signal arrives; resume with what is left.
    while (nanosleep(&request, &request) == -1 && errno == EINTR)
    {
    }
#endif
}

/**
 * @brief Converts an int or float Value to a double, reporting an error otherwise.
 */
static double number_arg(Value v, const char *func_name)
{
    if (v.type == VAL_INT)
        return (double) v.int_val;
    if (v.type == VAL_FLOAT)
        return v.float_val;
    report_error(get_exec_error_line(), "%s() argument must be a number.", func_name);
    return 0; // Unreachable
}

// --- Native functions ---

// time.sleep(seconds) -> void. Accepts int or float seconds, e.g. sleep(0.25).
static Value native_time_sleep(int arg_count, Value *args)
{
    if (arg_count != 1)
        report_error(get_exec_error_line(), "sleep() takes exactly one argument (seconds).");
    double seconds = number_arg(args[0], "sleep");
    if (seconds < 0)
        report_error(get_exec_error_line(), "sleep() duration must not be negative.");
    sleep_seconds(seconds);
    return (Value){VAL_VOID};
}

// time.sleep_ms(ms) -> void.
static Value native_time_sleep_ms(int arg_count, Value *args)
{
    if (arg_count != 1 || args[0].type != VAL_INT)
        report_error(get_exec_error_line(), "sleep_ms() takes exactly one integer argument (milliseconds).");
    if (args[0].int_val < 0)
        report_error(get_exec_error_line(), "sleep_ms() duration must not be negative.");
    sleep_seconds(args[0].int_val / 1000.0);
    return (Value){VAL_VOID};
}

// time.now() -> int. Current Unix time in whole seconds.
static Value native_time_now(int arg_count, Value *args)
{
    if (arg_count != 0)
        report_error(get_exec_error_line(), "now() takes no arguments.");
    Value v;
    v.type = VAL_INT;
    v.int_val = (int64_t) time(NULL);
    return v;
}

// time.millis() -> int. Milliseconds since the program started (monotonic).
static Value native_time_millis(int arg_count, Value *args)
{
    if (arg_count != 0)
        report_error(get_exec_error_line(), "millis() takes no arguments.");
    Value v;
    v.type = VAL_INT;
    v.int_val = (int64_t) ((time_monotonic_seconds() - start_seconds) * 1000.0);
    return v;
}

// time.perf() -> float. Seconds since the program started (monotonic); use the
// difference of two calls to time a piece of code.
static Value native_time_perf(int arg_count, Value *args)
{
    if (arg_count != 0)
        report_error(get_exec_error_line(), "perf() takes no arguments.");
    Value v;
    v.type = VAL_FLOAT;
    v.float_val = time_monotonic_seconds() - start_seconds;
    return v;
}

// time.format(fmt [, timestamp]) -> string. Formats local time with strftime();
// without a timestamp, formats the current time.
static Value native_time_format(int arg_count, Value *args)
{
    if (arg_count < 1 || arg_count > 2 || args[0].type != VAL_STRING)
        report_error(get_exec_error_line(), "format() takes a format string and an optional integer timestamp.");
    if (arg_count == 2 && args[1].type != VAL_INT)
        report_error(get_exec_error_line(), "format() timestamp must be an integer.");

    time_t t = (arg_count == 2) ? (time_t) args[1].int_val : time(NULL);
    struct tm *local = localtime(&t);
    if (local == NULL)
        report_error(get_exec_error_line(), "format(): timestamp is out of range.");

    // strftime() returns 0 if the result does not fit; an empty string is the
    // most useful thing to hand back in that (rare) case.
    char buffer[256];
    size_t length = strftime(buffer, sizeof(buffer), AS_CSTRING(args[0]), local);
    return string_value(buffer, (int) length);
}

// --- Registration ---

void time_register_module(HashMap *modules)
{
    start_seconds = time_monotonic_seconds();

    HashMap *time_funcs = hashmap_create(VAL_STRING, VAL_NATIVE_FN);
    register_native_method(time_funcs, "sleep", native_time_sleep);
    register_native_method(time_funcs, "sleep_ms", native_time_sleep_ms);
    register_native_method(time_funcs, "now", native_time_now);
    register_native_method(time_funcs, "millis", native_time_millis);
    register_native_method(time_funcs, "perf", native_time_perf);
    register_native_method(time_funcs, "format", native_time_format);

    Value time_module_val;
    time_module_val.type = VAL_HASHMAP;
    time_module_val.hashmap = time_funcs;
    hashmap_set(modules, "time", time_module_val, 0);
}
