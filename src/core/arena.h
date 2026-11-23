/* arena.h — region allocator. All long-lived compiler data is arena-owned;
 * there is no individual free (Design/02-architecture.md §3). */
#ifndef NC_ARENA_H
#define NC_ARENA_H

#include "core.h"

typedef struct ArenaChunk ArenaChunk;

typedef struct Arena
{
  ArenaChunk* head;
  u8* cur;
  u8* end;
  size_t bytesReserved;
} Arena;

typedef struct ArenaMark
{
  ArenaChunk* head;
  u8* cur;
} ArenaMark;

void ArenaInit(Arena* _a);
void ArenaRelease(Arena* _a);
void* ArenaAlloc(Arena* _a, size_t _size, size_t _align);
void* ArenaZalloc(Arena* _a, size_t _size, size_t _align);
char* ArenaStrdup(Arena* _a, const char* _s);
char* ArenaStrndup(Arena* _a, const char* _s, size_t _n);
char* ArenaPrintf(Arena* _a, const char* _fmt, ...);

ArenaMark ArenaGetMark(const Arena* _a);
void ArenaReset(Arena* _a, ArenaMark _m);

#define ARENA_NEW(a, T) ((T*)ArenaZalloc((a), sizeof(T), alignof(T)))
#define ARENA_NEW_ARRAY(a, T, n) ((T*)ArenaZalloc((a), sizeof(T) * (size_t)(n), alignof(T)))

#endif
