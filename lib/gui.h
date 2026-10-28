/**
 * @file gui.h
 * @brief Optional native `gui` module for Pith, backed by raylib + raygui.
 *
 * The gui module gives Pith programs a small, immediate-mode graphical UI.
 * "Immediate mode" means there are no widgets to create and no callbacks to
 * register: every frame the program simply *calls* the widgets it wants, and
 * each call both draws the widget and returns what the user did with it. This
 * maps cleanly onto Pith's native-function model (a widget is just a function
 * call that returns a value) and avoids having to call Pith closures back from
 * C or teach the garbage collector about long-lived UI objects.
 *
 * A typical Pith program using this module looks like:
 * @code
 *   import "gui"
 *
 *   gui.init(480, 240, "Hello")      # open a window
 *   gui.set_fps(60)
 *   while (!gui.should_close()):      # loop until the user closes the window
 *       gui.begin()                   # start a frame
 *       gui.clear(245, 245, 245)      # background colour (r, g, b)
 *       if (gui.button(20, 20, 120, 32, "Click me")):
 *           print("clicked!")
 *       gui.label(20, 70, 200, 24, "Hello from Pith")
 *       gui.end()                     # finish and present the frame
 *   gui.close()                       # tidy up
 * @endcode
 *
 * This module is compiled and registered only when the interpreter is built
 * with the CMake option `-DPITH_ENABLE_GUI=ON` (which also pulls in raylib).
 * When that option is off, none of this file is used and `import "gui"` fails
 * with "No module named 'gui'".
 */

#ifndef PITH_GUI_H
#define PITH_GUI_H

#include "value.h"

/**
 * @brief Registers the `gui` module and all of its functions.
 *
 * Builds the module's function table and installs it into @p modules under the
 * key "gui". Called once at start-up from register_all_native_modules(), and
 * only when the build enables the GUI (see file header).
 *
 * @param modules The interpreter's native-module registry
 *                (its global `native_module_funcs` hashmap).
 */
void gui_register_module(HashMap *modules);

#endif // PITH_GUI_H
