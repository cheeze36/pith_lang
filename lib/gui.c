/**
 * @file gui.c
 * @brief Implementation of the optional native `gui` module (raylib + raygui).
 *
 * See gui.h for the high-level design and a usage example. In short: this file
 * wraps a handful of raylib window/frame calls and raygui immediate-mode
 * widgets as Pith native functions, then bundles them into a module named
 * "gui".
 *
 * ## How the pieces fit together
 *
 *  - raylib owns the OS window, the render loop, and low-level drawing.
 *  - raygui is a single-header widget set that draws on top of raylib. It is an
 *    *immediate-mode* library: a widget is drawn and evaluated by one function
 *    call per frame, so there is no widget state for us to allocate or track.
 *  - Each Pith-visible function below is a thin, validated bridge: it checks
 *    the argument count and types, converts Pith `Value`s to C types, calls the
 *    raylib/raygui function, and converts the result back to a `Value`.
 *
 * ## Value conversions at the boundary
 *
 *  - Pith `int`/`float` -> C `int`/`float` (see as_int / as_float).
 *  - Pith `string`      -> C `const char *` (borrowed, read-only).
 *  - Colours are passed as three `int`s (r, g, b) in the range 0..255.
 *  - Booleans come back as Pith `bool`.
 *
 * ## Memory / GC
 *
 * None of these functions allocate Pith heap objects, so the garbage collector
 * needs no special handling here. Strings received from Pith are only read, not
 * stored, so no copying is required either.
 *
 * This file is compiled only when the build defines PITH_ENABLE_GUI (see
 * CMakeLists.txt); otherwise it contributes nothing to the interpreter.
 */

#include <stdbool.h>

#include "gui.h"
#include "interpreter.h" // hashmap_create, register_native_method, get_exec_error_line
#include "common.h"      // report_error
#include "pith_string.h" // AS_CSTRING

// raylib must be included before raygui, and exactly one translation unit must
// define RAYGUI_IMPLEMENTATION to emit raygui's function bodies. This is it.
#include "raylib.h"
#define RAYGUI_IMPLEMENTATION
#include "raygui.h"

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

// Whether gui.init() has successfully opened a window. Used to give friendly
// errors instead of letting raylib draw into a non-existent window.
static bool window_open = false;

// Whether we are currently between gui.begin() and gui.end(). Widget and draw
// calls are only valid inside a frame.
static bool in_frame = false;

// ---------------------------------------------------------------------------
// Small helpers: argument validation and Value construction
//
// These keep each widget function below short and uniform. They all report a
// runtime error (which aborts in file mode / recovers in the REPL) on misuse,
// so callers can assume success once a helper returns.
// ---------------------------------------------------------------------------

/** @brief Error out unless exactly @p want arguments were passed to @p name. */
static void require_arity(const char *name, int got, int want)
{
    if (got != want)
    {
        report_error(get_exec_error_line(),
                     "gui.%s() takes exactly %d argument(s), but %d were given.",
                     name, want, got);
    }
}

/** @brief Read argument @p index as a C int (accepts Pith int or float). */
static int as_int(const char *name, Value *args, int index)
{
    Value v = args[index];
    if (v.type == VAL_INT)
        return (int) v.int_val; // raylib takes 32-bit coordinates/sizes
    if (v.type == VAL_FLOAT)
        return (int) v.float_val;
    report_error(get_exec_error_line(),
                 "gui.%s(): argument %d must be a number.", name, index + 1);
    return 0; // Unreachable; keeps the compiler happy.
}

/** @brief Read argument @p index as a C float (accepts Pith int or float). */
static float as_float(const char *name, Value *args, int index)
{
    Value v = args[index];
    if (v.type == VAL_FLOAT)
        return (float) v.float_val; // raylib uses 32-bit floats
    if (v.type == VAL_INT)
        return (float) v.int_val;
    report_error(get_exec_error_line(),
                 "gui.%s(): argument %d must be a number.", name, index + 1);
    return 0.0f; // Unreachable.
}

/** @brief Read argument @p index as a borrowed C string (requires Pith string). */
static const char *as_string(const char *name, Value *args, int index)
{
    Value v = args[index];
    if (v.type != VAL_STRING)
    {
        report_error(get_exec_error_line(),
                     "gui.%s(): argument %d must be a string.", name, index + 1);
    }
    return AS_CSTRING(v);
}

/** @brief Build a Pith colour from three 0..255 channels (alpha fixed at 255). */
static Color as_color(const char *name, Value *args, int r_index)
{
    int r = as_int(name, args, r_index);
    int g = as_int(name, args, r_index + 1);
    int b = as_int(name, args, r_index + 2);
    // Clamp so out-of-range Pith values can't produce surprising colours.
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return (Color){(unsigned char) r, (unsigned char) g, (unsigned char) b, 255};
}

/** @brief Error out if called before gui.init() has opened a window. */
static void require_window(const char *name)
{
    if (!window_open)
        report_error(get_exec_error_line(),
                     "gui.%s() called before gui.init() opened a window.", name);
}

/** @brief Error out if called outside a gui.begin()/gui.end() frame. */
static void require_frame(const char *name)
{
    require_window(name);
    if (!in_frame)
        report_error(get_exec_error_line(),
                     "gui.%s() must be called between gui.begin() and gui.end().", name);
}

static Value make_void(void) { Value v; v.type = VAL_VOID; return v; }
static Value make_bool(int b) { Value v; v.type = VAL_BOOL; v.int_val = b ? 1 : 0; return v; }
static Value make_float(float f) { Value v; v.type = VAL_FLOAT; v.float_val = f; return v; }

// ---------------------------------------------------------------------------
// Window and frame lifecycle
// ---------------------------------------------------------------------------

/**
 * gui.init(width: int, height: int, title: string) -> void
 * Opens the application window. Must be called before any other gui function.
 */
static Value native_gui_init(int arg_count, Value *args)
{
    require_arity("init", arg_count, 3);
    int width = as_int("init", args, 0);
    int height = as_int("init", args, 1);
    const char *title = as_string("init", args, 2);

    if (window_open)
        report_error(get_exec_error_line(), "gui.init(): a window is already open.");

    InitWindow(width, height, title);
    window_open = true;
    return make_void();
}

/**
 * gui.set_fps(fps: int) -> void
 * Sets the target frames-per-second for the render loop.
 */
static Value native_gui_set_fps(int arg_count, Value *args)
{
    require_arity("set_fps", arg_count, 1);
    require_window("set_fps");
    SetTargetFPS(as_int("set_fps", args, 0));
    return make_void();
}

/**
 * gui.should_close() -> bool
 * Returns true when the user has asked to close the window (close button or
 * the Escape key). Use it as the condition of the main loop.
 */
static Value native_gui_should_close(int arg_count, Value *args)
{
    require_arity("should_close", arg_count, 0);
    require_window("should_close");
    return make_bool(WindowShouldClose());
}

/**
 * gui.begin() -> void
 * Starts a new frame. Call widgets/drawing after this, then gui.end().
 */
static Value native_gui_begin(int arg_count, Value *args)
{
    require_arity("begin", arg_count, 0);
    require_window("begin");
    if (in_frame)
        report_error(get_exec_error_line(), "gui.begin() called twice without gui.end().");
    BeginDrawing();
    in_frame = true;
    return make_void();
}

/**
 * gui.end() -> void
 * Finishes the current frame and presents it to the screen.
 */
static Value native_gui_end(int arg_count, Value *args)
{
    require_arity("end", arg_count, 0);
    if (!in_frame)
        report_error(get_exec_error_line(), "gui.end() called without a matching gui.begin().");
    EndDrawing();
    in_frame = false;
    return make_void();
}

/**
 * gui.clear(r: int, g: int, b: int) -> void
 * Fills the frame with a background colour. Call right after gui.begin().
 */
static Value native_gui_clear(int arg_count, Value *args)
{
    require_arity("clear", arg_count, 3);
    require_frame("clear");
    ClearBackground(as_color("clear", args, 0));
    return make_void();
}

/**
 * gui.close() -> void
 * Closes the window and releases its resources.
 */
static Value native_gui_close(int arg_count, Value *args)
{
    require_arity("close", arg_count, 0);
    if (window_open)
    {
        CloseWindow();
        window_open = false;
        in_frame = false;
    }
    return make_void();
}

// ---------------------------------------------------------------------------
// Widgets (immediate mode: called every frame, return value is the event)
// ---------------------------------------------------------------------------

/**
 * gui.button(x: int, y: int, w: int, h: int, text: string) -> bool
 * Draws a button and returns true on the frame it is clicked.
 */
static Value native_gui_button(int arg_count, Value *args)
{
    require_arity("button", arg_count, 5);
    require_frame("button");
    Rectangle bounds = {as_float("button", args, 0), as_float("button", args, 1),
                        as_float("button", args, 2), as_float("button", args, 3)};
    const char *text = as_string("button", args, 4);
    return make_bool(GuiButton(bounds, text));
}

/**
 * gui.label(x: int, y: int, w: int, h: int, text: string) -> void
 * Draws a non-interactive text label.
 */
static Value native_gui_label(int arg_count, Value *args)
{
    require_arity("label", arg_count, 5);
    require_frame("label");
    Rectangle bounds = {as_float("label", args, 0), as_float("label", args, 1),
                        as_float("label", args, 2), as_float("label", args, 3)};
    const char *text = as_string("label", args, 4);
    GuiLabel(bounds, text);
    return make_void();
}

/**
 * gui.checkbox(x: int, y: int, size: int, text: string, checked: bool) -> bool
 * Draws a checkbox and returns its (possibly toggled) state.
 *
 * Immediate-mode idiom: pass in the current state, use the returned state as
 * the new value, e.g. `checked = gui.checkbox(x, y, 20, "On", checked)`.
 */
static Value native_gui_checkbox(int arg_count, Value *args)
{
    require_arity("checkbox", arg_count, 5);
    require_frame("checkbox");
    float size = as_float("checkbox", args, 2);
    Rectangle bounds = {as_float("checkbox", args, 0), as_float("checkbox", args, 1), size, size};
    const char *text = as_string("checkbox", args, 3);

    if (args[4].type != VAL_BOOL)
        report_error(get_exec_error_line(), "gui.checkbox(): argument 5 must be a bool.");
    bool checked = args[4].int_val != 0;

    GuiCheckBox(bounds, text, &checked); // Toggles `checked` in place on click.
    return make_bool(checked);
}

/**
 * gui.slider(x: int, y: int, w: int, h: int, value: float, min: float, max: float) -> float
 * Draws a slider and returns its (possibly dragged) value.
 *
 * Immediate-mode idiom: pass in the current value, use the return as the new
 * value, e.g. `vol = gui.slider(x, y, 120, 16, vol, 0.0, 100.0)`.
 */
static Value native_gui_slider(int arg_count, Value *args)
{
    require_arity("slider", arg_count, 7);
    require_frame("slider");
    Rectangle bounds = {as_float("slider", args, 0), as_float("slider", args, 1),
                        as_float("slider", args, 2), as_float("slider", args, 3)};
    float value = as_float("slider", args, 4);
    float min = as_float("slider", args, 5);
    float max = as_float("slider", args, 6);

    GuiSlider(bounds, NULL, NULL, &value, min, max); // Updates `value` in place.
    return make_float(value);
}

/**
 * gui.text(x: int, y: int, text: string, size: int, r: int, g: int, b: int) -> void
 * Draws free-form text at a pixel position in the given size and colour.
 */
static Value native_gui_text(int arg_count, Value *args)
{
    require_arity("text", arg_count, 7);
    require_frame("text");
    int x = as_int("text", args, 0);
    int y = as_int("text", args, 1);
    const char *text = as_string("text", args, 2);
    int size = as_int("text", args, 3);
    Color color = as_color("text", args, 4);
    DrawText(text, x, y, size, color);
    return make_void();
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void gui_register_module(HashMap *modules)
{
    HashMap *gui_funcs = hashmap_create(VAL_STRING, VAL_NATIVE_FN);

    // Lifecycle
    register_native_method(gui_funcs, "init", native_gui_init);
    register_native_method(gui_funcs, "set_fps", native_gui_set_fps);
    register_native_method(gui_funcs, "should_close", native_gui_should_close);
    register_native_method(gui_funcs, "begin", native_gui_begin);
    register_native_method(gui_funcs, "end", native_gui_end);
    register_native_method(gui_funcs, "clear", native_gui_clear);
    register_native_method(gui_funcs, "close", native_gui_close);

    // Widgets
    register_native_method(gui_funcs, "button", native_gui_button);
    register_native_method(gui_funcs, "label", native_gui_label);
    register_native_method(gui_funcs, "checkbox", native_gui_checkbox);
    register_native_method(gui_funcs, "slider", native_gui_slider);
    register_native_method(gui_funcs, "text", native_gui_text);

    Value gui_module_val;
    gui_module_val.type = VAL_HASHMAP;
    gui_module_val.hashmap = gui_funcs;
    hashmap_set(modules, "gui", gui_module_val, 0);
}
