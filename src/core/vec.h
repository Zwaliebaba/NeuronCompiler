/* vec.h — typed growable arrays.
 *
 *   typedef VEC(MirInst) MirInstVec;
 *   MirInstVec v = {};
 *   VEC_PUSH(&v, inst);
 *   for (u32 i = 0; i < v.len; i++) use(v.data[i]);
 *   VEC_FREE(&v);
 *
 * Vectors are heap-backed (not arena) because they grow; they are owned by
 * an arena-allocated structure and freed with it explicitly. */
#ifndef NC_VEC_H
#define NC_VEC_H

#include "core.h"

#define VEC(T) \
  struct       \
  {            \
    T* data;   \
    u32 len;   \
    u32 cap;   \
  }

void VecGrow(void** _data, u32* _cap, u32 _need, size_t _elemSize);

#define VEC_RESERVE(v, n) VecGrow((void**)&(v)->data, &(v)->cap, (n), sizeof *(v)->data)

#define VEC_PUSH(v, x)              \
  do                                \
  {                                 \
    VEC_RESERVE((v), (v)->len + 1); \
    (v)->data[(v)->len++] = (x);    \
  } while (0)

/* Appends a zeroed element and returns a pointer to it. */
#define VEC_PUSH_ZERO(v) (VEC_RESERVE((v), (v)->len + 1), memset(&(v)->data[(v)->len], 0, sizeof *(v)->data), &(v)->data[(v)->len++])

#define VEC_LAST(v) (&(v)->data[(v)->len - 1])
#define VEC_CLEAR(v) ((v)->len = 0)
#define VEC_FREE(v) (NcXfree((void*)(v)->data), (v)->data = nullptr, (v)->len = (v)->cap = 0)

#endif
