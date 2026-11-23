#include "arena.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* Chunks are 64 KiB unless a single allocation needs more. A later version
 * maps chunks with VirtualAlloc/mmap directly; malloc is sufficient now. */
enum
{
  ARENA_CHUNK_BYTES = 64 * 1024
};

struct ArenaChunk
{
  ArenaChunk* prev;
  u8* end;
  alignas(16) u8 data[];
};

void ArenaInit(Arena* _a)
{
  memset(_a, 0, sizeof *_a);
}

void ArenaRelease(Arena* _a)
{
  ArenaChunk* c = _a->head;
  while (c)
  {
    ArenaChunk* p = c->prev;
    free(c);
    c = p;
  }
  memset(_a, 0, sizeof *_a);
}

static void ArenaGrow(Arena* _a, size_t _need)
{
  size_t size = NC_MAX((size_t)ARENA_CHUNK_BYTES, _need + 16);
  ArenaChunk* c = malloc(sizeof(ArenaChunk) + size);
  if (!c)
    NcFatal("out of memory (arena chunk of %zu bytes)", size);
  c->prev = _a->head;
  c->end = c->data + size;
  _a->head = c;
  _a->cur = c->data;
  _a->end = c->end;
  _a->bytesReserved += size;
}

void* ArenaAlloc(Arena* _a, size_t _size, size_t _align)
{
  NC_ASSERT(NC_IS_POW2(_align));
  if (_size == 0)
    _size = 1;
  for (;;)
  {
    if (_a->cur)
    {
      /* Pad by pointer arithmetic, not by converting an integer back
             * into a pointer, which loses provenance. */
      size_t pad = (size_t)(-(uintptr_t)_a->cur) & (_align - 1);
      if (pad + _size <= (size_t)(_a->end - _a->cur))
      {
        u8* p = _a->cur + pad;
        _a->cur = p + _size;
        return p;
      }
    }
    ArenaGrow(_a, _size + _align);
  }
}

void* ArenaZalloc(Arena* _a, size_t _size, size_t _align)
{
  void* p = ArenaAlloc(_a, _size, _align);
  memset(p, 0, _size);
  return p;
}

char* ArenaStrndup(Arena* _a, const char* _s, size_t _n)
{
  char* d = ArenaAlloc(_a, _n + 1, 1);
  memcpy(d, _s, _n);
  d[_n] = 0;
  return d;
}

char* ArenaStrdup(Arena* _a, const char* _s)
{
  return ArenaStrndup(_a, _s, strlen(_s));
}

char* ArenaPrintf(Arena* _a, const char* _fmt, ...)
{
  va_list ap, ap2;
  va_start(ap, _fmt);
  va_copy(ap2, ap);
  int n = vsnprintf(nullptr, 0, _fmt, ap);
  va_end(ap);
  NC_ASSERT(n >= 0);
  char* d = ArenaAlloc(_a, (size_t)n + 1, 1);
  vsnprintf(d, (size_t)n + 1, _fmt, ap2);
  va_end(ap2);
  return d;
}

ArenaMark ArenaGetMark(const Arena* _a)
{
  return (ArenaMark){_a->head, _a->cur};
}

void ArenaReset(Arena* _a, ArenaMark _m)
{
  while (_a->head != _m.head)
  {
    ArenaChunk* p = _a->head->prev;
    _a->bytesReserved -= (size_t)(_a->head->end - _a->head->data);
    free(_a->head);
    _a->head = p;
  }
  _a->cur = _m.cur;
  _a->end = _a->head ? _a->head->end : nullptr;
}
