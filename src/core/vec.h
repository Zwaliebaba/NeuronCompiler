/* vec.h — typed growable arrays.
 *
 *   typedef VEC(MInst) MInstVec;
 *   MInstVec v = {};
 *   vec_push(&v, inst);
 *   for (u32 i = 0; i < v.len; i++) use(v.data[i]);
 *   vec_free(&v);
 *
 * Vectors are heap-backed (not arena) because they grow; they are owned by
 * an arena-allocated structure and freed with it explicitly. */
#ifndef NC_VEC_H
#define NC_VEC_H

#include "core.h"

#define VEC(T) struct { T *data; u32 len; u32 cap; }

void vec__reserve(void **data, u32 *cap, u32 need, size_t elem_size);

#define vec_reserve(v, n) \
    vec__reserve((void **)&(v)->data, &(v)->cap, (n), sizeof *(v)->data)

#define vec_push(v, x)                                \
    do {                                              \
        vec_reserve((v), (v)->len + 1);               \
        (v)->data[(v)->len++] = (x);                  \
    } while (0)

/* Appends a zeroed element and returns a pointer to it. */
#define vec_push_zero(v)                                                    \
    (vec_reserve((v), (v)->len + 1),                                        \
     memset(&(v)->data[(v)->len], 0, sizeof *(v)->data), &(v)->data[(v)->len++])

#define vec_last(v) (&(v)->data[(v)->len - 1])
#define vec_clear(v) ((v)->len = 0)
#define vec_free(v) (nc_xfree((v)->data), (v)->data = nullptr, (v)->len = (v)->cap = 0)

#endif
