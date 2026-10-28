/**
 * @file json_module.h
 * @brief Native `json` module: encode Pith values as JSON text and decode it back.
 *
 * @code
 *   import "json"
 *
 *   map<string, int> scores = {"ann": 3, "bob": 5}
 *   string text = json.encode(scores)          # {"ann":3,"bob":5}
 *
 *   map data = json.decode("{\"name\": \"Pith\", \"tags\": [\"a\", \"b\"], \"age\": 2}")
 *   string name = data["name"]
 *   list tags = data["tags"]
 *   int age = data["age"]
 * @endcode
 *
 * ## Encoding (json.encode)
 *
 *  - int, float, string, bool -> number, number, string, true/false
 *  - void -> null
 *  - list -> array; map -> object (in insertion order)
 *  - class instance -> object of its fields (in declaration order)
 *  - Floats with no fractional part are written with ".0" (e.g. 3.0) so they
 *    decode back as floats. NaN and infinity cannot be encoded (JSON has no
 *    representation for them).
 *
 * ## Decoding (json.decode)
 *
 *  - object -> map, array -> list (both untyped: they adopt an element type when
 *    stored in a typed variable, e.g. `map<string, int> m = json.decode(...)`)
 *  - number -> int if it is written without a fraction/exponent and fits in a
 *    64-bit int, otherwise float
 *  - string -> string (escapes, including \uXXXX surrogate pairs, become UTF-8)
 *  - true/false -> bool, null -> void
 *
 * Malformed input raises a rescuable error naming the byte offset.
 */

#ifndef PITH_JSON_MODULE_H
#define PITH_JSON_MODULE_H

#include "value.h"

/**
 * @brief Registers the `json` module (encode, decode) into @p modules.
 *
 * @param modules The interpreter's native-module registry.
 */
void json_register_module(HashMap *modules);

#endif // PITH_JSON_MODULE_H
