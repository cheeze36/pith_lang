/**
 * @file sha256.c
 * @brief Implementation of SHA-256, HMAC-SHA256 and PBKDF2-HMAC-SHA256 (see sha256.h).
 *
 * The code follows the specifications step by step and names things after
 * them, so it can be checked against FIPS 180-4 / RFC 2104 / RFC 8018 directly.
 * Correctness is verified against the official test vectors in
 * tests/test_crypto.pith.
 */

#include <string.h>

#include "sha256.h"

// --- Helpers ---

/**
 * @brief Overwrites memory with zeros in a way the compiler may not optimize away.
 *
 * A plain memset() on a buffer that is never read again can legally be removed
 * by the optimizer; writing through a volatile pointer cannot. Used to clear
 * key material and intermediate hash state from the stack after use.
 */
static void secure_zero(void *memory, size_t length)
{
    volatile uint8_t *bytes = (volatile uint8_t *) memory;
    while (length--)
        *bytes++ = 0;
}

static uint32_t rotate_right(uint32_t value, unsigned bits)
{
    return (value >> bits) | (value << (32 - bits));
}

static uint32_t load_big_endian_32(const uint8_t *bytes)
{
    return ((uint32_t) bytes[0] << 24) | ((uint32_t) bytes[1] << 16) | ((uint32_t) bytes[2] << 8) |
           (uint32_t) bytes[3];
}

static void store_big_endian_32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t) (value >> 24);
    bytes[1] = (uint8_t) (value >> 16);
    bytes[2] = (uint8_t) (value >> 8);
    bytes[3] = (uint8_t) value;
}

// --- SHA-256 (FIPS 180-4) ---

/** The 64 round constants K0..K63 (section 4.2.2): the first 32 bits of the
 *  fractional parts of the cube roots of the first 64 primes. */
static const uint32_t ROUND_CONSTANTS[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

/** The initial hash values H0..H7 (section 5.3.3): the first 32 bits of the
 *  fractional parts of the square roots of the first 8 primes. */
static const uint32_t INITIAL_HASH[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
};

/**
 * @brief Processes one 64-byte block (the SHA-256 compression function, section 6.2.2).
 */
static void sha256_process_block(uint32_t state[8], const uint8_t block[SHA256_BLOCK_SIZE])
{
    // 1. Prepare the message schedule W0..W63.
    uint32_t schedule[64];
    for (int t = 0; t < 16; t++)
        schedule[t] = load_big_endian_32(block + 4 * t);
    for (int t = 16; t < 64; t++)
    {
        uint32_t small_sigma0 = rotate_right(schedule[t - 15], 7) ^ rotate_right(schedule[t - 15], 18) ^
                                (schedule[t - 15] >> 3);
        uint32_t small_sigma1 = rotate_right(schedule[t - 2], 17) ^ rotate_right(schedule[t - 2], 19) ^
                                (schedule[t - 2] >> 10);
        schedule[t] = small_sigma1 + schedule[t - 7] + small_sigma0 + schedule[t - 16];
    }

    // 2. Initialize the working variables a..h with the current hash value.
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

    // 3. Sixty-four rounds.
    for (int t = 0; t < 64; t++)
    {
        uint32_t big_sigma1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        uint32_t choose = (e & f) ^ (~e & g);
        uint32_t temp1 = h + big_sigma1 + choose + ROUND_CONSTANTS[t] + schedule[t];
        uint32_t big_sigma0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = big_sigma0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    // 4. Add the compressed block into the hash value.
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
    secure_zero(schedule, sizeof(schedule));
}

void sha256_init(Sha256Context *context)
{
    memcpy(context->state, INITIAL_HASH, sizeof(INITIAL_HASH));
    context->total_length = 0;
    context->pending_length = 0;
}

void sha256_update(Sha256Context *context, const uint8_t *data, size_t length)
{
    context->total_length += length;

    // Top up a partially filled block first.
    if (context->pending_length > 0)
    {
        size_t space = SHA256_BLOCK_SIZE - context->pending_length;
        size_t take = length < space ? length : space;
        memcpy(context->pending + context->pending_length, data, take);
        context->pending_length += take;
        data += take;
        length -= take;
        if (context->pending_length < SHA256_BLOCK_SIZE)
            return;
        sha256_process_block(context->state, context->pending);
        context->pending_length = 0;
    }

    // Whole blocks straight from the input.
    while (length >= SHA256_BLOCK_SIZE)
    {
        sha256_process_block(context->state, data);
        data += SHA256_BLOCK_SIZE;
        length -= SHA256_BLOCK_SIZE;
    }

    // Keep the remainder for next time.
    if (length > 0)
    {
        memcpy(context->pending, data, length);
        context->pending_length = length;
    }
}

void sha256_final(Sha256Context *context, uint8_t digest[SHA256_DIGEST_SIZE])
{
    // Padding (section 5.1.1): a single 1 bit, zeros, then the message length in
    // bits as a 64-bit big-endian number, filling the data to a multiple of 512 bits.
    uint64_t length_in_bits = context->total_length * 8;
    uint8_t padding[SHA256_BLOCK_SIZE * 2] = {0x80};
    size_t padding_length = (context->pending_length < 56 ? 56 : 120) - context->pending_length;
    for (int i = 0; i < 8; i++)
        padding[padding_length + (size_t) i] = (uint8_t) (length_in_bits >> (56 - 8 * i));
    sha256_update(context, padding, padding_length + 8);

    for (int i = 0; i < 8; i++)
        store_big_endian_32(digest + 4 * i, context->state[i]);
    secure_zero(context, sizeof(*context));
}

void sha256_hash(const uint8_t *data, size_t length, uint8_t digest[SHA256_DIGEST_SIZE])
{
    Sha256Context context;
    sha256_init(&context);
    sha256_update(&context, data, length);
    sha256_final(&context, digest);
}

// --- HMAC-SHA256 (RFC 2104) ---

/**
 * @brief HMAC state after absorbing the padded key: the inner and outer hashes
 *        with (key XOR ipad) and (key XOR opad) already processed.
 *
 * Preparing this once lets PBKDF2 compute each of its many HMACs with just two
 * more compression calls instead of four.
 */
typedef struct
{
    Sha256Context inner;
    Sha256Context outer;
} HmacSha256Key;

static void hmac_sha256_prepare(HmacSha256Key *prepared, const uint8_t *key, size_t key_length)
{
    // Keys longer than one block are replaced by their hash; shorter keys are
    // zero-padded to the block size.
    uint8_t block_key[SHA256_BLOCK_SIZE] = {0};
    if (key_length > SHA256_BLOCK_SIZE)
        sha256_hash(key, key_length, block_key);
    else if (key_length > 0)
        memcpy(block_key, key, key_length);

    uint8_t inner_pad[SHA256_BLOCK_SIZE];
    uint8_t outer_pad[SHA256_BLOCK_SIZE];
    for (int i = 0; i < SHA256_BLOCK_SIZE; i++)
    {
        inner_pad[i] = block_key[i] ^ 0x36; // ipad
        outer_pad[i] = block_key[i] ^ 0x5c; // opad
    }
    sha256_init(&prepared->inner);
    sha256_update(&prepared->inner, inner_pad, SHA256_BLOCK_SIZE);
    sha256_init(&prepared->outer);
    sha256_update(&prepared->outer, outer_pad, SHA256_BLOCK_SIZE);

    secure_zero(block_key, sizeof(block_key));
    secure_zero(inner_pad, sizeof(inner_pad));
    secure_zero(outer_pad, sizeof(outer_pad));
}

/**
 * @brief HMAC = H((key XOR opad) || H((key XOR ipad) || message)), using a prepared key.
 */
static void hmac_sha256_with_prepared(const HmacSha256Key *prepared, const uint8_t *message, size_t message_length,
                                      uint8_t mac[SHA256_DIGEST_SIZE])
{
    uint8_t inner_digest[SHA256_DIGEST_SIZE];
    Sha256Context context = prepared->inner;
    sha256_update(&context, message, message_length);
    sha256_final(&context, inner_digest);

    context = prepared->outer;
    sha256_update(&context, inner_digest, SHA256_DIGEST_SIZE);
    sha256_final(&context, mac);
    secure_zero(inner_digest, sizeof(inner_digest));
}

void hmac_sha256(const uint8_t *key, size_t key_length, const uint8_t *message, size_t message_length,
                 uint8_t mac[SHA256_DIGEST_SIZE])
{
    HmacSha256Key prepared;
    hmac_sha256_prepare(&prepared, key, key_length);
    hmac_sha256_with_prepared(&prepared, message, message_length, mac);
    secure_zero(&prepared, sizeof(prepared));
}

// --- PBKDF2-HMAC-SHA256 (RFC 8018, section 5.2) ---

void pbkdf2_hmac_sha256(const uint8_t *password, size_t password_length, const uint8_t *salt, size_t salt_length,
                        uint32_t iterations, uint8_t *output, size_t output_length)
{
    HmacSha256Key prepared;
    hmac_sha256_prepare(&prepared, password, password_length);

    // The output is the concatenation of blocks T1, T2, ... where
    //   T_i = U_1 XOR U_2 XOR ... XOR U_c
    //   U_1 = HMAC(password, salt || INT_32_BE(i)),  U_j = HMAC(password, U_{j-1})
    uint32_t block_index = 1;
    while (output_length > 0)
    {
        // U_1 hashes the salt followed by the block index; feed both to a copy
        // of the prepared inner hash instead of building a concatenated buffer.
        uint8_t index_bytes[4];
        store_big_endian_32(index_bytes, block_index);
        uint8_t inner_digest[SHA256_DIGEST_SIZE];
        Sha256Context context = prepared.inner;
        sha256_update(&context, salt, salt_length);
        sha256_update(&context, index_bytes, sizeof(index_bytes));
        sha256_final(&context, inner_digest);
        uint8_t u[SHA256_DIGEST_SIZE];
        context = prepared.outer;
        sha256_update(&context, inner_digest, SHA256_DIGEST_SIZE);
        sha256_final(&context, u);

        uint8_t block[SHA256_DIGEST_SIZE];
        memcpy(block, u, SHA256_DIGEST_SIZE);
        for (uint32_t j = 2; j <= iterations; j++)
        {
            hmac_sha256_with_prepared(&prepared, u, SHA256_DIGEST_SIZE, u);
            for (int k = 0; k < SHA256_DIGEST_SIZE; k++)
                block[k] ^= u[k];
        }

        size_t take = output_length < SHA256_DIGEST_SIZE ? output_length : SHA256_DIGEST_SIZE;
        memcpy(output, block, take);
        output += take;
        output_length -= take;
        block_index++;

        secure_zero(inner_digest, sizeof(inner_digest));
        secure_zero(u, sizeof(u));
        secure_zero(block, sizeof(block));
    }
    secure_zero(&prepared, sizeof(prepared));
}
