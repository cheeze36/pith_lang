/**
 * @file crypto_module.h
 * @brief Native `crypto` module: secure randomness, hashing, HMAC and password hashing.
 *
 * @code
 *   import "crypto"
 *   import "encoding"
 *   import "sys"
 *
 *   # Session ids, CSRF tokens, reset links: unguessable random text
 *   string session_id = crypto.random_token()               # 43 URL-safe characters
 *
 *   # Storing passwords: never store the password itself
 *   string stored = crypto.hash_password("correct horse")
 *   bool ok = crypto.verify_password("correct horse", stored)   # true
 *
 *   # Signing data so it cannot be tampered with (e.g. a cookie value)
 *   string secret = sys.env("APP_SECRET")
 *   string signature = encoding.base64url_encode(crypto.hmac_sha256(secret, "user=42"))
 *   bool authentic = crypto.timing_safe_equal(signature, received_signature)
 *
 *   # Checksums / content hashes
 *   print(encoding.hex_encode(crypto.sha256("abc")))
 * @endcode
 *
 * ## Functions
 *
 * Randomness (from the operating system's cryptographically secure generator,
 * never the predictable `random` module):
 *  - `random_bytes(count)`      -> string of @c count random bytes (0..1048576).
 *  - `random_token()`           -> string. 32 random bytes as URL-safe base64
 *                                  (43 characters, 256 bits of entropy).
 *  - `random_token(byte_count)` -> string. Same, with @c byte_count random bytes (1..1024).
 *  - `random_int(min, max)`     -> int, uniformly distributed in [min, max]
 *                                  (e.g. one-time codes); no modulo bias.
 *
 * Hashing and authentication (results are raw bytes: use the `encoding` module
 * to turn them into hex or base64 text):
 *  - `sha256(data)`             -> string of 32 bytes.
 *  - `hmac_sha256(key, message)` -> string of 32 bytes. A keyed hash: only someone
 *                                  with @c key can produce it, so it proves a
 *                                  message came from you and was not modified.
 *  - `timing_safe_equal(a, b)`  -> bool. Compares two strings in time that does not
 *                                  depend on where they differ. Always use this to
 *                                  check tokens, signatures and MACs: an ordinary
 *                                  == can leak, through timing, how many leading
 *                                  characters of a guess were right.
 *
 * Passwords:
 *  - `hash_password(password)`  -> string, e.g.
 *        "$pbkdf2-sha256$i=600000$<salt>$<hash>"
 *    PBKDF2-HMAC-SHA256 with a fresh 16-byte random salt and 600,000 iterations
 *    (the OWASP recommendation for this algorithm). The string records the
 *    algorithm, iteration count and salt, so it is all you need to store.
 *  - `verify_password(password, stored)` -> bool. Re-derives the hash with the
 *    stored salt and iterations and compares in constant time. A @c stored
 *    value that is not in the format above raises an error.
 *  - `pbkdf2_sha256(password, salt, iterations, byte_count)` -> string of
 *    @c byte_count bytes. The raw key-derivation function, for deriving
 *    encryption keys from passwords; prefer hash_password() for storing passwords.
 *
 * ## Notes
 *
 *  - Password hashing is deliberately slow (roughly a quarter of a second, see
 *    PASSWORD_HASH_ITERATIONS). The http server handles one request at a time,
 *    so a login request delays other requests by that much.
 *  - Strings are byte strings, so all functions work on binary data as well as text.
 */

#ifndef PITH_CRYPTO_MODULE_H
#define PITH_CRYPTO_MODULE_H

#include "value.h"

/**
 * @brief Registers the `crypto` module into @p modules.
 *
 * @param modules The interpreter's native-module registry.
 */
void crypto_register_module(HashMap *modules);

#endif // PITH_CRYPTO_MODULE_H
