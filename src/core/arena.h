/* arena.h — region allocator. All long-lived compiler data is arena-owned;
 * there is no individual free (Design/02-architecture.md §3). */
#ifndef NC_ARENA_H
#define NC_ARENA_H

#include "core.h"

typedef struct ArenaChunk ArenaChunk;

typedef struct Arena {
    ArenaChunk *head;
    u8 *cur;
    u8 *end;
    size_t bytes_reserved;
} Arena;

typedef struct ArenaMark {
    ArenaChunk *head;
    u8 *cur;
} ArenaMark;

void  arena_init(Arena *a);
void  arena_release(Arena *a);
void *arena_alloc(Arena *a, size_t size, size_t align);
void *arena_zalloc(Arena *a, size_t size, size_t align);
char *arena_strdup(Arena *a, const char *s);
char *arena_strndup(Arena *a, const char *s, size_t n);
char *arena_printf(Arena *a, const char *fmt, ...);

ArenaMark arena_mark(const Arena *a);
void      arena_reset(Arena *a, ArenaMark m);

#define ARENA_NEW(a, T) ((T *)arena_zalloc((a), sizeof(T), alignof(T)))
#define ARENA_NEW_ARRAY(a, T, n) ((T *)arena_zalloc((a), sizeof(T) * (size_t)(n), alignof(T)))

#endif
