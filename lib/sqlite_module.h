/**
 * @file sqlite_module.h
 * @brief Native `sqlite` module: an embedded SQL database (SQLite, vendored in third_party/sqlite).
 *
 * @code
 *   import "sqlite"
 *
 *   Database db = sqlite.open("app.db")         # the file is created if missing
 *   db.execute_script("CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY, name TEXT NOT NULL, age INTEGER)")
 *
 *   db.execute("INSERT INTO users (name, age) VALUES (?, ?)", ["Ann", 31])
 *   int ann_id = db.last_insert_id()
 *   db.execute("UPDATE users SET age = :age WHERE id = :id", {"age": 32, "id": ann_id})
 *
 *   foreach (map row in db.query("SELECT id, name, age FROM users WHERE age > ?", [18])):
 *       int id = row["id"]
 *       string name = row["name"]
 *       print(str(id) + ": " + name)
 *
 *   db.close()
 * @endcode
 *
 * ## Module functions
 *
 *  - `sqlite.open(path)`              -> Database. Opens (creating if needed) a database
 *                                        file; ":memory:" opens a private in-memory database.
 *
 * ## Database methods
 *
 *  - `execute(sql [, params])`        -> int. Runs one statement (INSERT, UPDATE,
 *                                        DELETE, CREATE, ...) and returns the number of
 *                                        rows it changed.
 *  - `query(sql [, params])`          -> list<map>. Runs one SELECT (or any statement
 *                                        returning rows); each row is a map from column
 *                                        name to value, in column order.
 *  - `execute_script(sql)`            Runs several statements separated by ';' (e.g. a
 *                                        schema). Takes no parameters.
 *  - `last_insert_id()`               -> int. Row id of the most recent INSERT.
 *  - `begin()`, `commit()`, `rollback()`  Transactions: group changes so they all
 *                                        happen or none do (and run much faster in bulk).
 *  - `close()`                        Closes the database now. Afterwards every use of
 *                                        it raises an error (is_closed() excepted).
 *                                        Unclosed databases are closed when collected.
 *  - `is_closed()`                    -> bool.
 *
 * ## Parameters: never build SQL from user input
 *
 * Put values in the SQL as placeholders and pass them separately; SQLite then
 * treats them strictly as data, so text like `'; DROP TABLE users; --` cannot
 * change the statement (no SQL injection).
 *
 *  - A list fills positional placeholders `?` in order:
 *        db.query("SELECT * FROM users WHERE name = ? AND age > ?", [name, 18])
 *  - A map fills named placeholders `:name` (also `@name` / `$name`):
 *        db.execute("DELETE FROM users WHERE id = :id", {"id": 7})
 *
 * The number of values must match the placeholders exactly.
 *
 * ## Types
 *
 * | Pith value          | stored as  | read back as               |
 * |---------------------|------------|----------------------------|
 * | int                 | INTEGER    | int                        |
 * | float               | REAL       | float                      |
 * | string              | TEXT       | string                     |
 * | buffer              | BLOB       | string (the exact bytes)   |
 * | bool                | INTEGER 0/1| int                        |
 * | void                | NULL       | void                       |
 *
 * A NULL column is void in the row map, so `string name = row["name"]` raises a
 * clear type error if the column was NULL; check `row["name"] == void` first
 * for nullable columns. When two result columns have the same name (e.g. two
 * `id`s in a join), the later one wins: give them distinct names with AS.
 *
 * ## Errors
 *
 * Every SQLite error (syntax errors, constraint violations such as "UNIQUE
 * constraint failed: users.email", a locked database, ...) raises a rescuable
 * Pith error that includes SQLite's message.
 *
 * ## Settings applied to every connection
 *
 *  - `PRAGMA foreign_keys = ON` - FOREIGN KEY constraints are enforced (SQLite
 *    leaves them off by default for backward compatibility).
 *  - A 5-second busy timeout - if another process is writing, wait for it
 *    instead of failing immediately with "database is locked".
 */

#ifndef PITH_SQLITE_MODULE_H
#define PITH_SQLITE_MODULE_H

#include "value.h"

/**
 * @brief Registers the `sqlite` module and its Database type into @p modules.
 *
 * @param modules The interpreter's native-module registry.
 */
void sqlite_register_module(HashMap *modules);

#endif // PITH_SQLITE_MODULE_H
