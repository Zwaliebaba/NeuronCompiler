/* core.h — base definitions shared by every NeuronCompiler component.
 *
 * NC-Boot dialect: C23 subset accepted by Clang >= 18 and GCC >= 13 in
 * -std=c2x mode (see Design/13-self-hosting-bootstrap.md). Do not use
 * `long` (LP64 vs LLP64), binary literals or digit separators here. */
#ifndef NC_CORE_H
#define NC_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;

#define NC_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define NC_MIN(a, b) ((a) < (b) ? (a) : (b))
#define NC_MAX(a, b) ((a) > (b) ? (a) : (b))
#define NC_ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~(typeof(x))((a) - 1))
#define NC_IS_POW2(x) ((x) != 0 && ((x) & ((x) - 1)) == 0)

/* Fatal internal error. Never returns. */
[[noreturn]] void nc_assert_fail(const char *expr, const char *file, int line);
[[noreturn]] void nc_fatal(const char *fmt, ...);

#define NC_ASSERT(e) ((e) ? (void)0 : nc_assert_fail(#e, __FILE__, __LINE__))
#define NC_UNREACHABLE() nc_assert_fail("unreachable", __FILE__, __LINE__)

/* Checked heap allocation for the few structures that are not arena-owned
 * (growable vectors). Aborts on failure. */
void *nc_xrealloc(void *p, size_t n);
void  nc_xfree(void *p);

/* Bit helpers. */
static inline u32 nc_clz64(u64 x) { return x ? (u32)__builtin_clzll(x) : 64; }
static inline u32 nc_ctz64(u64 x) { return x ? (u32)__builtin_ctzll(x) : 64; }
static inline u32 nc_popcount64(u64 x) { return (u32)__builtin_popcountll(x); }
static inline bool nc_fits_signed(i64 v, u32 bits) {
    i64 lim = (i64)1 << (bits - 1);
    return v >= -lim && v < lim;
}
static inline bool nc_fits_unsigned(u64 v, u32 bits) {
    return bits >= 64 || v < ((u64)1 << bits);
}

#endif
