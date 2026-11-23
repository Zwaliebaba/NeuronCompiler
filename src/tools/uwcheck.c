/* uwcheck.c — checks the unwind data of a linked ARM64 PE image.
 *
 *   ncc-uwcheck [--strict] [--verbose] image.exe|image.dll
 *
 * Maps the image from disk (it is never executed), walks its .pdata, rebuilds
 * each function's unwind record from the image bytes, emulates every prologue
 * prefix and epilogue boundary of the function's real code into a stack
 * buffer, and unwinds from each boundary:
 *
 *   - everywhere: with ncc's own unwinder, which proves the records survived
 *     linking intact and decode from the final image;
 *   - on Windows: additionally with the OS unwinder, RtlVirtualUnwind, which is
 *     the independent oracle Design/09 §3 calls for.
 *
 * Functions using instructions or unwind codes the emulator does not model
 * (typically CRT code compiled by MSVC) are reported as skipped; --strict
 * turns skips into failures, for images built entirely by ncc.
 *
 * This is a test tool, not compiler code, so it may include windows.h. */
#if defined(_WIN32)
#   define WIN32_LEAN_AND_MEAN
#   include <windows.h>
#endif

#include "../core/os.h"
#include "../target/arm64/unwind.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct Image
{
  u8* base;
  u32 size;
  u32 pdataRva, pdataSize;
} Image;

static bool Fail(const char* _msg)
{
  fprintf(stderr, "ncc-uwcheck: %s\n", _msg);
  return false;
}

/* Maps the file's sections at their RVAs into a zeroed buffer. */
static bool MapImage(const u8* _f, size_t _n, Image* _img)
{
  if (_n < 0x40 || _f[0] != 'M' || _f[1] != 'Z')
    return Fail("not a PE image (no MZ header)");
  u32 pe = BufReadU32(_f + 0x3C);
  if ((size_t)pe + 24 > _n || memcmp(_f + pe, "PE\0\0", 4) != 0)
    return Fail("no PE signature");
  const u8* fh = _f + pe + 4;
  if (BufReadU16(fh) != 0xAA64)
    return Fail("not an ARM64 image");
  u32 nsec = BufReadU16(fh + 2), optSize = BufReadU16(fh + 16);
  const u8* opt = fh + 20;
  if ((size_t)(opt - _f) + optSize > _n || BufReadU16(opt) != 0x20B)
    return Fail("not a PE32+ image");
  u32 sizeImage = BufReadU32(opt + 56), sizeHeaders = BufReadU32(opt + 60);
  u32 ndirs = BufReadU32(opt + 108);
  if (ndirs < 4)
    return Fail("image has no exception directory");
  /* PE32+ optional header: the data directories start at offset 112; the
     * exception directory is entry 3, each entry is (RVA, size). */
  const u8* exceptionDir = opt + 112 + (size_t)3 * 8;
  _img->pdataRva = BufReadU32(exceptionDir);
  _img->pdataSize = BufReadU32(exceptionDir + 4);
  _img->size = sizeImage;
  _img->base = calloc(1, sizeImage);
  if (!_img->base)
    return Fail("out of memory");
  memcpy(_img->base, _f, NC_MIN((size_t)sizeHeaders, _n));
  const u8* sh = opt + optSize;
  for (u32 i = 0; i < nsec; i++, sh += 40)
  {
    if ((size_t)(sh - _f) + 40 > _n)
      return Fail("truncated section table");
    u32 va = BufReadU32(sh + 12), rawSize = BufReadU32(sh + 16), rawPtr = BufReadU32(sh + 20);
    u32 copy = rawSize;
    if ((size_t)rawPtr + copy > _n)
      return Fail("section data beyond end of file");
    if ((size_t)va + copy > sizeImage)
      copy = sizeImage > va ? sizeImage - va : 0;
    if (rawPtr && copy)
      memcpy(_img->base + va, _f + rawPtr, copy);
  }
  return true;
}

/* ---- the OS unwinder (Windows only) ------------------------------------- */

enum
{
  STACK_SIZE = 4 << 20
};
alignas(16) static u8 g_stackBuf[STACK_SIZE];

#if defined(_WIN32)
typedef struct OsCtx
{
  const Image* img;
  u32 begin;    /* function RVA */
  u32 pdataOff; /* offset of the function's .pdata entry */
} OsCtx;

static bool OsUnwind(void* _vctx, u32 _off, const UwMachine* _in, const u64* _addr, const u64* _val, u32 _nmem, UwMachine* _out)
{
  OsCtx* c = _vctx;
  memset(g_stackBuf, 0xCC, sizeof g_stackBuf);
  for (u32 i = 0; i < _nmem; i++)
  {
    if (_addr[i] < (u64)(uintptr_t)g_stackBuf || _addr[i] + 8 > (u64)(uintptr_t)(g_stackBuf + sizeof g_stackBuf))
    {
      fprintf(stderr, "ncc-uwcheck: emulated store outside the stack buffer\n");
      return false;
    }
    memcpy(g_stackBuf + (_addr[i] - (u64)(uintptr_t)g_stackBuf), &_val[i], 8); /* in range: checked above */
  }
  CONTEXT ctx;
  memset(&ctx, 0, sizeof ctx);
  ctx.ContextFlags = CONTEXT_FULL;
  for (u32 r = 0; r < 31; r++)
    ctx.X[r] = _in->x[r];
  for (u32 r = 0; r < 32; r++)
    ctx.V[r].Low = _in->d[r];
  ctx.Sp = _in->sp;
  DWORD64 imageBase = (DWORD64)(uintptr_t)c->img->base;
  ctx.Pc = imageBase + c->begin + _off;
  PVOID handlerData = nullptr;
  DWORD64 establisher = 0;
  PRUNTIME_FUNCTION rf = (PRUNTIME_FUNCTION)(c->img->base + c->pdataOff);
  RtlVirtualUnwind(0 /* UNW_FLAG_NHANDLER */, imageBase, ctx.Pc, rf, &ctx, &handlerData, &establisher, nullptr);
  for (u32 r = 0; r < 31; r++)
    _out->x[r] = ctx.X[r];
  for (u32 r = 0; r < 32; r++)
    _out->d[r] = ctx.V[r].Low;
  _out->sp = ctx.Sp;
  _out->pc = ctx.Pc;
  return true;
}
#endif

int main(int argc, char** argv)
{
  bool strict = false, verbose = false;
  const char* path = nullptr;
  for (int i = 1; i < argc; i++)
  {
    if (strcmp(argv[i], "--strict") == 0)
      strict = true;
    else if (strcmp(argv[i], "--verbose") == 0)
      verbose = true;
    else
      path = argv[i];
  }
  if (!path)
  {
    fprintf(stderr, "usage: ncc-uwcheck [--strict] [--verbose] image.exe\n");
    return 2;
  }
  u8* file;
  size_t n;
  if (!OsReadFile(path, &file, &n))
  {
    fprintf(stderr, "ncc-uwcheck: cannot read %s\n", path);
    return 2;
  }
  Image img = {};
  if (!MapImage(file, n, &img))
  {
    free(img.base);
    NcXfree(file);
    return 2;
  }
  if (!img.pdataRva || img.pdataSize % 8 || (u64)img.pdataRva + img.pdataSize > img.size)
  {
    fprintf(stderr, "ncc-uwcheck: missing or malformed exception directory\n");
    free(img.base);
    NcXfree(file);
    return 2;
  }
#if defined(_WIN32)
  const char* oracle = "ncc + RtlVirtualUnwind";
#else
  const char* oracle = "ncc only (RtlVirtualUnwind needs Windows)";
#endif
  u64 entrySp = ((u64)(uintptr_t)(g_stackBuf + STACK_SIZE) - 4096) & ~(u64)15;
  u32 nfunc = img.pdataSize / 8, ok = 0, partial = 0, skipped = 0, failed = 0, checks = 0;
  const u8* base = img.base; /* fixed for the loop; OsUnwind only reads img */
  for (u32 i = 0; i < nfunc; i++)
  {
    u32 off = img.pdataRva + 8 * i;
    u32 begin = BufReadU32(base + off), word2 = BufReadU32(base + off + 4);
    UnwindRecord rec;
    u32 fbytes = 0;
    const u8* xdata = (word2 & 3) == 0 && word2 < img.size ? base + word2 : nullptr;
    size_t avail = xdata ? img.size - word2 : 0;
    char err[256] = "";
    UwCheckResult res;
    u32 nchk = 0;
    if (!UnwindRecordFromPdata(word2, xdata, avail, &rec, &fbytes))
    {
      res = UwCheckResultSkipped;
      snprintf(err, sizeof err, "unsupported .pdata form (flag %u)", word2 & 3);
    }
    else if ((u64)begin + fbytes > img.size)
    {
      res = UwCheckResultFail;
      snprintf(err, sizeof err, "function extends beyond the image");
    }
    else
    {
#if defined(_WIN32)
      OsCtx oc = {&img, begin, off};
      res = UnwindCrosscheck(base + begin, fbytes, &rec, entrySp, OsUnwind, &oc, &nchk, err, sizeof err);
#else
      res = UnwindCrosscheck(base + begin, fbytes, &rec, entrySp, nullptr, nullptr, &nchk, err, sizeof err);
#endif
    }
    UnwindRecordFree(&rec);
    checks += nchk;
    static const char* const NAMES[] = {"ok", "partial", "skipped", "FAIL"};
    if (res != UwCheckResultOk || verbose)
      printf("  %-7s rva %08x  %5u bytes  %s%s%u checks%s%s\n", NAMES[res], begin, fbytes, (word2 & 3) == 1 ? "packed  " : "xdata   ", "",
             nchk, err[0] ? "  " : "", err);
    switch (res)
    {
    case UwCheckResultOk:
      ok++;
      break;
    case UwCheckResultPartial:
      partial++;
      break;
    case UwCheckResultSkipped:
      skipped++;
      break;
    case UwCheckResultFail:
      failed++;
      break;
    }
  }
  printf("ncc-uwcheck: %s: %u functions: %u ok, %u partial, %u skipped, %u failed; "
         "%u unwinds checked (%s)\n",
         path, nfunc, ok, partial, skipped, failed, checks, oracle);
  free(img.base);
  NcXfree(file);
  if (failed || (strict && (skipped || partial)))
    return 1;
  return 0;
}
