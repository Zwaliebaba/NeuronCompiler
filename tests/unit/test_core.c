#include "test.h"

#include "../../src/core/arena.h"
#include "../../src/core/buf.h"
#include "../../src/core/crc32.h"
#include "../../src/core/vec.h"

void test_core(void) {
    Arena a;
    arena_init(&a);
    ArenaMark m0 = arena_mark(&a);
    for (u32 i = 0; i < 10000; i++) {
        u64 *p = arena_alloc(&a, sizeof(u64) * (i % 7 + 1), 8);
        CHECK(((uintptr_t)p & 7) == 0);
        *p = i;
    }
    void *big = arena_alloc(&a, 1 << 20, 64);
    CHECK(((uintptr_t)big & 63) == 0);
    memset(big, 0xAB, 1 << 20);
    char *s = arena_printf(&a, "%s-%d", "x", 42);
    CHECK(strcmp(s, "x-42") == 0);
    arena_reset(&a, m0);
    CHECK(a.head == nullptr);
    arena_release(&a);

    VEC(u32) v = {};
    for (u32 i = 0; i < 1000; i++) vec_push(&v, i * 3);
    CHECK(v.len == 1000 && v.data[999] == 2997);
    vec_free(&v);

    Buf b = {};
    buf_u8(&b, 1); buf_u16(&b, 0x0302); buf_u32(&b, 0x07060504); buf_u64(&b, 0x0F0E0D0C0B0A0908ull);
    CHECK(b.len == 15);
    for (u32 i = 0; i < 15; i++) CHECK(b.data[i] == i + 1);
    buf_align(&b, 16, 0);
    CHECK(b.len == 16);
    buf_put_u32(&b, 0, 0xDEADBEEF);
    CHECK(buf_get_u32(&b, 0) == 0xDEADBEEF);
    buf_free(&b);

    /* Known CRC-32 check value: crc32("123456789") = 0xCBF43926. */
    CHECK((nc_jamcrc("123456789", 9) ^ 0xFFFFFFFFu) == 0xCBF43926u);
}
