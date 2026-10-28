/**
 * @file template_module.h
 * @brief Native `template` module: HTML templates with typed, sandboxed expressions.
 *
 * The syntax is a strict, typed dialect of Handlebars (https://handlebarsjs.com),
 * whose tag style it borrows: explicit {{#if}} / {{#each}} blocks, comparisons
 * written directly in conditions, Pith-style typing (conditions must be bool,
 * unknown names are errors, never silently empty), and helpers that are
 * ordinary typed Pith functions.
 *
 * @code
 *   import "template"
 *
 *   define string money(int cents):
 *       return "$" + str(cents / 100) + "." + str(cents % 100)
 *
 *   Template page = template.load("views/users.html")
 *   page.helper("money", money)
 *   string html = page.render({"title": "Users", "users": users})
 * @endcode
 *
 * views/users.html:
 * @code
 *   {{> header}}
 *   <h1>{{title}}</h1>
 *   {{#each users as user}}
 *     <li class="{{#if @index % 2 == 0}}even{{else}}odd{{/if}}">
 *       {{user.name}}
 *       {{#if user.age < 18}}<em>(minor)</em>{{/if}}
 *       {{money user.balance_cents}}
 *     </li>
 *   {{else}}
 *     <p>No users yet.</p>
 *   {{/each}}
 * @endcode
 *
 * ## Tags
 *
 * | Tag                                   | Meaning                                               |
 * |---------------------------------------|-------------------------------------------------------|
 * | `{{ expression }}`                    | Output the value, HTML-escaped                        |
 * | `{{raw expression}}`                  | Output without escaping (only for trusted HTML)       |
 * | `{{#if c}} .. {{else if c}} .. {{else}} .. {{/if}}` | Conditional; each `c` must be a bool    |
 * | `{{#each list as item}} .. {{else}} .. {{/each}}`   | Loop over a list (or a map's keys); the `{{else}}` part renders when it is empty |
 * | `@index`, `@first`, `@last`           | Inside {{#each}}: position (int) and first/last (bool) |
 * | `{{helper arg1 arg2}}`                | Call a Pith function registered with helper()         |
 * | `{{> name}}`                          | Include another template file (a "partial")           |
 * | `{{! comment }}`                      | Comment; produces nothing                             |
 * | `\{{`                                 | A literal "{{"                                        |
 *
 * A block tag ({{#..}}, {{else}}, {{/..}}, {{!..}}, {{>..}}) that is alone on its
 * line removes that whole line from the output, so block structure does not
 * leave blank lines behind.
 *
 * ## Expressions (a safe subset of Pith)
 *
 *  - names: values from the data map, and loop variables
 *  - `a.b` (map key or object field), `a[i]` (list / string index, map key),
 *    `a.len()` (string, list, map, buffer)
 *  - literals: `18`, `2.5`, `"text"` or `'text'`, `true`, `false`, `void`
 *  - operators, with exactly Pith's rules: `+ - * / %`, `== != < > <= >=`,
 *    `and`, `or` (short-circuit), `!`, unary `-`, parentheses
 *  - helper calls: `name arg arg ...` at the start of a tag or inside
 *    parentheses (a "subexpression": `{{shout (str count) 2}}`); each argument
 *    is a name, literal, `a.b` / `a[i]` access, or a parenthesized expression
 *
 * Templates cannot assign, import, or call anything except registered helpers,
 * so a template cannot touch files, the database or other program state.
 *
 * ## Typing (errors, not silent output)
 *
 *  - An {{#if}} condition must be a bool: `{{#if user.age}}` is an error.
 *  - {{#each}} needs a list or a map; anything else is an error.
 *  - Unknown names, missing map keys and missing fields are errors.
 *  - Outputting void, a list or a map is an error.
 *  - Helper arguments and results are checked like any Pith call.
 *
 * Every error names the template file, line and column, e.g.
 * `views/users.html:9:7: 'user' has no key 'nmae'`. Syntax errors are found when
 * the template is loaded; the rest when it is rendered.
 *
 * ## Module functions
 *
 *  - `template.load(path)`     -> Template. Compiles a template file; partials
 *                                  are found next to it ({{> header}} in
 *                                  views/page.html loads views/header.html; a
 *                                  name without an extension gets the including
 *                                  file's extension).
 *  - `template.compile(text)`  -> Template from a string (partials are looked up
 *                                  in the current directory).
 *  - `template.escape(text)`   -> string. HTML-escapes text, as {{ }} does.
 *
 * ## Template methods
 *
 *  - `render(data)`            -> string. @c data is a map whose keys become the
 *                                  template's top-level names.
 *  - `helper(name, function)`  Register a Pith function callable as {{name ...}}.
 */

#ifndef PITH_TEMPLATE_MODULE_H
#define PITH_TEMPLATE_MODULE_H

#include "value.h"

/**
 * @brief Registers the `template` module and its Template type into @p modules.
 *
 * @param modules The interpreter's native-module registry.
 */
void template_register_module(HashMap *modules);

#endif // PITH_TEMPLATE_MODULE_H
