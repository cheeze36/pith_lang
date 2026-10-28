cmake_minimum_required(VERSION 3.28)
project(pith_lang)

set(CMAKE_C_STANDARD 99)

# Default to an optimized build. CMake otherwise builds with no optimizer
# (-O0), which for a C interpreter is typically 2-4x slower than -O2. Users
# who want a debug build can still pass -DCMAKE_BUILD_TYPE=Debug explicitly.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# -O2 is the sweet spot for this interpreter; -DNDEBUG strips assert() overhead.
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")

# Optional graphical UI module. Off by default so the core interpreter stays
# dependency-free and the test suite builds anywhere. Turn it on with:
#     cmake -DPITH_ENABLE_GUI=ON ..
# When on, raylib is fetched and built automatically (needs network on first
# configure) and the vendored raygui single header in third_party/ is compiled
# into the `gui` module. See lib/gui.h / lib/gui.c.
option(PITH_ENABLE_GUI "Build the raylib/raygui-backed gui module" OFF)

# On Linux, raylib's desktop backend can target either X11 or Wayland:
#   * OFF (default): build the X11 backend. On a Wayland session this still runs
#     transparently through XWayland, and it is the most battle-tested path.
#   * ON: build raylib's native Wayland backend (no X11). Needs Wayland dev
#     packages instead (see docs/GUI.md).
# This only has an effect when PITH_ENABLE_GUI is ON.
option(PITH_GUI_WAYLAND "Build raylib with the native Wayland backend instead of X11" OFF)

# Core interpreter: tokenizer -> parser -> resolver -> tree-walking interpreter.
set(PITH_SOURCES main.c tokenizer.c parser.c interpreter.c repl.c gc.c resolver.c pith_string.c types.c
    native_object.c pith_buffer.c)

# Native library modules (importable with `import "name"`) live in lib/.
list(APPEND PITH_SOURCES lib/io_module.c lib/sys_module.c lib/time_module.c lib/http_module.c lib/json_module.c
    lib/regex_module.c lib/encoding_module.c lib/crypto_module.c lib/sha256.c lib/sqlite_module.c
    lib/template_module.c)
if(PITH_ENABLE_GUI)
    list(APPEND PITH_SOURCES lib/gui.c)
endif()

add_executable(pith_lang ${PITH_SOURCES})

# SQLite, vendored as its official single-file amalgamation (see
# third_party/sqlite/README.md for the version and the reasons for each option).
# Built as its own library so its (third-party) warnings stay out of our build.
add_library(pith_sqlite STATIC third_party/sqlite/sqlite3.c)
target_include_directories(pith_sqlite PUBLIC ${CMAKE_SOURCE_DIR}/third_party/sqlite)
target_compile_definitions(pith_sqlite PRIVATE
    SQLITE_THREADSAFE=0          # the interpreter is single-threaded
    SQLITE_OMIT_LOAD_EXTENSION   # no native extensions loaded from SQL (and no dlopen dependency)
    SQLITE_DQS=0)                # "double quotes" are always identifiers, never strings
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(pith_sqlite PRIVATE -w)
endif()
target_link_libraries(pith_lang pith_sqlite)

# Where `import` finds the standard library's .pith files, so scripts can be run
# from any directory. Overridable at run time with the PITH_STDLIB variable.
target_compile_definitions(pith_lang PRIVATE PITH_STDLIB_DIR="${CMAKE_SOURCE_DIR}/stdlib")

# Calling an undeclared function is a hard error: C would otherwise assume it
# returns int, which silently truncates returned pointers (and crashes later).
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(pith_lang PRIVATE -Werror=implicit-function-declaration)
endif()

# lib/ modules include the core headers, and the core includes their headers
# (to register them), so both directories are on the include path.
target_include_directories(pith_lang PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/lib)

# The interpreter uses pow()/floor() from the C math library.
target_link_libraries(pith_lang m)

# Windows gives the main thread only 1 MB of stack by default; ask for 8 MB, the
# usual Linux/macOS default, so deep recursion behaves the same everywhere (the
# interpreter's stack guard assumes 8 MB on Windows, see interpreter.c).
if(MSVC)
    target_link_options(pith_lang PRIVATE /STACK:8388608)
elseif(WIN32)
    target_link_options(pith_lang PRIVATE -Wl,--stack,8388608)
endif()

# The http module uses sockets; on Windows those live in Winsock (ws2_32).
# The crypto module's secure random numbers come from BCryptGenRandom (bcrypt).
if(WIN32)
    target_link_libraries(pith_lang ws2_32 bcrypt)
endif()

if(PITH_ENABLE_GUI)
    include(FetchContent)

    # raylib provides the window, render loop and drawing. Pin to 5.0 to match
    # the vendored raygui 4.0. Disable raylib's own examples/games to keep the
    # build fast.
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
    # Select the windowing backend and build ONLY that one, so each path needs
    # just its own system packages. raylib bundles GLFW 3.4, whose backends are
    # controlled by GLFW_BUILD_WAYLAND / GLFW_BUILD_X11 (both default ON, which
    # would force us to satisfy both dependency sets). We pin exactly one.
    if(PITH_GUI_WAYLAND)
        set(USE_WAYLAND ON CACHE BOOL "" FORCE)        # raylib's own hint
        set(GLFW_BUILD_WAYLAND ON CACHE BOOL "" FORCE) # GLFW 3.4: Wayland backend
        set(GLFW_BUILD_X11 OFF CACHE BOOL "" FORCE)    # ...and nothing else
    else()
        set(GLFW_BUILD_X11 ON CACHE BOOL "" FORCE)     # GLFW 3.4: X11 backend
        set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
    endif()
    FetchContent_Declare(
        raylib
        GIT_REPOSITORY https://github.com/raysan5/raylib.git
        GIT_TAG 5.0
        GIT_SHALLOW TRUE   # fetch only the 5.0 tag, not full history (much faster)
        GIT_PROGRESS TRUE  # show clone progress instead of appearing to hang
    )
    FetchContent_MakeAvailable(raylib)

    # raygui is a single header; third_party/ holds raygui.h. lib/gui.c defines
    # RAYGUI_IMPLEMENTATION once to emit its bodies.
    target_include_directories(pith_lang PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
    target_link_libraries(pith_lang raylib)
    target_compile_definitions(pith_lang PRIVATE PITH_ENABLE_GUI)

    if(PITH_GUI_WAYLAND)
        message(STATUS "Pith: gui module ENABLED (raylib + raygui, native Wayland backend)")
    else()
        message(STATUS "Pith: gui module ENABLED (raylib + raygui, X11 backend / XWayland)")
    endif()
else()
    message(STATUS "Pith: gui module disabled (configure with -DPITH_ENABLE_GUI=ON to enable)")
endif()
