/**
 * @file resolver.h
 * @brief Static variable-resolution pass.
 *
 * Walks the AST once before interpretation and assigns every function-local
 * variable reference a (depth, slot) location, and every function a frame size.
 * This lets the interpreter index local variables by array slot instead of
 * walking a linked list and comparing names on every access.
 *
 * Globals, module members, natives, and all top-level code are left to the
 * interpreter's name-based environment; the resolver only annotates locals.
 */

#ifndef PITH_RESOLVER_H
#define PITH_RESOLVER_H

#include "parser.h"

/**
 * @brief Resolves variable references for a whole program (or module) AST.
 *
 * Idempotent and safe to run on any AST produced by the parser, including the
 * per-statement ASTs built by the REPL and the ASTs of imported modules.
 *
 * @param root The root AST node to resolve (may be NULL).
 */
void resolve_program(ASTNode *root);

#endif //PITH_RESOLVER_H
