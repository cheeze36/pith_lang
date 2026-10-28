/**
 * @file sha256.h
 * @brief SHA-256, HMAC-SHA256 and PBKDF2-HMAC-SHA256 in portable C.
 *
 * Pure algorithms with no dependency on the interpreter, so they can be unit
 * tested in isolation and reused by any native module. The Pith-facing API is
 * in crypto_module.h.
 *
 * Standards implemented:
 *  - SHA-256:            FIPS 180-4, section 6.2
 *  - HMAC:               RFC 2104 (test vectors: RFC 4231)
 *  - PBKDF2 (with HMAC): RFC 8018, section 5.2 (test vectors: RFC 7914, section 11)
 *
 * Usage (one-shot):
 * @code
 *   uint8_t digest[SHA256_DIGEST_SIZE];
 *   sha256_hash((const uint8_t *) "abc", 3, digest);
 * @endcode
 *
 * Usage (incremental, for data that arrives in pieces):
 * @code
 *   Sha256Context context;
 *   sha256_init(&context);
 *   sha256_update(&context, first_chunk, first_length);
 *   sha256_update(&context, second_chunk, second_length);
 *   sha256_final(&context, digest);
 * @endcode
 */

#ifndef PITH_SHA256_H
#define PITH_SHA256_H

#include <stddef.h>
#include <stdint.h>

/** Size of a SHA-256 digest in bytes (256 bits). */
#define SHA256_DIGEST_SIZE 32

/** Size of the blocks SHA-256 processes, in bytes (512 bits). HMAC pads keys to this size. */
#define SHA256_BLOCK_SIZE 64

/**
 * @brief State of an incremental SHA-256 computation.
 *
 * Treat as opaque: initialize with sha256_init(), feed with sha256_update(),
 * finish with sha256_final(). A context may be copied (e.g. to reuse a common
 * prefix, which is how HMAC and PBKDF2 avoid rehashing their padded keys).
 */
typedef struct
{
    uint32_t state[8]; // The eight 32-bit working hash values (H0..H7).
    uint64_t total_length; // Total number of message bytes processed so far.
    uint8_t pending[SHA256_BLOCK_SIZE]; // Bytes waiting to fill a complete block.
    size_t pending_length; // How many bytes of `pending` are in use.
} Sha256Context;

/**
 * @brief Starts a new SHA-256 computation.
 *
 * @param context The context to initialize.
 */
void sha256_init(Sha256Context *context);

/**
 * @brief Adds @p length bytes of message data.
 *
 * May be called any number of times; the result is the same as hashing the
 * concatenation of all the pieces.
 *
 * @param context An initialized context.
 * @param data The bytes to add (may be NULL when @p length is 0).
 * @param length Number of bytes.
 */
void sha256_update(Sha256Context *context, const uint8_t *data, size_t length);

/**
 * @brief Finishes the computation and writes the 32-byte digest.
 *
 * The context must be re-initialized before it is used again.
 *
 * @param context The context.
 * @param digest Receives SHA256_DIGEST_SIZE bytes.
 */
void sha256_final(Sha256Context *context, uint8_t digest[SHA256_DIGEST_SIZE]);

/**
 * @brief Hashes a complete message in one call.
 *
 * @param data The message.
 * @param length Its length in bytes.
 * @param digest Receives SHA256_DIGEST_SIZE bytes.
 */
void sha256_hash(const uint8_t *data, size_t length, uint8_t digest[SHA256_DIGEST_SIZE]);

/**
 * @brief Computes HMAC-SHA256(key, message): a keyed hash that proves a message
 *        was produced by someone who knows the key.
 *
 * Keys longer than the block size are hashed first, as RFC 2104 specifies.
 *
 * @param key The secret key.
 * @param key_length Its length in bytes (any length).
 * @param message The message to authenticate.
 * @param message_length Its length in bytes.
 * @param mac Receives SHA256_DIGEST_SIZE bytes.
 */
void hmac_sha256(const uint8_t *key, size_t key_length, const uint8_t *message, size_t message_length,
                 uint8_t mac[SHA256_DIGEST_SIZE]);

/**
 * @brief Derives a key from a password with PBKDF2-HMAC-SHA256.
 *
 * Deliberately slow: the work grows linearly with @p iterations, which makes
 * guessing passwords from a stolen hash expensive.
 *
 * @param password The password bytes.
 * @param password_length Its length in bytes.
 * @param salt A random, per-password salt.
 * @param salt_length Its length in bytes.
 * @param iterations Number of iterations (at least 1).
 * @param output Receives @p output_length derived bytes.
 * @param output_length How many bytes to derive.
 */
void pbkdf2_hmac_sha256(const uint8_t *password, size_t password_length, const uint8_t *salt, size_t salt_length,
                        uint32_t iterations, uint8_t *output, size_t output_length);

#endif // PITH_SHA256_H
