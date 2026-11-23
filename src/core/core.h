/* core.h — base definitions shared by every NeuronCompiler component.
 *
 * NC-Boot dialect: C23 subset accepted by Clang >= 18 and GCC >= 13 in
 * -std=c2x mode (see Design/13-self-hosting-bootstrap.md). Do not use
 * `long` (LP64 vs LLP64), binary literals or digit separators here. */
#ifndef NC_CORE_H
#define NC_CORE_H

#include "compiler.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

#define NC_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define NC_MIN(a, b) ((a) < (b) ? (a) : (b))
#define NC_MAX(a, b) ((a) > (b) ? (a) : (b))
#define NC_ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~(typeof(x))((a) - 1))
#define NC_IS_POW2(x) ((x) != 0 && ((x) & ((x) - 1)) == 0)

/* Fatal internal error. Never returns. */
[[noreturn]] void NcAssertFail(const char* _expr, const char* _file, int _line);
[[noreturn]] void NcFatal(const char* _fmt, ...);

#define NC_ASSERT(e) ((e) ? (void)0 : NcAssertFail(#e, __FILE__, __LINE__))
#define NC_UNREACHABLE() NcAssertFail("unreachable", __FILE__, __LINE__)

/* Checked heap allocation for the few structures that are not arena-owned
 * (growable vectors). Aborts on failure. */
void* NcXrealloc(void* _p, size_t _n);
void NcXfree(void* _p);

/* Bit helpers. */
static inline u32 NcClz64(u64 _x)
{
  return _x ? NC_CLZ64_NONZERO(_x) : 64;
}
static inline u32 NcCtz64(u64 _x)
{
  return _x ? NC_CTZ64_NONZERO(_x) : 64;
}
static inline u32 NcPopcount64(u64 _x)
{
  return NC_POPCOUNT64(_x);
}
static inline bool NcFitsSigned(i64 _v, u32 _bits)
{
  i64 lim = (i64)1 << (_bits - 1);
  return _v >= -lim && _v < lim;
}
static inline bool NcFitsUnsigned(u64 _v, u32 _bits)
{
  return _bits >= 64 || _v < ((u64)1 << _bits);
}

#endif
