/* buf.h — growable little-endian byte buffer, used for section contents,
 * unwind records and debug records. */
#ifndef NC_BUF_H
#define NC_BUF_H

#include "core.h"

typedef struct Buf {
    u8 *data;
    u32 len;
    u32 cap;
} Buf;

void buf_free(Buf *b);
void buf_reserve(Buf *b, u32 need);
void buf_bytes(Buf *b, const void *p, u32 n);
void buf_zeros(Buf *b, u32 n);
void buf_align(Buf *b, u32 align, u8 fill);
void buf_str0(Buf *b, const char *s); /* string including NUL */

static inline void buf_u8(Buf *b, u8 v) { buf_bytes(b, &v, 1); }
static inline void buf_u16(Buf *b, u16 v) { u8 t[2] = {(u8)v, (u8)(v >> 8)}; buf_bytes(b, t, 2); }
static inline void buf_u32(Buf *b, u32 v) {
    u8 t[4] = {(u8)v, (u8)(v >> 8), (u8)(v >> 16), (u8)(v >> 24)};
    buf_bytes(b, t, 4);
}
static inline void buf_u64(Buf *b, u64 v) { buf_u32(b, (u32)v); buf_u32(b, (u32)(v >> 32)); }

static inline void buf_put_u16(Buf *b, u32 at, u16 v) {
    NC_ASSERT(at + 2 <= b->len);
    b->data[at] = (u8)v; b->data[at + 1] = (u8)(v >> 8);
}
static inline void buf_put_u32(Buf *b, u32 at, u32 v) {
    NC_ASSERT(at + 4 <= b->len);
    for (int i = 0; i < 4; i++) b->data[at + i] = (u8)(v >> (8 * i));
}
static inline u32 buf_get_u32(const Buf *b, u32 at) {
    NC_ASSERT(at + 4 <= b->len);
    return (u32)b->data[at] | (u32)b->data[at + 1] << 8 | (u32)b->data[at + 2] << 16 |
           (u32)b->data[at + 3] << 24;
}

static inline u32 rd_u32(const u8 *p) {
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}
static inline u16 rd_u16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }

#endif
