/**
 * @file resolver.c
 * @brief Implementation of the static variable-resolution pass.
 *
 * For each function (top-level function or class method) the resolver tracks the
 * local variables currently in scope and assigns each a unique slot in that
 * function's activation frame. Variable references are annotated as either:
 *   - local: `is_local = 1`, with `var_depth` (enclosing-function hops) and
 *     `var_slot` (index in that frame); or
 *   - global/by-name: `is_local = 0` (the interpreter resolves it by name).
 *
 * Block scoping (if/while/for/foreach bodies, explicit blocks) is handled purely
 * at resolve time: a name declared inside a block stops resolving once the block
 * ends, but slots are never reused, so a function's frame size is simply the
 * total number of locals it declares.
 */

#include "resolver.h"
#include "types.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Maximum number of locals that may be simultaneously in scope within a single
// function. Generous; overflow aborts rather than silently miscompiling.
#define MAX_LOCALS 4096

/**
 * @brief Resolver state for one function being resolved.
 *
 * `names`/`slots`/`levels` form a stack of the locals currently in scope, most
 * recently declared last (so lookup scans backwards to honor shadowing).
 */
typedef struct RFunction
{
    struct RFunction *parent; // Lexically enclosing function (for closures), or NULL.
    const char *names[MAX_LOCALS]; // Name of each in-scope local.
    int slots[MAX_LOCALS]; // Frame slot assigned to each.
    int levels[MAX_LOCALS]; // Block-nesting level each was declared at.
    int count; // Number of entries currently in scope.
    int block_level; // Current block-nesting depth.
    int next_slot; // Next free frame slot (monotonic).
    TypeSpec **slot_types; // Declared type of every slot assigned so far (grows with next_slot).
    int slot_types_capacity;
} RFunction;

static void resolve(ASTNode *node, RFunction *fn);

/**
 * @brief Declares a local in the current block of `fn` and returns its slot.
 *
 * @param type The local's declared type (NULL = untyped), recorded per slot so
 *        the interpreter can check assignments to it.
 */
static int declare_local(RFunction *fn, const char *name, TypeSpec *type)
{
    if (fn->count >= MAX_LOCALS)
    {
        fprintf(stderr, "Fatal: too many local variables in one function (max %d).\n", MAX_LOCALS);
        exit(1);
    }
    int slot = fn->next_slot++;
    if (slot >= fn->slot_types_capacity)
    {
        fn->slot_types_capacity = fn->slot_types_capacity == 0 ? 16 : fn->slot_types_capacity * 2;
        fn->slot_types = realloc(fn->slot_types, sizeof(TypeSpec *) * (size_t) fn->slot_types_capacity);
    }
    fn->slot_types[slot] = type;
    fn->names[fn->count] = name;
    fn->slots[fn->count] = slot;
    fn->levels[fn->count] = fn->block_level;
    fn->count++;
    return slot;
}

/**
 * @brief Looks up `name` in `fn` and its enclosing functions.
 *
 * @param fn Innermost function scope (NULL at top level).
 * @param name Variable name to find.
 * @param out_depth Set to the number of enclosing-function hops if found.
 * @param out_slot  Set to the frame slot if found.
 * @return 1 if the name is a local (in any enclosing function), 0 otherwise.
 */
static int lookup_local(RFunction *fn, const char *name, int *out_depth, int *out_slot)
{
    int depth = 0;
    for (RFunction *f = fn; f != NULL; f = f->parent, depth++)
    {
        for (int i = f->count - 1; i >= 0; i--)
        {
            if (strcmp(f->names[i], name) == 0)
            {
                *out_depth = depth;
                *out_slot = f->slots[i];
                return 1;
            }
        }
    }
    return 0;
}

static void begin_block(RFunction *fn)
{
    fn->block_level++;
}

static void end_block(RFunction *fn)
{
    // Drop locals that belong to the block we are leaving. Slots are not reclaimed.
    while (fn->count > 0 && fn->levels[fn->count - 1] == fn->block_level)
        fn->count--;
    fn->block_level--;
}

/**
 * @brief Resolves a function body, assigning slots to `this` (methods), params,
 *        and locals, and records the frame size on the function-definition node.
 *
 * @param func_node The AST_FUNC_DEF node.
 * @param parent Enclosing function scope (NULL for top-level/method definitions).
 * @param this_type For methods, the class type of the implicit `this` receiver
 *        (reserved as slot 0); NULL for plain functions.
 */
static void resolve_function(ASTNode *func_node, RFunction *parent, TypeSpec *this_type)
{
    RFunction fn;
    fn.parent = parent;
    fn.count = 0;
    fn.block_level = 0;
    fn.next_slot = 0;
    fn.slot_types = NULL;
    fn.slot_types_capacity = 0;

    // Slot layout must match how the interpreter's call sites fill the frame:
    // `this` first (methods only), then declared parameters in order, then the
    // body's locals.
    if (this_type)
        declare_local(&fn, "this", this_type);
    for (int i = 0; i < func_node->arg_count; i++)
        declare_local(&fn, func_node->args[i], func_node->arg_types ? func_node->arg_types[i] : NULL);

    // The body is the function's block (children[0]); resolve its statements.
    if (func_node->children_count > 0)
        resolve(func_node->children[0], &fn);

    func_node->frame_slots = fn.next_slot;
    free(func_node->slot_types); // Re-resolving (e.g. in the REPL) replaces the table.
    func_node->slot_types = fn.slot_types;
}

/**
 * @brief Core recursive resolution.
 * @param node The node to resolve.
 * @param fn Current function scope, or NULL at top level (globals).
 */
static void resolve(ASTNode *node, RFunction *fn)
{
    if (!node)
        return;

    switch (node->type)
    {
        case AST_FUNC_DEF:
            // A top-level/nested function definition. Its name is bound by the
            // interpreter (globally, or as a local for a nested def); here we only
            // resolve the body as a new function scope.
            resolve_function(node, fn, NULL);
            return;

        case AST_CLASS_DEF:
            // Resolve each method body as a function with an implicit `this`.
            for (int i = 0; i < node->children_count; i++)
            {
                ASTNode *child = node->children[i];
                if (child->type == AST_FUNC_DEF)
                    resolve_function(child, fn, type_from_name(node->value));
            }
            return;

        case AST_VAR_DECL:
            // Resolve the initializer before the name is in scope, so `int x = x`
            // refers to any outer/global x, matching sequential semantics.
            for (int i = 0; i < node->children_count; i++)
                resolve(node->children[i], fn);
            if (fn)
            {
                node->is_local = 1;
                node->var_depth = 0;
                node->var_slot = declare_local(fn, node->value, node->type_spec);
            }
            else
            {
                node->is_local = 0; // Top-level declaration => global.
            }
            return;

        case AST_VAR_REF:
        {
            int depth, slot;
            if (fn && lookup_local(fn, node->value, &depth, &slot))
            {
                node->is_local = 1;
                node->var_depth = depth;
                node->var_slot = slot;
            }
            else
            {
                node->is_local = 0; // Global / by-name.
            }
            return;
        }

        case AST_FOREACH:
            // children[0] = collection, children[1] = body. The loop variable is
            // scoped to the loop.
            resolve(node->children[0], fn);
            if (fn)
            {
                begin_block(fn);
                node->is_local = 1;
                node->var_depth = 0;
                node->var_slot = declare_local(fn, node->value, node->type_spec);
            }
            else
            {
                node->is_local = 0;
            }
            if (node->children_count > 1)
                resolve(node->children[1], fn);
            if (fn)
                end_block(fn);
            return;

        case AST_RESCUE:
            // value = error variable (may be NULL), children[0] = handler block.
            // Like a foreach variable, the error variable is scoped to the handler.
            if (fn)
                begin_block(fn);
            if (node->value && fn)
            {
                node->is_local = 1;
                node->var_depth = 0;
                node->var_slot = declare_local(fn, node->value, type_from_name("string")); // error message
            }
            else
            {
                node->is_local = 0;
            }
            if (node->children_count > 0)
                resolve(node->children[0], fn);
            if (fn)
                end_block(fn);
            return;

        case AST_FOR:
            // children: 0 = init, 1 = cond, 2 = incr, 3 = body. The init variable
            // is scoped to the loop.
            if (fn)
                begin_block(fn);
            for (int i = 0; i < node->children_count; i++)
                resolve(node->children[i], fn);
            if (fn)
                end_block(fn);
            return;

        case AST_BLOCK:
            if (fn)
                begin_block(fn);
            for (int i = 0; i < node->children_count; i++)
                resolve(node->children[i], fn);
            if (fn)
                end_block(fn);
            return;

        default:
            // All other nodes: recurse into children. (Operators, calls, args,
            // field/index access, if/while/switch bodies — the body blocks are
            // AST_BLOCK nodes handled above, giving them their own scope.)
            for (int i = 0; i < node->children_count; i++)
                resolve(node->children[i], fn);
            return;
    }
}

void resolve_program(ASTNode *root)
{
    if (!root)
        return;
    // Top-level: fn == NULL, so all declarations and references are global.
    for (int i = 0; i < root->children_count; i++)
        resolve(root->children[i], NULL);
}
