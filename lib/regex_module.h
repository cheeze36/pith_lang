/**
 * @file regex_module.h
 * @brief Native `regex` module: regular expressions with guaranteed linear-time matching.
 *
 * @code
 *   import "regex"
 *
 *   Regex date = regex.compile(r"(?<year>\d{4})-(?<month>\d\d)-(?<day>\d\d)")
 *   Match m = date.find("released on 2026-10-23")
 *   if (m != void):
 *       print(m.text())               # 2026-10-23
 *       print(m.group("year"))        # 2026
 *       print(m.start())              # 12 (byte offset)
 *
 *   print(date.test("no date here"))                  # false
 *   print(date.replace("on 2026-10-23", "$day/$month/$year"))   # on 23/10/2026
 *   foreach (Match word in regex.compile(r"\w+").find_all("a bc def")):
 *       print(word.text())
 * @endcode
 *
 * ## Why linear time
 *
 * The engine is a Thompson NFA / Pike VM (the design used by RE2 and Go). It
 * runs in time proportional to (pattern size x input size) for every pattern,
 * so a hostile input cannot make a match take exponentially long ("ReDoS"),
 * which matters for patterns applied to web requests. The price is that
 * backreferences (\1 inside a pattern) and lookahead/lookbehind are not
 * supported (they cannot be matched in linear time).
 *
 * ## Syntax
 *
 *  - Literals; `.` (any character except newline; any character with (?s))
 *  - Classes: `[abc]`, `[a-z0-9_]`, `[^"]`, with `\d \w \s` (and `\D \W \S`) inside
 *  - Shorthands: `\d` digit, `\w` word character [A-Za-z0-9_], `\s` whitespace,
 *    and their negations `\D \W \S`
 *  - Quantifiers: `*  +  ?  {n}  {n,}  {n,m}`, lazy versions `*?  +?  ??  {n,m}?`
 *  - Groups: `( )` capturing, `(?: )` non-capturing, `(?<name> )` named
 *  - Alternation `|`; anchors `^ $` and word boundaries `\b \B`
 *  - Escapes: `\n \t \r \f \v \0 \xHH \u{H...}`, and `\` before any punctuation
 *  - Flags at the very start: `(?i)` case-insensitive (ASCII letters),
 *    `(?m)` multi-line (^ and $ also match at line breaks), `(?s)` `.` matches
 *    newlines; combinable as `(?im)`
 *
 * Patterns and subjects are UTF-8: `.` and classes match whole characters,
 * while all positions (start/end) are byte offsets, like string indexing.
 *
 * Results follow the usual leftmost-first rules (as in Perl, Python, Go). One
 * known difference from backtracking engines such as Python's: when a repeated
 * group can match the empty string, e.g. `(a*)*`, the group captures its last
 * non-empty iteration ("aaa"), as in Go and RE2, where Python reports "".
 * The overall match is the same.
 *
 * ## API
 *
 * Module functions:
 *  - `regex.compile(pattern)`  -> Regex. Invalid patterns raise an error that
 *                                 names the problem and its offset.
 *  - `regex.escape(text)`      -> string. Escapes every metacharacter so @p text
 *                                 matches literally (for embedding user input).
 *
 * Regex methods:
 *  - `test(text)`              -> bool. Does the pattern match anywhere?
 *  - `find(text)`              -> Match, or void if there is no match.
 *  - `find_all(text)`          -> list<Match>. All non-overlapping matches.
 *  - `replace(text, repl)`     -> string. Replaces every match; in @p repl, `$0`..`$9`,
 *                                 `${n}`, `$name` and `${name}` insert groups, `$$` is a `$`.
 *  - `split(text)`             -> list<string>. The pieces between matches.
 *  - `pattern()`               -> string; `group_count()` -> int.
 *
 * Match methods (`n` is a group number or a group name; 0 / omitted = whole match):
 *  - `text()`                  -> string. The matched text.
 *  - `group(n)`                -> string. A group's text ("" if it did not take part).
 *  - `start(n)`, `end(n)`      -> int. Byte offsets (-1 if the group did not take part).
 *  - `groups()`                -> list<string>. Groups 1..group_count().
 *  - `named()`                 -> map<string, string>. Named groups by name.
 */

#ifndef PITH_REGEX_MODULE_H
#define PITH_REGEX_MODULE_H

#include "value.h"

/**
 * @brief Registers the `regex` module and its Regex / Match types into @p modules.
 *
 * @param modules The interpreter's native-module registry.
 */
void regex_register_module(HashMap *modules);

#endif // PITH_REGEX_MODULE_H
