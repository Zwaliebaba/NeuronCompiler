/* foreign.c — compiled by clang (not ncc) for the link test. Its functions
 * give ncc-uwcheck unwind data from an independent producer, including frame
 * forms ncc never emits (unchained leaves, different save orders), so the
 * checker's model is tested against real-world records, not only our own.
 *
 * It is a test fixture, not compiler source: only clang compiles it, for
 * aarch64-pc-windows-msvc, and its __asm__ barriers exist to stop clang from
 * folding the work away. NC-Boot (AGENTS.md R12) does not apply to it. */
typedef unsigned long long u64;
extern void Sink(u64*);

/* Leaf that needs callee-saved registers: unchained frame (packed CR=0/1). */
u64 ForeignLeafSaves(u64 _a, u64 _b, u64 _c, u64 _d)
{
  u64 x = _a * 3, y = _b * 5, z = _c * 7, w = _d * 11;
  for (int i = 0; i < 100; i++)
  {
    x = x * y + z;
    y = y ^ (w >> 3);
    z = z + x * w;
    w = w - y;
    __asm__ volatile("" : "+r"(x), "+r"(y), "+r"(z), "+r"(w));
  }
  return x + y + z + w;
}

/* Non-leaf with locals. */
u64 ForeignNonleaf(u64 _n)
{
  u64 buf[8];
  for (int i = 0; i < 8; i++)
    buf[i] = _n + (u64)i;
  Sink(buf);
  return buf[0] + buf[7];
}

/* Large frame: probes the stack with __chkstk (defined by p0_nocrt.obj). */
u64 ForeignBig(u64 _n)
{
  u64 big[1024];
  for (int i = 0; i < 1024; i++)
    big[i] = _n ^ (u64)i;
  Sink(big);
  return big[1023];
}

/* Floating-point callee-saved registers across a call. */
double ForeignFp(double _a, double _b)
{
  double x = _a * _b, y = _a + _b;
  Sink((u64*)0);
  return x / y + _a;
}

void Sink(u64* _p)
{
  __asm__ volatile("" : : "r"(_p) : "memory");
}
