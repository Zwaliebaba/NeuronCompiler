/* compiler.h — the only place compiler sources reach a compiler extension
 * (AGENTS.md R12, Design/13 §1).
 *
 * NC-Boot forbids extensions in compiler sources so that every stage-0
 * compiler and, once it self-hosts, ncc itself accepts the whole tree. What
 * the tree cannot do without is named here, once, so that a new host compiler
 * (or ncc) has exactly one file to provide. */
#ifndef NC_COMPILER_H
#define NC_COMPILER_H

/* Bit counting on a NON-ZERO operand: the result for zero is undefined, as it
 * is for the builtins. core.h wraps them in total functions (NcClz64 etc.). */
#define NC_CLZ32_NONZERO(x) ((unsigned)__builtin_clz(x))
#define NC_CLZ64_NONZERO(x) ((unsigned)__builtin_clzll(x))
#define NC_CTZ64_NONZERO(x) ((unsigned)__builtin_ctzll(x))
#define NC_POPCOUNT64(x) ((unsigned)__builtin_popcountll(x))

/* Hand-written Win32 declarations in the OS layer (AGENTS.md §2: the
 * compiler does not include windows.h). */
#if defined(_WIN32)
#   define NC_DLLIMPORT __declspec(dllimport)
#   define NC_WINAPI __stdcall
#endif

#endif
