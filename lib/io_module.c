/**
 * @file io_module.c
 * @brief Implementation of the native `io` module (see io_module.h).
 *
 * File system calls differ between POSIX and Windows; the small wrappers in the
 * "Platform layer" section hide that, and everything above them is shared.
 *
 * ## Memory / GC
 *
 * Functions that return lists (list_dir, read_lines) root the list while they
 * allocate one string per entry. Path arguments are rooted by the interpreter
 * for the duration of each call, so their chars stay valid.
 */

// realpath(), strdup() and the directory functions are POSIX, not C99, and
// glibc only declares realpath() with the X/Open extensions enabled. Without a
// declaration C would assume it returns int and truncate the pointer.
#if !defined(_WIN32)
#if !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#if !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700
#endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <io.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

#include "io_module.h"
#include "interpreter.h"
#include "common.h"
#include "gc.h"
#include "pith_string.h"
#include "types.h"

// --- Platform layer ---

#ifdef _WIN32
typedef struct _stat64 FileStat;
#else
typedef struct stat FileStat;
#endif

static int file_stat(const char *path, FileStat *st)
{
#ifdef _WIN32
    return _stat64(path, st);
#else
    return stat(path, st);
#endif
}

static int stat_is_dir(const FileStat *st)
{
#ifdef _WIN32
    return (st->st_mode & _S_IFMT) == _S_IFDIR;
#else
    return S_ISDIR(st->st_mode);
#endif
}

static int stat_is_file(const FileStat *st)
{
#ifdef _WIN32
    return (st->st_mode & _S_IFMT) == _S_IFREG;
#else
    return S_ISREG(st->st_mode);
#endif
}

static int make_one_dir(const char *path)
{
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0777); // Further restricted by the process umask.
#endif
}

static int remove_empty_dir(const char *path)
{
#ifdef _WIN32
    return _rmdir(path);
#else
    return rmdir(path);
#endif
}

static int rename_path(const char *from, const char *to)
{
#ifdef _WIN32
    // C rename() on Windows refuses to replace an existing file; match POSIX.
    if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
        return 0;
    errno = EACCES;
    return -1;
#else
    return rename(from, to);
#endif
}

static int is_separator(char c)
{
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

static int compare_names(const void *a, const void *b)
{
    return strcmp(*(char *const *) a, *(char *const *) b);
}

/**
 * @brief Reads the names in a directory (excluding "." and ".."), sorted.
 *
 * @return A malloc'd array of malloc'd names, or NULL (errno set) on failure.
 */
static char **read_dir_names(const char *path, int *count_out)
{
    int count = 0, capacity = 16;
    char **names = malloc(sizeof(char *) * (size_t) capacity);
#ifdef _WIN32
    char pattern[MAX_PATH + 3];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    WIN32_FIND_DATAA entry;
    HANDLE find = FindFirstFileA(pattern, &entry);
    if (find == INVALID_HANDLE_VALUE)
    {
        free(names);
        errno = ENOENT;
        return NULL;
    }
    do
    {
        const char *name = entry.cFileName;
#else
    DIR *dir = opendir(path);
    if (dir == NULL)
    {
        free(names);
        return NULL;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
    {
        const char *name = entry->d_name;
#endif
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        if (count == capacity)
        {
            capacity *= 2;
            names = realloc(names, sizeof(char *) * (size_t) capacity);
        }
        names[count++] = strdup(name);
#ifdef _WIN32
    }
    while (FindNextFileA(find, &entry));
    FindClose(find);
#else
    }
    closedir(dir);
#endif
    qsort(names, (size_t) count, sizeof(char *), compare_names);
    *count_out = count;
    return names;
}

// --- Helpers ---

/**
 * @brief Validates that every argument is a string and the count is right.
 */
static void require_strings(int arg_count, Value *args, int expected, const char *usage)
{
    int ok = arg_count == expected;
    for (int i = 0; ok && i < arg_count; i++)
        ok = args[i].type == VAL_STRING;
    if (!ok)
        report_error(get_exec_error_line(), "%s", usage);
}

/**
 * @brief Raises "<what>: could not <action> '<path>': <OS reason>." using errno.
 */
static void io_fail(const char *what, const char *action, const char *path)
{
    report_error(get_exec_error_line(), "%s: could not %s '%s': %s.", what, action, path, strerror(errno));
}

static Value make_bool(int b)
{
    Value v;
    v.type = VAL_BOOL;
    v.int_val = b ? 1 : 0;
    return v;
}

static Value make_int(int64_t n)
{
    Value v;
    v.type = VAL_INT;
    v.int_val = n;
    return v;
}

/**
 * @brief Creates an empty list<string>; the caller must root it before filling it.
 */
static Value new_string_list(void)
{
    Value v;
    v.type = VAL_LIST;
    v.list = (List *) allocate_obj(sizeof(List), OBJ_LIST);
    v.list->element_spec = type_from_name("string");
    return v;
}

/**
 * @brief Writes @p length bytes to @p path, opened with @p mode ("wb" or "ab").
 */
static void write_bytes(const char *what, const char *path, const char *mode, const char *bytes, size_t length)
{
    FILE *file = fopen(path, mode);
    if (file == NULL)
        io_fail(what, "open", path);
    size_t written = fwrite(bytes, 1, length, file);
    int closed = fclose(file);
    if (written != length || closed != 0)
        io_fail(what, "write", path);
}

// --- Files ---

// io.read_file(path) -> string (exact bytes)
static Value native_io_read_file(int arg_count, Value *args)
{
    require_strings(arg_count, args, 1, "read_file() takes exactly one string argument (the path).");
    long length;
    char *content = read_file_bytes(AS_CSTRING(args[0]), &length);
    if (content == NULL)
        io_fail("read_file()", "read", AS_CSTRING(args[0]));
    Value v = string_value(content, (int) length); // Binary-safe: NULs included.
    free(content);
    return v;
}

// io.read_lines(path) -> list<string>
static Value native_io_read_lines(int arg_count, Value *args)
{
    require_strings(arg_count, args, 1, "read_lines() takes exactly one string argument (the path).");
    long length;
    char *content = read_file_bytes(AS_CSTRING(args[0]), &length);
    if (content == NULL)
        io_fail("read_lines()", "read", AS_CSTRING(args[0]));

    Value lines = new_string_list();
    gc_push_value(lines); // One string allocation per line below.
    long start = 0;
    while (start < length)
    {
        long end = start;
        while (end < length && content[end] != '\n')
            end++;
        long line_end = end;
        if (line_end > start && content[line_end - 1] == '\r')
            line_end--; // Windows line endings
        list_add(lines.list, string_value(content + start, (int) (line_end - start)));
        start = end + 1; // A final "\n" does not start another (empty) line.
    }
    gc_pop_root();
    free(content);
    return lines;
}

// io.write_file(path, text)
static Value native_io_write_file(int arg_count, Value *args)
{
    require_strings(arg_count, args, 2, "write_file() takes two string arguments (path, content).");
    // "wb": write the exact bytes (no newline translation on Windows).
    write_bytes("write_file()", AS_CSTRING(args[0]), "wb", AS_CSTRING(args[1]), (size_t) STRING_LENGTH(args[1]));
    return (Value){VAL_VOID};
}

// io.append_file(path, text)
static Value native_io_append_file(int arg_count, Value *args)
{
    require_strings(arg_count, args, 2, "append_file() takes two string arguments (path, content).");
    write_bytes("append_file()", AS_CSTRING(args[0]), "ab", AS_CSTRING(args[1]), (size_t) STRING_LENGTH(args[1]));
    return (Value){VAL_VOID};
}

// io.copy_file(from, to)
static Value native_io_copy_file(int arg_count, Value *args)
{
    require_strings(arg_count, args, 2, "copy_file() takes two string arguments (from, to).");
    const char *from = AS_CSTRING(args[0]);
    const char *to = AS_CSTRING(args[1]);
    FILE *in = fopen(from, "rb");
    if (in == NULL)
        io_fail("copy_file()", "open", from);
    FILE *out = fopen(to, "wb");
    if (out == NULL)
    {
        int saved = errno;
        fclose(in);
        errno = saved;
        io_fail("copy_file()", "create", to);
    }
    char buffer[65536];
    size_t n;
    int ok = 1;
    while (ok && (n = fread(buffer, 1, sizeof(buffer), in)) > 0)
        ok = fwrite(buffer, 1, n, out) == n;
    ok = ok && !ferror(in);
    fclose(in);
    if (fclose(out) != 0 || !ok)
        io_fail("copy_file()", "copy to", to);
    return (Value){VAL_VOID};
}

// io.rename(from, to)
static Value native_io_rename(int arg_count, Value *args)
{
    require_strings(arg_count, args, 2, "rename() takes two string arguments (from, to).");
    if (rename_path(AS_CSTRING(args[0]), AS_CSTRING(args[1])) != 0)
        io_fail("rename()", "rename", AS_CSTRING(args[0]));
    return (Value){VAL_VOID};
}

// io.remove(path): a file or an empty directory.
static Value native_io_remove(int arg_count, Value *args)
{
    require_strings(arg_count, args, 1, "remove() takes exactly one string argument (the path).");
    const char *path = AS_CSTRING(args[0]);
    FileStat st;
    if (file_stat(path, &st) != 0)
        io_fail("remove()", "remove", path);
    int failed = stat_is_dir(&st) ? remove_empty_dir(path) : remove(path);
    if (failed)
        io_fail("remove()", "remove", path);
    return (Value){VAL_VOID};
}

// --- Queries ---

static Value native_io_exists(int arg_count, Value *args)
{
    require_strings(arg_count, args, 1, "exists() takes exactly one string argument (the path).");
    FileStat st;
    return make_bool(file_stat(AS_CSTRING(args[0]), &st) == 0);
}

static Value native_io_is_file(int arg_count, Value *args)
{
    require_strings(arg_count, args, 1, "is_file() takes exactly one string argument (the path).");
    FileStat st;
    return make_bool(file_stat(AS_CSTRING(args[0]), &st) == 0 && stat_is_file(&st));
}

static Value native_io_is_dir(int arg_count, Value *args)
{
    require_strings(arg_count, args, 1, "is_dir() takes exactly one string argument (the path).");
    FileStat st;
    return make_bool(file_stat(AS_CSTRING(args[0]), &st) == 0 && stat_is_dir(&st));
}

static Value native_io_file_size(int arg_count, Value *args)
{
    require_strings(arg_count, args, 1, "file_size() takes exactly one string argument (the path).");
    FileStat st;
    if (file_stat(AS_CSTRING(args[0]), &st) != 0)
        io_fail("file_size()", "read the size of", AS_CSTRING(args[0]));
    return make_int((int64_t) st.st_size);
}

static Value native_io_modified_time(int arg_count, Value *args)
{
    require_strings(arg_count, args, 1, "modified_time() takes exactly one string argument (the path).");
    FileStat st;
    if (file_stat(AS_CSTRING(args[0]), &st) != 0)
        io_fail("modified_time()", "read the time of", AS_CSTRING(args[0]));
    return make_int((int64_t) st.st_mtime);
}

// --- Directories ---

// io.list_dir(path) -> list<string> of entry names, sorted.
static Value native_io_list_dir(int arg_count, Value *args)
{
    require_strings(arg_count, args, 1, "list_dir() takes exactly one string argument (the path).");
    int count;
    char **names = read_dir_names(AS_CSTRING(args[0]), &count);
    if (names == NULL)
        io_fail("list_dir()", "list", AS_CSTRING(args[0]));

    Value list = new_string_list();
    gc_push_value(list);
    for (int i = 0; i < count; i++)
    {
        list_add(list.list, cstring_value(names[i]));
        free(names[i]);
    }
    gc_pop_root();
    free(names);
    return list;
}

// io.make_dir(path): creates missing parents; fine if it already exists.
static Value native_io_make_dir(int arg_count, Value *args)
{
    require_strings(arg_count, args, 1, "make_dir() takes exactly one string argument (the path).");
    const char *path = AS_CSTRING(args[0]);
    size_t length = strlen(path);
    char *partial = malloc(length + 1);

    // Create each prefix that ends at a separator, then the full path. Prefixes
    // that are the root ("/") or a drive ("C:") are skipped.
    for (size_t i = 1; i <= length; i++)
    {
        if (i < length && !is_separator(path[i]))
            continue;
        memcpy(partial, path, i);
        partial[i] = '\0';
        if (i == 2 && partial[1] == ':')
            continue;
        FileStat st;
        if (file_stat(partial, &st) == 0)
        {
            if (!stat_is_dir(&st))
            {
                // A file is in the way; name it rather than reporting "File exists".
                char message[1024];
                snprintf(message, sizeof(message), "make_dir(): could not create '%s': '%s' is a file.", path,
                         partial);
                free(partial);
                report_error(get_exec_error_line(), "%s", message);
            }
            continue;
        }
        if (make_one_dir(partial) != 0 && errno != EEXIST)
        {
            int saved = errno;
            free(partial);
            errno = saved;
            io_fail("make_dir()", "create", path);
        }
    }
    free(partial);
    return (Value){VAL_VOID};
}

// io.temp_dir() -> string
static Value native_io_temp_dir(int arg_count, Value *args)
{
    (void) args;
    if (arg_count != 0)
        report_error(get_exec_error_line(), "temp_dir() takes no arguments.");
    char buffer[4096];
#ifdef _WIN32
    DWORD n = GetTempPathA(sizeof(buffer), buffer);
    if (n == 0 || n >= sizeof(buffer))
        snprintf(buffer, sizeof(buffer), ".");
#else
    const char *tmp = getenv("TMPDIR");
    snprintf(buffer, sizeof(buffer), "%s", tmp && tmp[0] ? tmp : "/tmp");
#endif
    size_t len = strlen(buffer);
    while (len > 1 && is_separator(buffer[len - 1]))
        buffer[--len] = '\0'; // No trailing separator, so join_path() works cleanly.
    return cstring_value(buffer);
}

// --- Paths ---

// io.join_path(a, b, ...) -> "a/b/..."
static Value native_io_join_path(int arg_count, Value *args)
{
    if (arg_count < 2)
        report_error(get_exec_error_line(), "join_path() takes two or more string arguments.");
    size_t size = 1;
    for (int i = 0; i < arg_count; i++)
    {
        if (args[i].type != VAL_STRING)
            report_error(get_exec_error_line(), "join_path(): argument %d must be a string.", i + 1);
        size += (size_t) STRING_LENGTH(args[i]) + 1;
    }
    char *out = malloc(size);
    size_t n = 0;
    for (int i = 0; i < arg_count; i++)
    {
        const char *part = AS_CSTRING(args[i]);
        size_t len = (size_t) STRING_LENGTH(args[i]);
        if (i > 0)
        {
            // Exactly one separator between parts.
            while (len > 0 && is_separator(*part))
            {
                part++;
                len--;
            }
            if (n > 0 && !is_separator(out[n - 1]))
                out[n++] = '/';
        }
        memcpy(out + n, part, len);
        n += len;
    }
    Value v = string_value(out, (int) n);
    free(out);
    return v;
}

/**
 * @brief Length of @p path without trailing separators (but keeping a lone root "/").
 */
static int trimmed_length(const char *path, int length)
{
    while (length > 1 && is_separator(path[length - 1]))
        length--;
    return length;
}

// io.basename(path) -> last component
static Value native_io_basename(int arg_count, Value *args)
{
    require_strings(arg_count, args, 1, "basename() takes exactly one string argument (the path).");
    const char *path = AS_CSTRING(args[0]);
    int end = trimmed_length(path, STRING_LENGTH(args[0]));
    int start = end;
    while (start > 0 && !is_separator(path[start - 1]))
        start--;
    return string_value(path + start, end - start);
}

// io.dirname(path) -> everything before the last component
static Value native_io_dirname(int arg_count, Value *args)
{
    require_strings(arg_count, args, 1, "dirname() takes exactly one string argument (the path).");
    const char *path = AS_CSTRING(args[0]);
    int end = trimmed_length(path, STRING_LENGTH(args[0]));
    while (end > 0 && !is_separator(path[end - 1]))
        end--;
    if (end == 0)
        return cstring_value("."); // "file.txt" lives in the current directory.
    end = trimmed_length(path, end);
    return string_value(path, end);
}

// io.extension(path) -> ".txt" (or "")
static Value native_io_extension(int arg_count, Value *args)
{
    require_strings(arg_count, args, 1, "extension() takes exactly one string argument (the path).");
    const char *path = AS_CSTRING(args[0]);
    int end = trimmed_length(path, STRING_LENGTH(args[0]));
    int start = end;
    while (start > 0 && !is_separator(path[start - 1]))
        start--;
    // The last dot of the file name, unless the name starts with it (".bashrc").
    for (int i = end - 1; i > start; i--)
    {
        if (path[i] == '.')
            return string_value(path + i, end - i);
    }
    return cstring_value("");
}

// io.absolute_path(path) -> absolute, normalized path of an existing file/directory
static Value native_io_absolute_path(int arg_count, Value *args)
{
    require_strings(arg_count, args, 1, "absolute_path() takes exactly one string argument (the path).");
    const char *path = AS_CSTRING(args[0]);
    FileStat st;
    if (file_stat(path, &st) != 0)
        io_fail("absolute_path()", "resolve", path);
#ifdef _WIN32
    char *resolved = _fullpath(NULL, path, 0);
#else
    char *resolved = realpath(path, NULL);
#endif
    if (resolved == NULL)
        io_fail("absolute_path()", "resolve", path);
    Value v = cstring_value(resolved);
    free(resolved);
    return v;
}

// --- Registration ---

void io_register_module(HashMap *modules)
{
    HashMap *funcs = hashmap_create(VAL_STRING, VAL_NATIVE_FN);
    register_native_method(funcs, "read_file", native_io_read_file);
    register_native_method(funcs, "read_lines", native_io_read_lines);
    register_native_method(funcs, "write_file", native_io_write_file);
    register_native_method(funcs, "append_file", native_io_append_file);
    register_native_method(funcs, "copy_file", native_io_copy_file);
    register_native_method(funcs, "rename", native_io_rename);
    register_native_method(funcs, "remove", native_io_remove);
    register_native_method(funcs, "exists", native_io_exists);
    register_native_method(funcs, "is_file", native_io_is_file);
    register_native_method(funcs, "is_dir", native_io_is_dir);
    register_native_method(funcs, "file_size", native_io_file_size);
    register_native_method(funcs, "modified_time", native_io_modified_time);
    register_native_method(funcs, "list_dir", native_io_list_dir);
    register_native_method(funcs, "make_dir", native_io_make_dir);
    register_native_method(funcs, "temp_dir", native_io_temp_dir);
    register_native_method(funcs, "join_path", native_io_join_path);
    register_native_method(funcs, "basename", native_io_basename);
    register_native_method(funcs, "dirname", native_io_dirname);
    register_native_method(funcs, "extension", native_io_extension);
    register_native_method(funcs, "absolute_path", native_io_absolute_path);

    Value module;
    module.type = VAL_HASHMAP;
    module.hashmap = funcs;
    hashmap_set(modules, "io", module, 0);
}
