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

add_executable(pith_lang main.c tokenizer.c parser.c interpreter.c repl.c gc.c resolver.c)

# The interpreter uses pow()/floor() from the C math library.
target_link_libraries(pith_lang m)
