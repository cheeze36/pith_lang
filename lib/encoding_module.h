/**
 * @file encoding_module.h
 * @brief Native `encoding` module: base64 and hex conversion of byte strings.
 *
 * Cryptographic results (hashes, random bytes, MACs) are raw bytes, which are
 * not printable. These functions turn bytes into text that can be printed,
 * stored, put in a URL or a cookie, and back again.
 *
 * @code
 *   import "encoding"
 *
 *   string text = encoding.base64_encode("hello")     # "aGVsbG8="
 *   string back = encoding.base64_decode(text)        # "hello"
 *   string hex  = encoding.hex_encode("\x01\xff")     # "01ff"
 *   string safe = encoding.base64url_encode(token)    # URL/cookie-safe, no padding
 * @endcode
 *
 * ## Pith functions
 *
 *  - `base64_encode(bytes)`     -> string. Standard base64 (RFC 4648 section 4), with "=" padding.
 *  - `base64_decode(text)`      -> string (bytes). Strict: invalid characters or
 *                                  padding raise an error.
 *  - `base64url_encode(bytes)`  -> string. URL-safe alphabet ("-" and "_"), no
 *                                  padding (RFC 4648 section 5). Use for tokens in
 *                                  URLs, cookies and JSON Web Tokens.
 *  - `base64url_decode(text)`   -> string (bytes). Accepts the text with or
 *                                  without "=" padding.
 *  - `hex_encode(bytes)`        -> string. Two lowercase hex digits per byte.
 *  - `hex_decode(text)`         -> string (bytes). Upper- or lowercase digits;
 *                                  an odd length or a non-hex character raises an error.
 *
 * ## C API
 *
 * The encoders/decoders are also exported for other native modules (the crypto
 * module uses them for password hashes and tokens).
 */

#ifndef PITH_ENCODING_MODULE_H
#define PITH_ENCODING_MODULE_H

#include <stddef.h>
#include <stdint.h>
#include "value.h"

/**
 * @brief Encodes bytes as base64 text.
 *
 * @param data The bytes.
 * @param length Number of bytes.
 * @param url_safe Use the URL-safe alphabet ("-", "_") instead of ("+", "/").
 * @param padded Append "=" so the length is a multiple of 4.
 * @param text_length Receives the length of the result.
 * @return A malloc'd, NUL-terminated string; the caller frees it.
 */
char *base64_encode(const uint8_t *data, size_t length, int url_safe, int padded, size_t *text_length);

/**
 * @brief Decodes base64 text.
 *
 * @param text The text.
 * @param length Its length.
 * @param url_safe Expect the URL-safe alphabet instead of the standard one.
 * @param padding_required Reject text whose "=" padding is missing.
 * @param data_length Receives the number of decoded bytes.
 * @return A malloc'd buffer (the caller frees it), or NULL if the text is not
 *         valid base64.
 */
uint8_t *base64_decode(const char *text, size_t length, int url_safe, int padding_required, size_t *data_length);

/**
 * @brief Encodes bytes as lowercase hex.
 *
 * @return A malloc'd, NUL-terminated string of 2 * @p length characters.
 */
char *hex_encode(const uint8_t *data, size_t length);

/**
 * @brief Decodes hex text (upper- or lowercase).
 *
 * @param data_length Receives the number of decoded bytes.
 * @return A malloc'd buffer, or NULL if the length is odd or a character is not hex.
 */
uint8_t *hex_decode(const char *text, size_t length, size_t *data_length);

/**
 * @brief Registers the `encoding` module into @p modules.
 *
 * @param modules The interpreter's native-module registry.
 */
void encoding_register_module(HashMap *modules);

#endif // PITH_ENCODING_MODULE_H
