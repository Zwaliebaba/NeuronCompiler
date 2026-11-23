/* sym.h — string interning (Design/02 §3). Identifiers, file names and
 * section names are interned once into a session-wide table and handled as
 * `Sym` handles, so comparing two names is comparing two integers. */
#ifndef NC_SYM_H
#define NC_SYM_H

#include "arena.h"

/* A handle into a SymTable (R16). Index 0 is SYM_NONE, which no string
 * interns to; the empty string is an ordinary symbol. */
typedef struct Sym
{
  u32 index;
} Sym;

#define SYM_NONE ((Sym){0})

typedef struct SymTable SymTable; /* opaque: defined in sym.c */

/* The table's strings live in `_arena`, which must outlive it. */
[[nodiscard]] SymTable* SymTableCreate(Arena* _arena);
void SymTableDestroy(SymTable* _table);

/* Interns `_lengthBytes` bytes of `_text`, which need not be NUL-terminated
 * and may contain NUL bytes. Returns the same Sym for equal byte strings. */
Sym SymIntern(SymTable* _table, const char* _text, u32 _lengthBytes);
Sym SymInternCstr(SymTable* _table, const char* _text);

/* As SymIntern, with the hash already computed as HashBytes(_text,
 * _lengthBytes): the lexer hashes identifiers while it scans them (Design/03
 * §2), so it interns without a second pass over the bytes. */
Sym SymInternHashed(SymTable* _table, const char* _text, u32 _lengthBytes, u64 _hash);

/* The interned bytes, NUL-terminated; valid as long as the arena is. */
const char* SymText(const SymTable* _table, Sym _sym);
u32 SymLength(const SymTable* _table, Sym _sym);

/* Number of distinct symbols interned, excluding SYM_NONE. */
u32 SymCount(const SymTable* _table);

static inline bool SymEq(Sym _a, Sym _b)
{
  return _a.index == _b.index;
}

static inline bool SymIsNone(Sym _sym)
{
  return _sym.index == 0;
}

#endif
