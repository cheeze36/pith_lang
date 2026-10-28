/**
 * @file crypto_module.c
 * @brief Implementation of the native `crypto` module (see crypto_module.h).
 *
 * The algorithms live in sha256.c and the text encodings in encoding_module.c;
 * this file adds the operating system's secure random number generator and the
 * Pith-facing functions with their argument checking.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#elif defined(__linux__)
#include <errno.h>
#include <sys/random.h>
#endif

#include "crypto_module.h"
#include "encoding_module.h"
#include "sha256.h"
#include "interpreter.h"
#include "common.h"
#include "pith_string.h"
#include "types.h"

/** PBKDF2 iterations for hash_password(): the OWASP (2023) recommendation for
 *  PBKDF2-HMAC-SHA256. Raising it makes stolen hashes harder to crack and
 *  every login slower. */
#define PASSWORD_HASH_ITERATIONS 600000

/** Random salt length for hash_password(), in bytes (128 bits). */
#define PASSWORD_SALT_SIZE 16

/** Length of the derived hash stored by hash_password(), in bytes. */
#define PASSWORD_HASH_SIZE SHA256_DIGEST_SIZE

/** Upper bound on iterations accepted from a stored hash, so a corrupted or
 *  hostile value cannot make verify_password() run for hours. */
#define MAX_PBKDF2_ITERATIONS 10000000

/** The identifier at the start of strings produced by hash_password()
 *  (PHC string format: $<algorithm>$<parameters>$<salt>$<hash>). */
#define PASSWORD_HASH_PREFIX "$pbkdf2-sha256$i="

#define MAX_RANDOM_BYTES (1024 * 1024)
#define DEFAULT_TOKEN_BYTES 32
#define MAX_TOKEN_BYTES 1024

// --- Operating-system randomness ---

/**
 * @brief Fills @p buffer with cryptographically secure random bytes from the OS.
 *
 * Uses getrandom() on Linux, arc4random_buf() on macOS and the BSDs,
 * BCryptGenRandom() on Windows, and /dev/urandom as a last resort elsewhere.
 * Raises a Pith error if no secure source is available: falling back to a
 * predictable generator would silently make every token guessable.
 */
static void fill_random_bytes(uint8_t *buffer, size_t length)
{
#if defined(_WIN32)
    if (BCryptGenRandom(NULL, buffer, (ULONG) length, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0)
        return;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    arc4random_buf(buffer, length);
    return;
#elif defined(__linux__)
    size_t filled = 0;
    while (filled < length)
    {
        ssize_t got = getrandom(buffer + filled, length - filled, 0);
        if (got < 0)
        {
            if (errno == EINTR)
                continue; // Interrupted by a signal: try again.
            break; // Unavailable (very old kernel): fall back to /dev/urandom.
        }
        filled += (size_t) got;
    }
    if (filled == length)
        return;
#endif
#if !defined(_WIN32)
    FILE *device = fopen("/dev/urandom", "rb");
    if (device != NULL)
    {
        size_t read = fread(buffer, 1, length, device);
        fclose(device);
        if (read == length)
            return;
    }
#endif
    report_error(get_exec_error_line(), "crypto: the operating system's secure random number generator is unavailable.");
}

/**
 * @brief Returns a uniformly distributed random integer in [0, bound) (bound > 0),
 *        or any 64-bit value when @p bound is 0 (meaning the full 2^64 range).
 *
 * Uses rejection sampling: values from the top, incomplete copy of the range are
 * discarded, so every result is exactly equally likely (no "modulo bias").
 */
static uint64_t random_below(uint64_t bound)
{
    uint64_t value;
    if (bound == 0)
    {
        fill_random_bytes((uint8_t *) &value, sizeof(value));
        return value;
    }
    uint64_t limit = UINT64_MAX - (UINT64_MAX % bound); // Largest multiple of bound (exclusive end)
    do
        fill_random_bytes((uint8_t *) &value, sizeof(value));
    while (value >= limit);
    return value % bound;
}

// --- Argument checking ---

/**
 * @brief Raises a type error unless args[index] is a string.
 */
static void require_string(Value *args, int index, const char *function_name, const char *parameter_name)
{
    if (args[index].type != VAL_STRING)
        report_error(get_exec_error_line(), "%s(): %s must be a string, got %s.", function_name, parameter_name,
                     type_describe_value(args[index]));
}

/**
 * @brief Raises a type error unless args[index] is an int in [minimum, maximum].
 *
 * @return The value.
 */
static int64_t require_int_in_range(Value *args, int index, const char *function_name, const char *parameter_name,
                                    int64_t minimum, int64_t maximum)
{
    if (args[index].type != VAL_INT)
        report_error(get_exec_error_line(), "%s(): %s must be an int, got %s.", function_name, parameter_name,
                     type_describe_value(args[index]));
    int64_t value = args[index].int_val;
    if (value < minimum || value > maximum)
        report_error(get_exec_error_line(), "%s(): %s must be between %" PITH_INT_FMT " and %" PITH_INT_FMT
                     ", got %" PITH_INT_FMT ".", function_name, parameter_name, minimum, maximum, value);
    return value;
}

static void require_argument_count(int arg_count, int expected, const char *function_name, const char *signature)
{
    if (arg_count != expected)
        report_error(get_exec_error_line(), "%s() takes %s.", function_name, signature);
}

static const uint8_t *string_bytes(Value value)
{
    return (const uint8_t *) AS_CSTRING(value);
}

static size_t string_byte_count(Value value)
{
    return (size_t) STRING_LENGTH(value);
}

/**
 * @brief Wraps raw bytes in a Pith string.
 */
static Value bytes_value(const uint8_t *bytes, size_t length)
{
    return string_value((const char *) bytes, (int) length);
}

// --- Randomness functions ---

// crypto.random_bytes(count) -> string of count random bytes
static Value native_random_bytes(int arg_count, Value *args)
{
    require_argument_count(arg_count, 1, "random_bytes", "one argument (the number of bytes)");
    size_t count = (size_t) require_int_in_range(args, 0, "random_bytes", "count", 0, MAX_RANDOM_BYTES);
    uint8_t *buffer = malloc(count > 0 ? count : 1);
    fill_random_bytes(buffer, count);
    Value result = bytes_value(buffer, count);
    free(buffer);
    return result;
}

// crypto.random_token([byte_count]) -> URL-safe base64 of byte_count random bytes (default 32)
static Value native_random_token(int arg_count, Value *args)
{
    if (arg_count > 1)
        report_error(get_exec_error_line(), "random_token() takes an optional number of random bytes.");
    size_t count = DEFAULT_TOKEN_BYTES;
    if (arg_count == 1)
        count = (size_t) require_int_in_range(args, 0, "random_token", "byte_count", 1, MAX_TOKEN_BYTES);

    uint8_t buffer[MAX_TOKEN_BYTES];
    fill_random_bytes(buffer, count);
    size_t token_length;
    char *token = base64_encode(buffer, count, 1, 0, &token_length);
    Value result = string_value(token, (int) token_length);
    free(token);
    return result;
}

// crypto.random_int(min, max) -> uniformly distributed int in [min, max]
static Value native_random_int(int arg_count, Value *args)
{
    require_argument_count(arg_count, 2, "random_int", "two arguments (min, max)");
    int64_t minimum = require_int_in_range(args, 0, "random_int", "min", INT64_MIN, INT64_MAX);
    int64_t maximum = require_int_in_range(args, 1, "random_int", "max", INT64_MIN, INT64_MAX);
    if (minimum > maximum)
        report_error(get_exec_error_line(), "random_int(): min must not be greater than max.");

    // The span is computed unsigned so the full int range does not overflow
    // (it wraps to 0, which random_below() treats as "all 2^64 values").
    uint64_t span = (uint64_t) maximum - (uint64_t) minimum + 1;
    Value result;
    result.type = VAL_INT;
    result.int_val = (int64_t) ((uint64_t) minimum + random_below(span));
    return result;
}

// --- Hashing functions ---

// crypto.sha256(data) -> string of 32 bytes
static Value native_sha256(int arg_count, Value *args)
{
    require_argument_count(arg_count, 1, "sha256", "one argument (the data)");
    require_string(args, 0, "sha256", "data");
    uint8_t digest[SHA256_DIGEST_SIZE];
    sha256_hash(string_bytes(args[0]), string_byte_count(args[0]), digest);
    return bytes_value(digest, sizeof(digest));
}

// crypto.hmac_sha256(key, message) -> string of 32 bytes
static Value native_hmac_sha256(int arg_count, Value *args)
{
    require_argument_count(arg_count, 2, "hmac_sha256", "two arguments (key, message)");
    require_string(args, 0, "hmac_sha256", "key");
    require_string(args, 1, "hmac_sha256", "message");
    uint8_t mac[SHA256_DIGEST_SIZE];
    hmac_sha256(string_bytes(args[0]), string_byte_count(args[0]), string_bytes(args[1]),
                string_byte_count(args[1]), mac);
    return bytes_value(mac, sizeof(mac));
}

/**
 * @brief Compares two byte sequences in time that depends only on their lengths.
 *
 * Every byte is examined even after a difference is found, so the running time
 * reveals nothing about where (or whether) the contents differ.
 */
static int bytes_equal_in_constant_time(const uint8_t *a, size_t a_length, const uint8_t *b, size_t b_length)
{
    size_t length = a_length > b_length ? a_length : b_length;
    unsigned difference = (unsigned) (a_length != b_length);
    for (size_t i = 0; i < length; i++)
    {
        uint8_t byte_a = i < a_length ? a[i] : 0;
        uint8_t byte_b = i < b_length ? b[i] : 0;
        difference |= (unsigned) (byte_a ^ byte_b);
    }
    return difference == 0;
}

// crypto.timing_safe_equal(a, b) -> bool
static Value native_timing_safe_equal(int arg_count, Value *args)
{
    require_argument_count(arg_count, 2, "timing_safe_equal", "two string arguments");
    require_string(args, 0, "timing_safe_equal", "the first argument");
    require_string(args, 1, "timing_safe_equal", "the second argument");
    Value result;
    result.type = VAL_BOOL;
    result.int_val = bytes_equal_in_constant_time(string_bytes(args[0]), string_byte_count(args[0]),
                                                  string_bytes(args[1]), string_byte_count(args[1]));
    return result;
}

// crypto.pbkdf2_sha256(password, salt, iterations, byte_count) -> string of byte_count bytes
static Value native_pbkdf2_sha256(int arg_count, Value *args)
{
    require_argument_count(arg_count, 4, "pbkdf2_sha256", "four arguments (password, salt, iterations, byte_count)");
    require_string(args, 0, "pbkdf2_sha256", "password");
    require_string(args, 1, "pbkdf2_sha256", "salt");
    uint32_t iterations = (uint32_t) require_int_in_range(args, 2, "pbkdf2_sha256", "iterations", 1,
                                                          MAX_PBKDF2_ITERATIONS);
    size_t byte_count = (size_t) require_int_in_range(args, 3, "pbkdf2_sha256", "byte_count", 1, 1024);

    uint8_t *output = malloc(byte_count);
    pbkdf2_hmac_sha256(string_bytes(args[0]), string_byte_count(args[0]), string_bytes(args[1]),
                       string_byte_count(args[1]), iterations, output, byte_count);
    Value result = bytes_value(output, byte_count);
    free(output);
    return result;
}

// --- Password hashing ---

// crypto.hash_password(password) -> "$pbkdf2-sha256$i=<iterations>$<salt>$<hash>"
static Value native_hash_password(int arg_count, Value *args)
{
    require_argument_count(arg_count, 1, "hash_password", "one argument (the password)");
    require_string(args, 0, "hash_password", "password");

    uint8_t salt[PASSWORD_SALT_SIZE];
    fill_random_bytes(salt, sizeof(salt));
    uint8_t hash[PASSWORD_HASH_SIZE];
    pbkdf2_hmac_sha256(string_bytes(args[0]), string_byte_count(args[0]), salt, sizeof(salt),
                       PASSWORD_HASH_ITERATIONS, hash, sizeof(hash));

    // PHC string format uses standard base64 without padding for salt and hash.
    size_t salt_text_length, hash_text_length;
    char *salt_text = base64_encode(salt, sizeof(salt), 0, 0, &salt_text_length);
    char *hash_text = base64_encode(hash, sizeof(hash), 0, 0, &hash_text_length);
    char encoded[256];
    int encoded_length = snprintf(encoded, sizeof(encoded), "%s%d$%s$%s", PASSWORD_HASH_PREFIX,
                                  PASSWORD_HASH_ITERATIONS, salt_text, hash_text);
    free(salt_text);
    free(hash_text);
    return string_value(encoded, encoded_length);
}

/**
 * @brief A parsed "$pbkdf2-sha256$i=N$salt$hash" string.
 */
typedef struct
{
    uint32_t iterations;
    uint8_t *salt;
    size_t salt_length;
    uint8_t *hash;
    size_t hash_length;
} StoredPasswordHash;

/**
 * @brief Parses a string produced by hash_password().
 *
 * @return 1 on success (the caller frees salt and hash), 0 if the string is malformed.
 */
static int parse_stored_password_hash(const char *text, size_t length, StoredPasswordHash *parsed)
{
    size_t prefix_length = strlen(PASSWORD_HASH_PREFIX);
    if (length <= prefix_length || strncmp(text, PASSWORD_HASH_PREFIX, prefix_length) != 0)
        return 0;

    // Iterations: decimal digits up to the next '$'.
    size_t position = prefix_length;
    uint64_t iterations = 0;
    size_t digits_start = position;
    while (position < length && text[position] >= '0' && text[position] <= '9')
    {
        iterations = iterations * 10 + (uint64_t) (text[position] - '0');
        if (iterations > MAX_PBKDF2_ITERATIONS)
            return 0;
        position++;
    }
    if (position == digits_start || iterations == 0 || position >= length || text[position] != '$')
        return 0;
    position++;

    // Salt and hash: base64 (no padding) separated by a single '$'.
    const char *salt_text = text + position;
    const char *separator = memchr(salt_text, '$', length - position);
    if (separator == NULL)
        return 0;
    size_t salt_text_length = (size_t) (separator - salt_text);
    const char *hash_text = separator + 1;
    size_t hash_text_length = length - (size_t) (hash_text - text);
    if (salt_text_length == 0 || hash_text_length == 0 || memchr(hash_text, '$', hash_text_length) != NULL)
        return 0;

    parsed->salt = base64_decode(salt_text, salt_text_length, 0, 0, &parsed->salt_length);
    parsed->hash = base64_decode(hash_text, hash_text_length, 0, 0, &parsed->hash_length);
    if (parsed->salt == NULL || parsed->hash == NULL || parsed->hash_length < 16 || parsed->hash_length > 64)
    {
        free(parsed->salt);
        free(parsed->hash);
        return 0;
    }
    parsed->iterations = (uint32_t) iterations;
    return 1;
}

// crypto.verify_password(password, stored) -> bool
static Value native_verify_password(int arg_count, Value *args)
{
    require_argument_count(arg_count, 2, "verify_password", "two arguments (password, stored_hash)");
    require_string(args, 0, "verify_password", "password");
    require_string(args, 1, "verify_password", "stored_hash");

    StoredPasswordHash stored;
    if (!parse_stored_password_hash(AS_CSTRING(args[1]), string_byte_count(args[1]), &stored))
        report_error(get_exec_error_line(),
                     "verify_password(): stored_hash is not a hash produced by hash_password() "
                     "(expected \"$pbkdf2-sha256$i=<iterations>$<salt>$<hash>\").");

    uint8_t candidate[64];
    pbkdf2_hmac_sha256(string_bytes(args[0]), string_byte_count(args[0]), stored.salt, stored.salt_length,
                       stored.iterations, candidate, stored.hash_length);
    int matches = bytes_equal_in_constant_time(candidate, stored.hash_length, stored.hash, stored.hash_length);
    free(stored.salt);
    free(stored.hash);

    Value result;
    result.type = VAL_BOOL;
    result.int_val = matches;
    return result;
}

// --- Registration ---

void crypto_register_module(HashMap *modules)
{
    HashMap *functions = hashmap_create(VAL_STRING, VAL_NATIVE_FN);
    register_native_method(functions, "random_bytes", native_random_bytes);
    register_native_method(functions, "random_token", native_random_token);
    register_native_method(functions, "random_int", native_random_int);
    register_native_method(functions, "sha256", native_sha256);
    register_native_method(functions, "hmac_sha256", native_hmac_sha256);
    register_native_method(functions, "timing_safe_equal", native_timing_safe_equal);
    register_native_method(functions, "pbkdf2_sha256", native_pbkdf2_sha256);
    register_native_method(functions, "hash_password", native_hash_password);
    register_native_method(functions, "verify_password", native_verify_password);

    Value module;
    module.type = VAL_HASHMAP;
    module.hashmap = functions;
    hashmap_set(modules, "crypto", module, 0);
}
