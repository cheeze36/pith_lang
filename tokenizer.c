/**
 * @file tokenizer.c
 * @brief Implementation of the Pith language tokenizer (lexer).
 *
 * This file contains the logic for converting raw source code text into a sequence of tokens.
 * It handles significant whitespace (indentation/dedentation), comments, literals (strings, numbers),
 * keywords, and various operators. The tokenizer produces a stream of tokens that is consumed by the parser.
 */

#include "tokenizer.h"
#include "debug.h"
#include "common.h" // report_error() for malformed string literals
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>

/**
 * @brief Adds a token to the tokenizer state.
 *
 * Resizes the token array if necessary and appends the new token.
 *
 * @param state The tokenizer state.
 * @param type The type of the token.
 * @param value The value of the token (e.g., a number or identifier). Can be NULL for simple tokens.
 * @param line_num The line number for error reporting.
 */
/**
 * @brief Appends a token whose value is @p length bytes that may include NULs.
 *
 * The stored value is still NUL-terminated for code that treats it as text.
 */
static void add_token_bytes(TokenizerState *state, TokenType type, const char *value, int length, int line_num)
{
    if (state->token_count >= state->token_capacity)
    {
        state->token_capacity = (state->token_capacity == 0) ? 128 : state->token_capacity * 2;
        state->tokens = realloc(state->tokens, state->token_capacity * sizeof(Token));
    }
    char *copy = NULL;
    if (value)
    {
        copy = malloc((size_t) length + 1);
        memcpy(copy, value, (size_t) length);
        copy[length] = '\0';
    }
    state->tokens[state->token_count].type = type;
    state->tokens[state->token_count].value = copy;
    state->tokens[state->token_count].length = value ? length : 0;
    state->tokens[state->token_count].line_num = line_num;
    state->token_count++;
}

void add_token(TokenizerState *state, TokenType type, const char *value, int line_num)
{
    add_token_bytes(state, type, value, value ? (int) strlen(value) : 0, line_num);
}

/**
 * @brief Returns the value of hex digit @p c, or -1 if it is not one.
 */
static int hex_digit_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/**
 * @brief Writes the UTF-8 encoding of @p code_point into @p out.
 *
 * @return The number of bytes written (1-4).
 */
static int encode_utf8(unsigned long code_point, char *out)
{
    if (code_point < 0x80)
    {
        out[0] = (char) code_point;
        return 1;
    }
    if (code_point < 0x800)
    {
        out[0] = (char) (0xC0 | (code_point >> 6));
        out[1] = (char) (0x80 | (code_point & 0x3F));
        return 2;
    }
    if (code_point < 0x10000)
    {
        out[0] = (char) (0xE0 | (code_point >> 12));
        out[1] = (char) (0x80 | ((code_point >> 6) & 0x3F));
        out[2] = (char) (0x80 | (code_point & 0x3F));
        return 3;
    }
    out[0] = (char) (0xF0 | (code_point >> 18));
    out[1] = (char) (0x80 | ((code_point >> 12) & 0x3F));
    out[2] = (char) (0x80 | ((code_point >> 6) & 0x3F));
    out[3] = (char) (0x80 | (code_point & 0x3F));
    return 4;
}

const char *keywords[] = {
    "print", "define", "return", "int", "string", "void", "float", "bool",
    "if", "else", "elif", "while", "for", "foreach", "in", "do", "switch", "case", "default",
    "break", "continue", "pass", "true", "false", "and", "or", "map", "import",
    "class", "new", "list", "extends", "try", "rescue", "finally", "raise", "buffer", NULL
};

/**
 * @brief Checks if a word is a reserved keyword.
 *
 * @param word The word to check.
 * @return 1 if the word is a keyword, 0 otherwise.
 */
int is_keyword(const char *word)
{
    for (int i = 0; keywords[i] != NULL; i++)
    {
        if (strcmp(word, keywords[i]) == 0)
            return 1;
    }
    return 0;
}

/**
 * @brief Tokenizes a source string.
 *
 * Scans the source string character by character and populates the tokenizer state with tokens.
 * Handles indentation tracking for block structure.
 *
 * @param source The source string to tokenize.
 * @param state The tokenizer state to populate.
 */
void tokenize(const char *source, TokenizerState *state)
{
    state->tokens = NULL;
    state->token_count = 0;
    state->token_capacity = 0;

    int i = 0;
    int line_num = 1;
    int indent_stack[100];
    int indent_level = 0;
    indent_stack[0] = 0;

    int at_line_start = 1;

    // Number of currently open (, [ and { brackets. While any is open, a line
    // break is ordinary whitespace (no NEWLINE token, no indentation change),
    // so long calls, lists and maps can be split across lines:
    //     print(hex_is(crypto.sha256(data),
    //                  "e3b0c442..."))
    int bracket_depth = 0;

    while (source[i] != '\0')
    {
        char c = source[i];

        // DO NOT DISCARD DEBUG CODE
#ifdef DEBUG_TRACE_TOKENIZER
        printf("[TOKENIZER] Processing char '%c' at index %d\n", c, i); fflush(stdout);
#endif

        // --- Comment Handling ---
        if (c == '#')
        {
            // Check for multi-line comment start '###'
            if (source[i + 1] == '#' && source[i + 2] == '#')
            {
#ifdef DEBUG_TRACE_TOKENIZER
                printf("[TOKENIZER] Entering multi-line comment at line %d\n", line_num); fflush(stdout);
#endif
                i += 3; // Skip '###'
                while (source[i] != '\0')
                {
                    // Check for multi-line comment end '###'
                    if (source[i] == '#' && source[i + 1] == '#' && source[i + 2] == '#')
                    {
                        i += 3; // Skip '###'
#ifdef DEBUG_TRACE_TOKENIZER
                        printf("[TOKENIZER] Exiting multi-line comment at line %d\n", line_num); fflush(stdout);
#endif
                        break;
                    }
                    if (source[i] == '\n')
                    {
                        line_num++;
                    }
                    i++;
                }
                continue;
            }
            else
            {
                // Single-line comment using '#'
                while (source[i] != '\n' && source[i] != '\0')
                    i++;
                continue;
            }
        }

        // --- Indentation Handling ---
        if (at_line_start)
        {
            int spaces = 0;
            while (source[i] == ' ' || source[i] == '\t')
            {
                spaces++;
                i++;
            }

            // Handle CR (Windows line endings)
            if (source[i] == '\r')
            {
#ifdef DEBUG_TRACE_TOKENIZER
                printf("[TOKENIZER_DEBUG] Skipping CR at index %d\n", i);
#endif
                i++;
            }

            // Skip empty lines
            if (source[i] == '\n' || source[i] == '\0')
            {
#ifdef DEBUG_TRACE_TOKENIZER
                printf("[TOKENIZER_DEBUG] Empty line at index %d\n", i);
#endif
                at_line_start = 1;
                if (source[i] == '\n')
                {
                    i++;
                    line_num++;
                }
                continue;
            }

#ifdef DEBUG_TRACE_TOKENIZER
            printf("[TOKENIZER_DEBUG] Line start at index %d, spaces=%d, indent_level=%d, stack[level]=%d\n", i, spaces,
                   indent_level, indent_stack[indent_level]);
#endif

            // Check for indentation increase (INDENT)
            if (spaces > indent_stack[indent_level])
            {
                indent_level++;
                indent_stack[indent_level] = spaces;
                add_token(state, TOKEN_INDENT, NULL, line_num);
            }
            // Check for indentation decrease (DEDENT)
            else
            {
                while (spaces < indent_stack[indent_level])
                {
#ifdef DEBUG_TRACE_TOKENIZER
                    printf("[TOKENIZER_DEBUG] Emitting DEDENT. spaces=%d < stack[%d]=%d\n", spaces, indent_level,
                           indent_stack[indent_level]);
#endif
                    add_token(state, TOKEN_DEDENT, NULL, line_num);
                    indent_level--;
                }
            }
            at_line_start = 0;
            continue;
        }

        // Skip whitespace within lines
        if (c == ' ' || c == '\t' || c == '\r')
        {
            i++;
            continue;
        }

        // Handle newlines
        if (c == '\n' && bracket_depth > 0)
        {
            line_num++; // Inside brackets: the expression continues on the next line.
            i++;
            continue;
        }
        if (c == '\n')
        {
            add_token(state, TOKEN_NEWLINE, NULL, line_num);
            line_num++;
            i++;
            at_line_start = 1;
            continue;
        }

        // --- Single Character Tokens ---
        if (c == '(')
        {
            bracket_depth++;
            add_token(state, TOKEN_LPAREN, "(", line_num);
            i++;
            continue;
        }
        if (c == ')')
        {
            if (bracket_depth > 0)
                bracket_depth--;
            add_token(state, TOKEN_RPAREN, ")", line_num);
            i++;
            continue;
        }
        if (c == '[')
        {
            bracket_depth++;
            add_token(state, TOKEN_LBRACKET, "[", line_num);
            i++;
            continue;
        }
        if (c == ']')
        {
            if (bracket_depth > 0)
                bracket_depth--;
            add_token(state, TOKEN_RBRACKET, "]", line_num);
            i++;
            continue;
        }
        if (c == '{')
        {
            bracket_depth++;
            add_token(state, TOKEN_LBRACE, "{", line_num);
            i++;
            continue;
        }
        if (c == '}')
        {
            if (bracket_depth > 0)
                bracket_depth--;
            add_token(state, TOKEN_RBRACE, "}", line_num);
            i++;
            continue;
        }
        if (c == ':')
        {
            add_token(state, TOKEN_COLON, ":", line_num);
            i++;
            continue;
        }
        if (c == ',')
        {
            add_token(state, TOKEN_COMMA, ",", line_num);
            i++;
            continue;
        }
        if (c == ';')
        {
            add_token(state, TOKEN_SEMICOLON, ";", line_num);
            i++;
            continue;
        }
        if (c == '.')
        {
            add_token(state, TOKEN_DOT, ".", line_num);
            i++;
            continue;
        }
        if (c == '+')
        {
            add_token(state, TOKEN_PLUS, "+", line_num);
            i++;
            continue;
        }
        if (c == '-')
        {
            add_token(state, TOKEN_MINUS, "-", line_num);
            i++;
            continue;
        }
        if (c == '*')
        {
            add_token(state, TOKEN_STAR, "*", line_num);
            i++;
            continue;
        }
        if (c == '/')
        {
            add_token(state, TOKEN_SLASH, "/", line_num);
            i++;
            continue;
        }
        if (c == '%')
        {
            add_token(state, TOKEN_PERCENT, "%", line_num);
            i++;
            continue;
        }
        if (c == '^')
        {
            add_token(state, TOKEN_CARET, "^", line_num);
            i++;
            continue;
        }

        // --- Multi-Character Operators ---
        if (c == '!')
        {
            if (source[i + 1] == '=')
            {
                add_token(state, TOKEN_NEQ, "!=", line_num);
                i += 2;
                continue;
            }
            add_token(state, TOKEN_BANG, "!", line_num);
            i++;
            continue;
        }
        if (c == '>')
        {
            if (source[i + 1] == '=')
            {
                add_token(state, TOKEN_GTE, ">=", line_num);
                i += 2;
                continue;
            }
            add_token(state, TOKEN_GT, ">", line_num);
            i++;
            continue;
        }
        if (c == '<')
        {
            if (source[i + 1] == '=')
            {
                add_token(state, TOKEN_LTE, "<=", line_num);
                i += 2;
                continue;
            }
            add_token(state, TOKEN_LT, "<", line_num);
            i++;
            continue;
        }
        if (c == '=')
        {
            if (source[i + 1] == '=')
            {
                add_token(state, TOKEN_EQ, "==", line_num);
                i += 2;
                continue;
            }
            add_token(state, TOKEN_ASSIGN, "=", line_num);
            i++;
            continue;
        }

        // --- Raw String Literals ---
        // r"..." keeps every character exactly as written (no escapes), which
        // suits regex patterns and Windows paths: r"\d+\.\d+", r"C:\Users".
        // To include a double quote, add hashes: r#"say "hi""# ends at the first
        // `"` followed by the same number of `#` (r##"..."## for "# inside, etc.).
        // The value is an ordinary string.
        if (c == 'r' && (source[i + 1] == '"' || source[i + 1] == '#'))
        {
            int hashes = 0;
            int j = i + 1;
            while (source[j] == '#')
            {
                hashes++;
                j++;
            }
            if (source[j] == '"')
            {
                int start_line = line_num;
                int start = j + 1;
                int end = start;
                for (;;)
                {
                    if (source[end] == '\0')
                        report_error(start_line, "Unterminated raw string literal.");
                    if (source[end] == '"')
                    {
                        int k = 0;
                        while (k < hashes && source[end + 1 + k] == '#')
                            k++;
                        if (k == hashes)
                            break;
                    }
                    if (source[end] == '\n')
                        line_num++; // Raw strings may span lines.
                    end++;
                }
                add_token_bytes(state, TOKEN_STRING, source + start, end - start, start_line);
                i = end + 1 + hashes;
                continue;
            }
            // Not a raw string (e.g. a name like `r`): fall through to identifiers.
        }

        // --- String Literals ---
        // Escapes: \n \t \r \0 \a \b \f \v \e \\ \" \' \xHH (one byte) and
        // \u{H...} (a Unicode code point, encoded as UTF-8). Any other escape is an
        // error, so a typo cannot silently become literal text.
        if (c == '"')
        {
            i++;
            int start_line = line_num; // A literal may span lines; the token belongs to its first.
            // Every escape produces no more bytes than its source text, so the
            // rest of the source is a safe upper bound for the literal's size.
            char *buf = malloc(strlen(source + i) + 1);
            int bpos = 0;
            while (source[i] != '\0' && source[i] != '"')
            {
                if (source[i] != '\\')
                {
                    if (source[i] == '\n')
                        line_num++; // Keep line numbers right for everything after the literal.
                    buf[bpos++] = source[i++];
                    continue;
                }
                i++; // skip backslash
                char esc = source[i];
                switch (esc) {
                    case 'n': buf[bpos++] = '\n'; i++; break;
                    case 't': buf[bpos++] = '\t'; i++; break;
                    case 'r': buf[bpos++] = '\r'; i++; break;
                    case '0': buf[bpos++] = '\0'; i++; break;
                    case 'a': buf[bpos++] = '\a'; i++; break;
                    case 'b': buf[bpos++] = '\b'; i++; break;
                    case 'f': buf[bpos++] = '\f'; i++; break;
                    case 'v': buf[bpos++] = '\v'; i++; break;
                    case 'e': buf[bpos++] = 27; i++; break; // ESC, for terminal colour codes
                    case '\\': buf[bpos++] = '\\'; i++; break;
                    case '"': buf[bpos++] = '"'; i++; break;
                    case '\'': buf[bpos++] = '\''; i++; break;
                    case 'x':
                    {
                        int high = hex_digit_value(source[i + 1]);
                        int low = high < 0 ? -1 : hex_digit_value(source[i + 2]);
                        if (low < 0)
                            report_error(line_num, "Invalid escape: \\x must be followed by exactly two hex digits.");
                        buf[bpos++] = (char) (high * 16 + low);
                        i += 3;
                        break;
                    }
                    case 'u':
                    {
                        if (source[i + 1] != '{')
                            report_error(line_num, "Invalid escape: expected \\u{...} with 1-6 hex digits.");
                        i += 2;
                        unsigned long code_point = 0;
                        int digits = 0;
                        while (hex_digit_value(source[i]) >= 0 && digits < 7)
                        {
                            code_point = code_point * 16 + (unsigned long) hex_digit_value(source[i]);
                            digits++;
                            i++;
                        }
                        if (source[i] != '}' || digits == 0 || digits > 6)
                            report_error(line_num, "Invalid escape: expected \\u{...} with 1-6 hex digits.");
                        if (code_point > 0x10FFFF || (code_point >= 0xD800 && code_point <= 0xDFFF))
                            report_error(line_num, "Invalid escape: \\u{%lX} is not a valid Unicode code point.",
                                         code_point);
                        i++; // skip '}'
                        bpos += encode_utf8(code_point, buf + bpos);
                        break;
                    }
                    case '\0':
                        report_error(line_num, "Unterminated string literal.");
                        break;
                    default:
                        report_error(line_num, "Unknown escape sequence '\\%c' in string literal.", esc);
                        break;
                }
            }
            if (source[i] != '"')
                report_error(start_line, "Unterminated string literal.");
            i++; // skip closing quote
            add_token_bytes(state, TOKEN_STRING, buf, bpos, start_line);
            free(buf);
            continue;
        }

        // --- Number Literals ---
        if (isdigit(c))
        {
            int start = i;
            int is_float = 0;
            while (isdigit(source[i]) || source[i] == '.')
            {
                if (source[i] == '.')
                    is_float = 1;
                i++;
            }
            // Optional exponent: 1e300, 2.5E-3. Only consumed when digits follow,
            // so `2e` is still the number 2 followed by the name `e`.
            if (source[i] == 'e' || source[i] == 'E')
            {
                int j = i + 1;
                if (source[j] == '+' || source[j] == '-')
                    j++;
                if (isdigit(source[j]))
                {
                    is_float = 1;
                    i = j;
                    while (isdigit(source[i]))
                        i++;
                }
            }
            int length = i - start;
            char *num_val = malloc(length + 1);
            strncpy(num_val, source + start, length);
            num_val[length] = '\0';
            if (is_float)
                add_token(state, TOKEN_FLOAT_LITERAL, num_val, line_num);
            else
                add_token(state, TOKEN_NUMBER, num_val, line_num);
            continue;
        }

        // --- Identifiers and Keywords ---
        if (isalpha(c))
        {
            int start = i;
            while (isalnum(source[i]) || source[i] == '_')
                i++;
            int length = i - start;
            char *word = malloc(length + 1);
            strncpy(word, source + start, length);
            word[length] = '\0';

            if (strcmp(word, "extends") == 0)
            {
                add_token(state, TOKEN_EXTENDS, "extends", line_num);
            }
            else if (is_keyword(word))
            {
                if (strcmp(word, "import") == 0)
                {
                    add_token(state, TOKEN_IMPORT, "import", line_num);
                }
                else
                {
                    add_token(state, TOKEN_KEYWORD, word, line_num);
                }
            }
            else
            {
                add_token(state, TOKEN_IDENTIFIER, word, line_num);
            }
            free(word);
            continue;
        }
        i++;
    }

    // Emit remaining DEDENT tokens at EOF
    while (indent_level > 0)
    {
        add_token(state, TOKEN_DEDENT, NULL, line_num);
        indent_level--;
    }
    add_token(state, TOKEN_EOF, NULL, line_num);
}

/**
 * @brief Frees memory associated with the tokenizer state.
 *
 * @param state The tokenizer state to free.
 */
void free_tokens(TokenizerState *state)
{
    if (state->tokens)
    {
        for (int i = 0; i < state->token_count; i++)
        {
            if (state->tokens[i].value)
            {
                free(state->tokens[i].value);
            }
        }
        free(state->tokens);
        state->tokens = NULL;
        state->token_count = 0;
        state->token_capacity = 0;
    }
}
