/* foreign.c — compiled by clang (not ncc) for the link test. Its functions
 * give ncc-uwcheck unwind data from an independent producer, including frame
 * forms ncc never emits (unchained leaves, different save orders), so the
 * checker's model is tested against real-world records, not only our own. */
typedef unsigned long long u64;
extern void sink(u64 *);

/* Leaf that needs callee-saved registers: unchained frame (packed CR=0/1). */
u64 foreign_leaf_saves(u64 a, u64 b, u64 c, u64 d) {
    u64 x = a * 3, y = b * 5, z = c * 7, w = d * 11;
    for (int i = 0; i < 100; i++) {
        x = x * y + z; y = y ^ (w >> 3); z = z + x * w; w = w - y;
        __asm__ volatile("" : "+r"(x), "+r"(y), "+r"(z), "+r"(w));
    }
    return x + y + z + w;
}

/* Non-leaf with locals. */
u64 foreign_nonleaf(u64 n) {
    u64 buf[8];
    for (int i = 0; i < 8; i++) buf[i] = n + (u64)i;
    sink(buf);
    return buf[0] + buf[7];
}

/* Large frame: probes the stack with __chkstk (defined by p0_nocrt.obj). */
u64 foreign_big(u64 n) {
    u64 big[1024];
    for (int i = 0; i < 1024; i++) big[i] = n ^ (u64)i;
    sink(big);
    return big[1023];
}

/* Floating-point callee-saved registers across a call. */
double foreign_fp(double a, double b) {
    double x = a * b, y = a + b;
    sink((u64 *)0);
    return x / y + a;
}

void sink(u64 *p) { __asm__ volatile("" : : "r"(p) : "memory"); }
