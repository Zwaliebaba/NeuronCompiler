/* core.c — fatal errors, checked allocation, vectors, buffers, CRC. */
#include "buf.h"
#include "crc32.h"
#include "vec.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void NcAssertFail(const char* _expr, const char* _file, int _line)
{
  fflush(stdout);
  fprintf(stderr, "%s(%d): fatal error NC0001: internal compiler error: assertion '%s' failed\n", _file, _line, _expr);
  abort();
}

void NcFatal(const char* _fmt, ...)
{
  fflush(stdout);
  va_list ap;
  va_start(ap, _fmt);
  fputs("ncc: fatal error NC0002: ", stderr);
  vfprintf(stderr, _fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
  exit(2);
}

void* NcXrealloc(void* _p, size_t _n)
{
  void* q = realloc(_p, _n ? _n : 1);
  if (!q)
    NcFatal("out of memory (%zu bytes)", _n);
  return q;
}

void NcXfree(void* _p)
{
  free(_p);
}

/* ---- vectors ---------------------------------------------------------- */

void VecGrow(void** _data, u32* _cap, u32 _need, size_t _elemSize)
{
  if (_need <= *_cap)
    return;
  u32 ncap = *_cap ? *_cap : 8;
  while (ncap < _need)
  {
    if (ncap > UINT32_MAX / 2)
      NcFatal("vector too large");
    ncap *= 2;
  }
  *_data = NcXrealloc(*_data, (size_t)ncap * _elemSize);
  *_cap = ncap;
}

/* ---- buffers ---------------------------------------------------------- */

void BufFree(Buf* _b)
{
  NcXfree(_b->data);
  memset(_b, 0, sizeof *_b);
}

void BufReserve(Buf* _b, u32 _need)
{
  if (_need <= _b->cap)
    return;
  u32 ncap = _b->cap ? _b->cap : 64;
  while (ncap < _need)
  {
    if (ncap > UINT32_MAX / 2)
      NcFatal("buffer too large");
    ncap *= 2;
  }
  _b->data = NcXrealloc(_b->data, ncap);
  _b->cap = ncap;
}

void BufBytes(Buf* _b, const void* _p, u32 _n)
{
  if (!_n)
    return;
  BufReserve(_b, _b->len + _n);
  memcpy(_b->data + _b->len, _p, _n);
  _b->len += _n;
}

void BufZeros(Buf* _b, u32 _n)
{
  if (!_n)
    return;
  BufReserve(_b, _b->len + _n);
  memset(_b->data + _b->len, 0, _n);
  _b->len += _n;
}

void BufAlign(Buf* _b, u32 _align, u8 _fill)
{
  NC_ASSERT(NC_IS_POW2(_align));
  while (_b->len & (_align - 1))
    BufU8(_b, _fill);
}

void BufStr0(Buf* _b, const char* _s)
{
  BufBytes(_b, _s, (u32)strlen(_s) + 1);
}

/* ---- CRC-32 (reflected, poly 0xEDB88320) ------------------------------- */

static u32 g_crcTable[256];
static bool g_crcReady;

static void CrcInit(void)
{
  for (u32 i = 0; i < 256; i++)
  {
    u32 c = i;
    for (int k = 0; k < 8; k++)
      c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    g_crcTable[i] = c;
  }
  g_crcReady = true;
}

u32 Crc32Update(u32 _crc, const void* _data, size_t _n)
{
  if (!g_crcReady)
    CrcInit();
  const u8* p = _data;
  for (size_t i = 0; i < _n; i++)
    _crc = g_crcTable[(_crc ^ p[i]) & 0xFF] ^ (_crc >> 8);
  return _crc;
}

u32 Crc32Jam(const void* _data, size_t _n)
{
  return Crc32Update(0xFFFFFFFFu, _data, _n);
}
