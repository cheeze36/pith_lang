/**
 * @file io_module.h
 * @brief Native `io` module: files, directories and paths.
 *
 * @code
 *   import "io"
 *
 *   io.make_dir("data/logs")                       # creates parents too
 *   io.write_file("data/notes.txt", "first line\n")
 *   io.append_file("data/notes.txt", "second line\n")
 *   foreach (string line in io.read_lines("data/notes.txt")):
 *       print(line)
 *
 *   foreach (string name in io.list_dir("data")):
 *       string path = io.join_path("data", name)
 *       if (io.is_file(path)):
 *           print(name + " " + str(io.file_size(path)) + " bytes")
 * @endcode
 *
 * ## Files
 *
 *  - `read_file(path)`           -> string. The file's exact bytes (binary-safe).
 *  - `read_lines(path)`          -> list<string>. Lines without their "\n" / "\r\n".
 *  - `write_file(path, text)`    Create or replace a file.
 *  - `append_file(path, text)`   Append to a file, creating it if needed.
 *  - `copy_file(from, to)`       Copy a file (replacing `to`).
 *  - `rename(from, to)`          Move/rename a file or directory (replacing a file at `to`).
 *  - `remove(path)`              Delete a file or an *empty* directory.
 *
 * ## Queries
 *
 *  - `exists(path)`, `is_file(path)`, `is_dir(path)` -> bool
 *  - `file_size(path)`           -> int, in bytes.
 *  - `modified_time(path)`       -> int, Unix seconds (compare with time.now()).
 *
 * ## Directories
 *
 *  - `list_dir(path)`            -> list<string>. Entry names (not paths), sorted,
 *                                   without "." and "..".
 *  - `make_dir(path)`            Create a directory and any missing parents; fine
 *                                   if it already exists.
 *  - `temp_dir()`                -> string. The system's directory for temporary files.
 *
 * ## Paths (pure string operations; the file system is not touched)
 *
 *  - `join_path(a, b, ...)`      -> "a/b/...". Joins two or more parts with "/"
 *                                   (which Windows accepts too).
 *  - `basename(path)`            -> "c.txt" for "a/b/c.txt".
 *  - `dirname(path)`             -> "a/b" for "a/b/c.txt" ("." if there is none).
 *  - `extension(path)`           -> ".txt" for "a/b/c.txt" ("" if there is none).
 *  - `absolute_path(path)`       -> the absolute, normalized path (the path must exist).
 *
 * Every operation that fails (missing file, permission denied, ...) raises a
 * rescuable error that includes the operating system's reason.
 */

#ifndef PITH_IO_MODULE_H
#define PITH_IO_MODULE_H

#include "value.h"

/**
 * @brief Registers the `io` module into @p modules.
 *
 * @param modules The interpreter's native-module registry.
 */
void io_register_module(HashMap *modules);

#endif // PITH_IO_MODULE_H
