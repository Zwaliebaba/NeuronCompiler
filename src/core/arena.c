#include "arena.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* Chunks are 64 KiB unless a single allocation needs more. A later version
 * maps chunks with VirtualAlloc/mmap directly; malloc is sufficient now. */
enum { ARENA_CHUNK_SIZE = 64 * 1024 };

struct ArenaChunk {
    ArenaChunk *prev;
    u8 *end;
    alignas(16) u8 data[];
};

void arena_init(Arena *a) { memset(a, 0, sizeof *a); }

void arena_release(Arena *a) {
    ArenaChunk *c = a->head;
    while (c) {
        ArenaChunk *p = c->prev;
        free(c);
        c = p;
    }
    memset(a, 0, sizeof *a);
}

static void arena_grow(Arena *a, size_t need) {
    size_t size = NC_MAX((size_t)ARENA_CHUNK_SIZE, need + 16);
    ArenaChunk *c = malloc(sizeof(ArenaChunk) + size);
    if (!c) nc_fatal("out of memory (arena chunk of %zu bytes)", size);
    c->prev = a->head;
    c->end = c->data + size;
    a->head = c;
    a->cur = c->data;
    a->end = c->end;
    a->bytes_reserved += size;
}

void *arena_alloc(Arena *a, size_t size, size_t align) {
    NC_ASSERT(NC_IS_POW2(align));
    if (size == 0) size = 1;
    for (;;) {
        if (a->cur) {
            uintptr_t p = ((uintptr_t)a->cur + (align - 1)) & ~(uintptr_t)(align - 1);
            if (p + size <= (uintptr_t)a->end) {
                a->cur = (u8 *)(p + size);
                return (void *)p;
            }
        }
        arena_grow(a, size + align);
    }
}

void *arena_zalloc(Arena *a, size_t size, size_t align) {
    void *p = arena_alloc(a, size, align);
    memset(p, 0, size);
    return p;
}

char *arena_strndup(Arena *a, const char *s, size_t n) {
    char *d = arena_alloc(a, n + 1, 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

char *arena_strdup(Arena *a, const char *s) { return arena_strndup(a, s, strlen(s)); }

char *arena_printf(Arena *a, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);
    NC_ASSERT(n >= 0);
    char *d = arena_alloc(a, (size_t)n + 1, 1);
    vsnprintf(d, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return d;
}

ArenaMark arena_mark(const Arena *a) { return (ArenaMark){a->head, a->cur}; }

void arena_reset(Arena *a, ArenaMark m) {
    while (a->head != m.head) {
        ArenaChunk *p = a->head->prev;
        a->bytes_reserved -= (size_t)(a->head->end - a->head->data);
        free(a->head);
        a->head = p;
    }
    a->cur = m.cur;
    a->end = a->head ? a->head->end : nullptr;
}
