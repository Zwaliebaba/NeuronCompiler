/* core.c — fatal errors, checked allocation, vectors, buffers, CRC. */
#include "buf.h"
#include "crc32.h"
#include "vec.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void nc_assert_fail(const char *expr, const char *file, int line) {
    fflush(stdout);
    fprintf(stderr, "%s(%d): fatal error NC0001: internal compiler error: assertion '%s' failed\n",
            file, line, expr);
    abort();
}

void nc_fatal(const char *fmt, ...) {
    fflush(stdout);
    va_list ap;
    va_start(ap, fmt);
    fputs("ncc: fatal error NC0002: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(2);
}

void *nc_xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) nc_fatal("out of memory (%zu bytes)", n);
    return q;
}

void nc_xfree(void *p) { free(p); }

/* ---- vectors ---------------------------------------------------------- */

void vec__reserve(void **data, u32 *cap, u32 need, size_t elem_size) {
    if (need <= *cap) return;
    u32 ncap = *cap ? *cap : 8;
    while (ncap < need) {
        if (ncap > UINT32_MAX / 2) nc_fatal("vector too large");
        ncap *= 2;
    }
    *data = nc_xrealloc(*data, (size_t)ncap * elem_size);
    *cap = ncap;
}

/* ---- buffers ---------------------------------------------------------- */

void buf_free(Buf *b) {
    nc_xfree(b->data);
    memset(b, 0, sizeof *b);
}

void buf_reserve(Buf *b, u32 need) {
    if (need <= b->cap) return;
    u32 ncap = b->cap ? b->cap : 64;
    while (ncap < need) {
        if (ncap > UINT32_MAX / 2) nc_fatal("buffer too large");
        ncap *= 2;
    }
    b->data = nc_xrealloc(b->data, ncap);
    b->cap = ncap;
}

void buf_bytes(Buf *b, const void *p, u32 n) {
    if (!n) return;
    buf_reserve(b, b->len + n);
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

void buf_zeros(Buf *b, u32 n) {
    if (!n) return;
    buf_reserve(b, b->len + n);
    memset(b->data + b->len, 0, n);
    b->len += n;
}

void buf_align(Buf *b, u32 align, u8 fill) {
    NC_ASSERT(NC_IS_POW2(align));
    while (b->len & (align - 1)) buf_u8(b, fill);
}

void buf_str0(Buf *b, const char *s) { buf_bytes(b, s, (u32)strlen(s) + 1); }

/* ---- CRC-32 (reflected, poly 0xEDB88320) ------------------------------- */

static u32 crc_table[256];
static bool crc_ready;

static void crc_init(void) {
    for (u32 i = 0; i < 256; i++) {
        u32 c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
    crc_ready = true;
}

u32 nc_crc32_update(u32 crc, const void *data, size_t n) {
    if (!crc_ready) crc_init();
    const u8 *p = data;
    for (size_t i = 0; i < n; i++) crc = crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

u32 nc_jamcrc(const void *data, size_t n) { return nc_crc32_update(0xFFFFFFFFu, data, n); }
