/* buf.h — growable little-endian byte buffer, used for section contents,
 * unwind records and debug records. */
#ifndef NC_BUF_H
#define NC_BUF_H

#include "core.h"

typedef struct Buf
{
  u8* data;
  u32 len;
  u32 cap;
} Buf;

void BufFree(Buf* _b);
void BufReserve(Buf* _b, u32 _need);
void BufBytes(Buf* _b, const void* _p, u32 _n);
void BufZeros(Buf* _b, u32 _n);
void BufAlign(Buf* _b, u32 _align, u8 _fill);
void BufStr0(Buf* _b, const char* _s); /* string including NUL */

static inline void BufU8(Buf* _b, u8 _v)
{
  BufBytes(_b, &_v, 1);
}
static inline void BufU16(Buf* _b, u16 _v)
{
  u8 t[2] = {(u8)_v, (u8)(_v >> 8)};
  BufBytes(_b, t, 2);
}
static inline void BufU32(Buf* _b, u32 _v)
{
  u8 t[4] = {(u8)_v, (u8)(_v >> 8), (u8)(_v >> 16), (u8)(_v >> 24)};
  BufBytes(_b, t, 4);
}
static inline void BufU64(Buf* _b, u64 _v)
{
  BufU32(_b, (u32)_v);
  BufU32(_b, (u32)(_v >> 32));
}

static inline void BufPutU16(Buf* _b, u32 _at, u16 _v)
{
  NC_ASSERT(_at + 2 <= _b->len);
  _b->data[_at] = (u8)_v;
  _b->data[_at + 1] = (u8)(_v >> 8);
}
static inline void BufPutU32(Buf* _b, u32 _at, u32 _v)
{
  NC_ASSERT(_at + 4 <= _b->len);
  for (int i = 0; i < 4; i++)
    _b->data[_at + i] = (u8)(_v >> (8 * i));
}
static inline u32 BufGetU32(const Buf* _b, u32 _at)
{
  NC_ASSERT(_at + 4 <= _b->len);
  return (u32)_b->data[_at] | (u32)_b->data[_at + 1] << 8 | (u32)_b->data[_at + 2] << 16 | (u32)_b->data[_at + 3] << 24;
}

static inline u32 BufReadU32(const u8* _p)
{
  return (u32)_p[0] | (u32)_p[1] << 8 | (u32)_p[2] << 16 | (u32)_p[3] << 24;
}
static inline u16 BufReadU16(const u8* _p)
{
  return (u16)(_p[0] | _p[1] << 8);
}

#endif
