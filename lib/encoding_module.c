/**
 * @file encoding_module.c
 * @brief Implementation of the native `encoding` module (see encoding_module.h).
 */

#include <stdlib.h>
#include <string.h>

#include "encoding_module.h"
#include "interpreter.h"
#include "common.h"
#include "pith_string.h"
#include "types.h"

// --- Base64 (RFC 4648) ---

static const char STANDARD_ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char URL_SAFE_ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

char *base64_encode(const uint8_t *data, size_t length, int url_safe, int padded, size_t *text_length)
{
    const char *alphabet = url_safe ? URL_SAFE_ALPHABET : STANDARD_ALPHABET;
    char *text = malloc((length + 2) / 3 * 4 + 1);
    size_t out = 0;

    // Every 3 input bytes (24 bits) become 4 characters of 6 bits each.
    size_t i = 0;
    for (; i + 3 <= length; i += 3)
    {
        uint32_t group = ((uint32_t) data[i] << 16) | ((uint32_t) data[i + 1] << 8) | data[i + 2];
        text[out++] = alphabet[(group >> 18) & 63];
        text[out++] = alphabet[(group >> 12) & 63];
        text[out++] = alphabet[(group >> 6) & 63];
        text[out++] = alphabet[group & 63];
    }

    // A final 1 or 2 bytes become 2 or 3 characters (plus "=" padding to 4).
    size_t remaining = length - i;
    if (remaining > 0)
    {
        uint32_t group = (uint32_t) data[i] << 16;
        if (remaining == 2)
            group |= (uint32_t) data[i + 1] << 8;
        text[out++] = alphabet[(group >> 18) & 63];
        text[out++] = alphabet[(group >> 12) & 63];
        if (remaining == 2)
            text[out++] = alphabet[(group >> 6) & 63];
        if (padded)
        {
            text[out++] = '=';
            if (remaining == 1)
                text[out++] = '=';
        }
    }
    text[out] = '\0';
    *text_length = out;
    return text;
}

/**
 * @brief Value (0-63) of a base64 character in the chosen alphabet, or -1.
 */
static int base64_value(char character, int url_safe)
{
    if (character >= 'A' && character <= 'Z')
        return character - 'A';
    if (character >= 'a' && character <= 'z')
        return character - 'a' + 26;
    if (character >= '0' && character <= '9')
        return character - '0' + 52;
    if (character == (url_safe ? '-' : '+'))
        return 62;
    if (character == (url_safe ? '_' : '/'))
        return 63;
    return -1;
}

uint8_t *base64_decode(const char *text, size_t length, int url_safe, int padding_required, size_t *data_length)
{
    // Strip "=" padding (at most two), then check that its presence is consistent.
    size_t padding = 0;
    while (length > 0 && text[length - 1] == '=' && padding < 2)
    {
        length--;
        padding++;
    }
    size_t final_group = length % 4; // Characters in the last, partial group (0, 2 or 3)
    if (final_group == 1)
        return NULL; // A single leftover character cannot encode a whole byte.
    if (padding > 0 && (length + padding) % 4 != 0)
        return NULL; // Padding must complete the final group exactly.
    if (padding_required && final_group != 0 && padding == 0)
        return NULL;

    uint8_t *data = malloc(length / 4 * 3 + 3);
    size_t out = 0;
    uint32_t group = 0;
    int characters_in_group = 0;
    for (size_t i = 0; i < length; i++)
    {
        int value = base64_value(text[i], url_safe);
        if (value < 0)
        {
            free(data);
            return NULL;
        }
        group = (group << 6) | (uint32_t) value;
        if (++characters_in_group == 4)
        {
            data[out++] = (uint8_t) (group >> 16);
            data[out++] = (uint8_t) (group >> 8);
            data[out++] = (uint8_t) group;
            group = 0;
            characters_in_group = 0;
        }
    }
    // A partial final group: 2 characters = 1 byte, 3 characters = 2 bytes.
    // The unused low bits must be zero, so every input has exactly one decoding.
    if (characters_in_group == 2)
    {
        if (group & 0x0F)
        {
            free(data);
            return NULL;
        }
        data[out++] = (uint8_t) (group >> 4);
    }
    else if (characters_in_group == 3)
    {
        if (group & 0x03)
        {
            free(data);
            return NULL;
        }
        data[out++] = (uint8_t) (group >> 10);
        data[out++] = (uint8_t) (group >> 2);
    }
    *data_length = out;
    return data;
}

// --- Hex ---

char *hex_encode(const uint8_t *data, size_t length)
{
    static const char DIGITS[] = "0123456789abcdef";
    char *text = malloc(length * 2 + 1);
    for (size_t i = 0; i < length; i++)
    {
        text[2 * i] = DIGITS[data[i] >> 4];
        text[2 * i + 1] = DIGITS[data[i] & 0x0F];
    }
    text[length * 2] = '\0';
    return text;
}

static int hex_value(char character)
{
    if (character >= '0' && character <= '9')
        return character - '0';
    if (character >= 'a' && character <= 'f')
        return character - 'a' + 10;
    if (character >= 'A' && character <= 'F')
        return character - 'A' + 10;
    return -1;
}

uint8_t *hex_decode(const char *text, size_t length, size_t *data_length)
{
    if (length % 2 != 0)
        return NULL;
    uint8_t *data = malloc(length / 2 + 1);
    for (size_t i = 0; i < length; i += 2)
    {
        int high = hex_value(text[i]);
        int low = hex_value(text[i + 1]);
        if (high < 0 || low < 0)
        {
            free(data);
            return NULL;
        }
        data[i / 2] = (uint8_t) (high * 16 + low);
    }
    *data_length = length / 2;
    return data;
}

// --- Pith functions ---

/**
 * @brief Checks that a native function received exactly one string argument.
 */
static void require_one_string(int arg_count, Value *args, const char *function_name)
{
    if (arg_count != 1 || args[0].type != VAL_STRING)
        report_error(get_exec_error_line(), "%s() takes exactly one string argument, got %s.", function_name,
                     arg_count == 1 ? type_describe_value(args[0]) : "a different number of arguments");
}

/**
 * @brief Wraps a malloc'd text result as a Pith string and frees the buffer.
 */
static Value take_text(char *text, size_t length)
{
    Value result = string_value(text, (int) length);
    free(text);
    return result;
}

/**
 * @brief Wraps a decoder's malloc'd result as a Pith (byte) string, or raises an error.
 */
static Value take_decoded(uint8_t *data, size_t length, const char *function_name, const char *format_name)
{
    if (data == NULL)
        report_error(get_exec_error_line(), "%s(): the text is not valid %s.", function_name, format_name);
    Value result = string_value((const char *) data, (int) length);
    free(data);
    return result;
}

// encoding.base64_encode(bytes) -> string
static Value native_base64_encode(int arg_count, Value *args)
{
    require_one_string(arg_count, args, "base64_encode");
    size_t length;
    char *text = base64_encode((const uint8_t *) AS_CSTRING(args[0]), (size_t) STRING_LENGTH(args[0]), 0, 1,
                               &length);
    return take_text(text, length);
}

// encoding.base64_decode(text) -> string (bytes)
static Value native_base64_decode(int arg_count, Value *args)
{
    require_one_string(arg_count, args, "base64_decode");
    size_t length;
    uint8_t *data = base64_decode(AS_CSTRING(args[0]), (size_t) STRING_LENGTH(args[0]), 0, 1, &length);
    return take_decoded(data, length, "base64_decode", "base64");
}

// encoding.base64url_encode(bytes) -> string (URL-safe alphabet, no padding)
static Value native_base64url_encode(int arg_count, Value *args)
{
    require_one_string(arg_count, args, "base64url_encode");
    size_t length;
    char *text = base64_encode((const uint8_t *) AS_CSTRING(args[0]), (size_t) STRING_LENGTH(args[0]), 1, 0,
                               &length);
    return take_text(text, length);
}

// encoding.base64url_decode(text) -> string (bytes); padding optional
static Value native_base64url_decode(int arg_count, Value *args)
{
    require_one_string(arg_count, args, "base64url_decode");
    size_t length;
    uint8_t *data = base64_decode(AS_CSTRING(args[0]), (size_t) STRING_LENGTH(args[0]), 1, 0, &length);
    return take_decoded(data, length, "base64url_decode", "URL-safe base64");
}

// encoding.hex_encode(bytes) -> string
static Value native_hex_encode(int arg_count, Value *args)
{
    require_one_string(arg_count, args, "hex_encode");
    size_t length = (size_t) STRING_LENGTH(args[0]);
    return take_text(hex_encode((const uint8_t *) AS_CSTRING(args[0]), length), length * 2);
}

// encoding.hex_decode(text) -> string (bytes)
static Value native_hex_decode(int arg_count, Value *args)
{
    require_one_string(arg_count, args, "hex_decode");
    size_t length;
    uint8_t *data = hex_decode(AS_CSTRING(args[0]), (size_t) STRING_LENGTH(args[0]), &length);
    return take_decoded(data, length, "hex_decode", "hex (an even number of 0-9 / a-f digits)");
}

void encoding_register_module(HashMap *modules)
{
    HashMap *functions = hashmap_create(VAL_STRING, VAL_NATIVE_FN);
    register_native_method(functions, "base64_encode", native_base64_encode);
    register_native_method(functions, "base64_decode", native_base64_decode);
    register_native_method(functions, "base64url_encode", native_base64url_encode);
    register_native_method(functions, "base64url_decode", native_base64url_decode);
    register_native_method(functions, "hex_encode", native_hex_encode);
    register_native_method(functions, "hex_decode", native_hex_decode);

    Value module;
    module.type = VAL_HASHMAP;
    module.hashmap = functions;
    hashmap_set(modules, "encoding", module, 0);
}
