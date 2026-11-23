/* sym.c — string interning. */
#include "sym.h"

#include "hash.h"
#include "vec.h"

/* The map key is the string itself, by reference: the bytes live in the
 * arena copy that the symbol owns, so the map never holds a dangling key. */
typedef struct SymKey
{
  const char* text;
  u32 lengthBytes;
  u64 hash;
} SymKey;

static inline bool SymKeyEq(SymKey _a, SymKey _b)
{
  return _a.hash == _b.hash && _a.lengthBytes == _b.lengthBytes && memcmp(_a.text, _b.text, _a.lengthBytes) == 0;
}

#define HMAP_NAME SymKeyMap
#define HMAP_K SymKey
#define HMAP_V u32
#define HMAP_HASH(k) ((k).hash)
#define HMAP_EQ(a, b) SymKeyEq((a), (b))
#include "hmap.inc"

typedef struct SymEntry
{
  const char* text;
  u32 lengthBytes;
} SymEntry;

struct SymTable
{
  Arena* arena;
  SymKeyMap byText;
  VEC(SymEntry) entries; /* indexed by Sym.index; entry 0 is SYM_NONE */
};

SymTable* SymTableCreate(Arena* _arena)
{
  SymTable* t = ARENA_NEW(_arena, SymTable);
  t->arena = _arena;
  VEC_PUSH(&t->entries, ((SymEntry){.text = "", .lengthBytes = 0}));
  return t;
}

void SymTableDestroy(SymTable* _table)
{
  SymKeyMapFree(&_table->byText);
  VEC_FREE(&_table->entries);
}

Sym SymInternHashed(SymTable* _table, const char* _text, u32 _lengthBytes, u64 _hash)
{
  SymKey key = {.text = _text, .lengthBytes = _lengthBytes, .hash = _hash};
  const u32* found = SymKeyMapGet(&_table->byText, key);
  if (found)
    return (Sym){*found};
  NC_ASSERT(_table->entries.len < UINT32_MAX);
  char* copy = ArenaAlloc(_table->arena, (size_t)_lengthBytes + 1, 1);
  memcpy(copy, _text, _lengthBytes);
  copy[_lengthBytes] = 0;
  u32 index = _table->entries.len;
  VEC_PUSH(&_table->entries, ((SymEntry){.text = copy, .lengthBytes = _lengthBytes}));
  key.text = copy;
  SymKeyMapPut(&_table->byText, key, index);
  return (Sym){index};
}

Sym SymIntern(SymTable* _table, const char* _text, u32 _lengthBytes)
{
  return SymInternHashed(_table, _text, _lengthBytes, HashBytes(_text, _lengthBytes));
}

Sym SymInternCstr(SymTable* _table, const char* _text)
{
  size_t n = strlen(_text);
  NC_ASSERT(n < UINT32_MAX);
  return SymIntern(_table, _text, (u32)n);
}

const char* SymText(const SymTable* _table, Sym _sym)
{
  NC_ASSERT(_sym.index < _table->entries.len);
  return _table->entries.data[_sym.index].text;
}

u32 SymLength(const SymTable* _table, Sym _sym)
{
  NC_ASSERT(_sym.index < _table->entries.len);
  return _table->entries.data[_sym.index].lengthBytes;
}

u32 SymCount(const SymTable* _table)
{
  return _table->entries.len - 1;
}
