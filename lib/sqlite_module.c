/**
 * @file sqlite_module.c
 * @brief Implementation of the native `sqlite` module (see sqlite_module.h).
 *
 * ## Structure
 *
 *  - A Database is a native object wrapping an `sqlite3 *` connection.
 *  - execute() / query() prepare exactly one statement, bind the parameters,
 *    step through it, and finalize it. Every error path finalizes the statement
 *    *before* raising the Pith error: report_error() does not return, and an
 *    unfinalized statement would keep the database locked.
 *  - close() closes the connection but keeps the Database object, marked as
 *    closed, so any later use is reported instead of touching a freed handle
 *    (the same contract as buffer.free()).
 *
 * ## Memory / GC
 *
 * A Database holds no Pith values, so its native class needs no mark hook; its
 * finalizer closes the connection if the program never called close(). Result
 * rows are built into a list that is rooted while it is filled; each row map is
 * added to the list before its columns are read, so it is reachable throughout.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sqlite3.h"

#include "sqlite_module.h"
#include "interpreter.h"
#include "common.h"
#include "gc.h"
#include "native_object.h"
#include "pith_buffer.h"
#include "pith_string.h"
#include "types.h"

/** How long to wait for another process's write lock before failing, in milliseconds. */
#define BUSY_TIMEOUT_MS 5000

typedef struct
{
    sqlite3 *handle; // NULL once closed
    int is_closed;
} DatabaseData;

static NativeClass *database_class;

// --- Errors ---

/**
 * @brief Finalizes @p statement (if any), then raises a Pith error.
 *
 * Always use this instead of report_error() while a statement is open.
 */
static void fail(sqlite3_stmt *statement, const char *format, ...)
{
    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (statement != NULL)
        sqlite3_finalize(statement);
    report_error(get_exec_error_line(), "%s", message);
}

// --- Receiver and arguments ---

/**
 * @brief The Database a method was called on, checked to still be open.
 */
static DatabaseData *open_database(Value receiver, const char *method)
{
    DatabaseData *database = (DatabaseData *) native_object_data(receiver, database_class, method);
    if (database->is_closed)
        report_error(get_exec_error_line(), "This database was closed with close(); it can no longer be used.");
    return database;
}

/**
 * @brief Checks a method's argument count: sql, plus optionally parameters.
 *
 * @return 1 if parameters were given.
 */
static int sql_and_optional_parameters(int arg_count, Value *args, const char *method)
{
    if (arg_count < 2 || arg_count > 3)
        report_error(get_exec_error_line(), "%s() takes the SQL (string) and optional parameters (list or map).",
                     method);
    if (args[1].type != VAL_STRING)
        report_error(get_exec_error_line(), "%s(): the SQL must be a string, got %s.", method,
                     type_describe_value(args[1]));
    if (arg_count == 3 && args[2].type != VAL_LIST && args[2].type != VAL_HASHMAP)
        report_error(get_exec_error_line(),
                     "%s(): parameters must be a list (for ? placeholders) or a map (for :name placeholders), got %s.",
                     method, type_describe_value(args[2]));
    return arg_count == 3;
}

// --- Preparing and binding ---

/**
 * @brief Prepares exactly one SQL statement.
 *
 * Several statements in one call are refused (use execute_script()), so a
 * single call can never run more than the one statement its author wrote.
 */
static sqlite3_stmt *prepare_one_statement(DatabaseData *database, Value sql, const char *method)
{
    sqlite3_stmt *statement = NULL;
    const char *rest = NULL;
    if (sqlite3_prepare_v2(database->handle, AS_CSTRING(sql), STRING_LENGTH(sql) + 1, &statement, &rest) !=
        SQLITE_OK)
        fail(statement, "%s(): %s", method, sqlite3_errmsg(database->handle));
    if (statement == NULL)
        fail(NULL, "%s(): the SQL contains no statement.", method);

    // Anything after the first statement must be only whitespace and comments:
    // preparing it then yields no statement.
    sqlite3_stmt *second = NULL;
    if (rest != NULL && *rest != '\0' &&
        sqlite3_prepare_v2(database->handle, rest, -1, &second, NULL) == SQLITE_OK && second != NULL)
    {
        sqlite3_finalize(second);
        fail(statement, "%s(): the SQL contains more than one statement; use execute_script() for several.",
             method);
    }
    return statement;
}

/**
 * @brief Binds one Pith value to placeholder number @p index (1-based).
 */
static void bind_value(sqlite3_stmt *statement, int index, Value value, const char *method, const char *where)
{
    int result;
    switch (value.type)
    {
        case VAL_INT:
            result = sqlite3_bind_int64(statement, index, value.int_val);
            break;
        case VAL_FLOAT:
            result = sqlite3_bind_double(statement, index, value.float_val);
            break;
        case VAL_BOOL:
            result = sqlite3_bind_int64(statement, index, value.int_val ? 1 : 0);
            break;
        case VAL_STRING:
            // SQLITE_TRANSIENT: SQLite copies the bytes, so the Pith string may be collected later.
            result = sqlite3_bind_text(statement, index, AS_CSTRING(value), STRING_LENGTH(value), SQLITE_TRANSIENT);
            break;
        case VAL_BUFFER:
            if (value.buffer->is_freed)
                fail(statement, "%s(): %s is a buffer that was freed.", method, where);
            // An empty buffer has no data pointer; bind a zero-length blob explicitly
            // (binding a NULL pointer would store SQL NULL instead).
            result = value.buffer->length == 0
                         ? sqlite3_bind_zeroblob(statement, index, 0)
                         : sqlite3_bind_blob(statement, index, value.buffer->data, value.buffer->length,
                                             SQLITE_TRANSIENT);
            break;
        case VAL_VOID:
            result = sqlite3_bind_null(statement, index);
            break;
        default:
            fail(statement, "%s(): %s is a %s, which cannot be stored (use int, float, string, buffer, bool or void).",
                 method, where, type_describe_value(value));
            return;
    }
    if (result != SQLITE_OK)
        fail(statement, "%s(): could not bind %s: %s", method, where, sqlite3_errstr(result));
}

/**
 * @brief Binds a list (positional) or map (named) of parameters, checking that
 *        the values and placeholders correspond exactly.
 */
static void bind_parameters(sqlite3_stmt *statement, int has_parameters, Value parameters, const char *method)
{
    int placeholder_count = sqlite3_bind_parameter_count(statement);
    if (!has_parameters)
    {
        if (placeholder_count > 0)
            fail(statement, "%s(): the SQL has %d placeholder%s but no values were given.", method,
                 placeholder_count, placeholder_count == 1 ? "" : "s");
        return;
    }

    if (parameters.type == VAL_LIST)
    {
        List *values = parameters.list;
        if (values->count != placeholder_count)
            fail(statement, "%s(): the SQL has %d placeholder%s but %d value%s given.", method, placeholder_count,
                 placeholder_count == 1 ? "" : "s", values->count, values->count == 1 ? " was" : "s were");
        for (int i = 0; i < placeholder_count; i++)
        {
            char where[64];
            snprintf(where, sizeof(where), "parameter %d", i + 1);
            bind_value(statement, i + 1, values->items[i], method, where);
        }
        return;
    }

    // Map: every placeholder must be named and have a value...
    HashMap *values = parameters.hashmap;
    for (int i = 1; i <= placeholder_count; i++)
    {
        const char *placeholder = sqlite3_bind_parameter_name(statement, i);
        if (placeholder == NULL)
            fail(statement, "%s(): the SQL uses ? placeholders; pass the values as a list, not a map.", method);
        const char *key = placeholder + 1; // Skip the ':', '@' or '$'.
        if (!hashmap_has(values, key))
            fail(statement, "%s(): no value given for placeholder %s.", method, placeholder);
        char where[300];
        snprintf(where, sizeof(where), "the value for %s", placeholder);
        bind_value(statement, i, hashmap_get(values, key), method, where);
    }
    // ...and every value must belong to a placeholder (catches misspelled keys).
    MapEntry **entries = hashmap_entries_in_order(values);
    for (int i = 0; i < values->count; i++)
    {
        static const char PREFIXES[] = ":@$";
        int used = 0;
        for (int p = 0; p < 3 && !used; p++)
        {
            char placeholder[300];
            snprintf(placeholder, sizeof(placeholder), "%c%s", PREFIXES[p], entries[i]->key);
            used = sqlite3_bind_parameter_index(statement, placeholder) > 0;
        }
        if (!used)
        {
            char key[256];
            snprintf(key, sizeof(key), "%s", entries[i]->key);
            free(entries);
            fail(statement, "%s(): the SQL has no placeholder :%s for the value given under that name.", method, key);
        }
    }
    free(entries);
}

/**
 * @brief Converts the value in result column @p column of the current row.
 */
static Value column_value(sqlite3_stmt *statement, int column)
{
    Value value = {VAL_VOID};
    switch (sqlite3_column_type(statement, column))
    {
        case SQLITE_INTEGER:
            value.type = VAL_INT;
            value.int_val = sqlite3_column_int64(statement, column);
            break;
        case SQLITE_FLOAT:
            value.type = VAL_FLOAT;
            value.float_val = sqlite3_column_double(statement, column);
            break;
        case SQLITE_TEXT:
            value = string_value((const char *) sqlite3_column_text(statement, column),
                                 sqlite3_column_bytes(statement, column));
            break;
        case SQLITE_BLOB:
        {
            // Read the length after the pointer, as SQLite documents.
            const void *bytes = sqlite3_column_blob(statement, column);
            value = string_value((const char *) bytes, sqlite3_column_bytes(statement, column));
            break;
        }
        default: // SQLITE_NULL
            break;
    }
    return value;
}

// --- Module function ---

// sqlite.open(path) -> Database
static Value native_sqlite_open(int arg_count, Value *args)
{
    if (arg_count != 1 || args[0].type != VAL_STRING)
        report_error(get_exec_error_line(), "sqlite.open() takes exactly one string argument (the file path, or \":memory:\").");
    sqlite3 *handle = NULL;
    int result = sqlite3_open_v2(AS_CSTRING(args[0]), &handle, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (result != SQLITE_OK)
    {
        char message[512];
        snprintf(message, sizeof(message), "%s", handle ? sqlite3_errmsg(handle) : sqlite3_errstr(result));
        sqlite3_close(handle);
        report_error(get_exec_error_line(), "sqlite.open(): could not open '%s': %s", AS_CSTRING(args[0]), message);
    }
    sqlite3_busy_timeout(handle, BUSY_TIMEOUT_MS);
    sqlite3_exec(handle, "PRAGMA foreign_keys = ON", NULL, NULL, NULL);

    DatabaseData *database = malloc(sizeof(DatabaseData));
    database->handle = handle;
    database->is_closed = 0;
    return native_object_new(database_class, database);
}

// --- Database methods ---

// db.execute(sql [, params]) -> int (rows changed)
static Value method_execute(int arg_count, Value *args)
{
    DatabaseData *database = open_database(args[0], "execute");
    int has_parameters = sql_and_optional_parameters(arg_count, args, "execute");
    sqlite3_stmt *statement = prepare_one_statement(database, args[1], "execute");
    bind_parameters(statement, has_parameters, has_parameters ? args[2] : (Value){VAL_VOID}, "execute");

    int result;
    while ((result = sqlite3_step(statement)) == SQLITE_ROW)
    {
        // A statement that returns rows (e.g. INSERT ... RETURNING) still runs to completion.
    }
    if (result != SQLITE_DONE)
        fail(statement, "execute(): %s", sqlite3_errmsg(database->handle));
    sqlite3_finalize(statement);

    Value changed;
    changed.type = VAL_INT;
    changed.int_val = sqlite3_changes64(database->handle);
    return changed;
}

// db.query(sql [, params]) -> list<map>
static Value method_query(int arg_count, Value *args)
{
    DatabaseData *database = open_database(args[0], "query");
    int has_parameters = sql_and_optional_parameters(arg_count, args, "query");
    sqlite3_stmt *statement = prepare_one_statement(database, args[1], "query");
    bind_parameters(statement, has_parameters, has_parameters ? args[2] : (Value){VAL_VOID}, "query");

    Value rows;
    rows.type = VAL_LIST;
    rows.list = (List *) allocate_obj(sizeof(List), OBJ_LIST);
    rows.list->element_spec = type_from_name("map");
    gc_push_value(rows); // Every column value below is a new allocation.

    int column_count = sqlite3_column_count(statement);
    int result;
    while ((result = sqlite3_step(statement)) == SQLITE_ROW)
    {
        Value row;
        row.type = VAL_HASHMAP;
        row.hashmap = hashmap_create(VAL_STRING, VAL_VOID);
        list_add(rows.list, row); // Reachable through the rooted list from here on.
        for (int column = 0; column < column_count; column++)
            hashmap_set(row.hashmap, sqlite3_column_name(statement, column), column_value(statement, column), 0);
    }
    gc_pop_root();
    if (result != SQLITE_DONE)
        fail(statement, "query(): %s", sqlite3_errmsg(database->handle));
    sqlite3_finalize(statement);
    return rows;
}

/**
 * @brief Runs SQL text that may contain several statements (no parameters).
 */
static void run_script(DatabaseData *database, const char *sql, const char *method)
{
    char *error = NULL;
    if (sqlite3_exec(database->handle, sql, NULL, NULL, &error) != SQLITE_OK)
    {
        char message[1024];
        snprintf(message, sizeof(message), "%s(): %s", method, error ? error : sqlite3_errmsg(database->handle));
        sqlite3_free(error);
        report_error(get_exec_error_line(), "%s", message);
    }
}

// db.execute_script(sql)
static Value method_execute_script(int arg_count, Value *args)
{
    DatabaseData *database = open_database(args[0], "execute_script");
    if (arg_count != 2 || args[1].type != VAL_STRING)
        report_error(get_exec_error_line(), "execute_script() takes exactly one string argument (the SQL).");
    run_script(database, AS_CSTRING(args[1]), "execute_script");
    return (Value){VAL_VOID};
}

/**
 * @brief Shared body of begin(), commit() and rollback().
 */
static Value transaction_command(int arg_count, Value *args, const char *method, const char *sql)
{
    DatabaseData *database = open_database(args[0], method);
    if (arg_count != 1)
        report_error(get_exec_error_line(), "%s() takes no arguments.", method);
    run_script(database, sql, method);
    return (Value){VAL_VOID};
}

// db.begin()
static Value method_begin(int arg_count, Value *args)
{
    return transaction_command(arg_count, args, "begin", "BEGIN");
}

// db.commit()
static Value method_commit(int arg_count, Value *args)
{
    return transaction_command(arg_count, args, "commit", "COMMIT");
}

// db.rollback()
static Value method_rollback(int arg_count, Value *args)
{
    return transaction_command(arg_count, args, "rollback", "ROLLBACK");
}

// db.last_insert_id() -> int
static Value method_last_insert_id(int arg_count, Value *args)
{
    DatabaseData *database = open_database(args[0], "last_insert_id");
    if (arg_count != 1)
        report_error(get_exec_error_line(), "last_insert_id() takes no arguments.");
    Value id;
    id.type = VAL_INT;
    id.int_val = sqlite3_last_insert_rowid(database->handle);
    return id;
}

// db.close()
static Value method_close(int arg_count, Value *args)
{
    DatabaseData *database = open_database(args[0], "close");
    if (arg_count != 1)
        report_error(get_exec_error_line(), "close() takes no arguments.");
    // close_v2 also succeeds if statements are somehow still open (they are
    // finalized automatically); every statement here is finalized eagerly anyway.
    sqlite3_close_v2(database->handle);
    database->handle = NULL;
    database->is_closed = 1;
    return (Value){VAL_VOID};
}

// db.is_closed() -> bool (allowed after close())
static Value method_is_closed(int arg_count, Value *args)
{
    DatabaseData *database = (DatabaseData *) native_object_data(args[0], database_class, "is_closed");
    if (arg_count != 1)
        report_error(get_exec_error_line(), "is_closed() takes no arguments.");
    Value closed;
    closed.type = VAL_BOOL;
    closed.int_val = database->is_closed;
    return closed;
}

/**
 * @brief GC finalizer: closes a connection the program did not close itself.
 */
static void database_finalize(void *data)
{
    DatabaseData *database = (DatabaseData *) data;
    if (!database->is_closed)
        sqlite3_close_v2(database->handle);
    free(database);
}

// --- Registration ---

void sqlite_register_module(HashMap *modules)
{
    database_class = native_class_create("Database", database_finalize, NULL);
    native_class_add_method(database_class, "execute", method_execute);
    native_class_add_method(database_class, "query", method_query);
    native_class_add_method(database_class, "execute_script", method_execute_script);
    native_class_add_method(database_class, "begin", method_begin);
    native_class_add_method(database_class, "commit", method_commit);
    native_class_add_method(database_class, "rollback", method_rollback);
    native_class_add_method(database_class, "last_insert_id", method_last_insert_id);
    native_class_add_method(database_class, "close", method_close);
    native_class_add_method(database_class, "is_closed", method_is_closed);

    HashMap *functions = hashmap_create(VAL_STRING, VAL_NATIVE_FN);
    register_native_method(functions, "open", native_sqlite_open);

    Value module;
    module.type = VAL_HASHMAP;
    module.hashmap = functions;
    hashmap_set(modules, "sqlite", module, 0);
}
