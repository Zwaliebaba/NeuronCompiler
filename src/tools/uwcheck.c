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
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "../core/os.h"
#include "../target/arm64/unwind.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct Image {
    u8 *base;
    u32 size;
    u32 pdata_rva, pdata_size;
} Image;

static bool fail(const char *msg) {
    fprintf(stderr, "ncc-uwcheck: %s\n", msg);
    return false;
}

/* Maps the file's sections at their RVAs into a zeroed buffer. */
static bool map_image(const u8 *f, size_t n, Image *img) {
    if (n < 0x40 || f[0] != 'M' || f[1] != 'Z') return fail("not a PE image (no MZ header)");
    u32 pe = rd_u32(f + 0x3C);
    if ((size_t)pe + 24 > n || memcmp(f + pe, "PE\0\0", 4) != 0) return fail("no PE signature");
    const u8 *fh = f + pe + 4;
    if (rd_u16(fh) != 0xAA64) return fail("not an ARM64 image");
    u32 nsec = rd_u16(fh + 2), opt_size = rd_u16(fh + 16);
    const u8 *opt = fh + 20;
    if ((size_t)(opt - f) + opt_size > n || rd_u16(opt) != 0x20B) return fail("not a PE32+ image");
    u32 size_image = rd_u32(opt + 56), size_headers = rd_u32(opt + 60);
    u32 ndirs = rd_u32(opt + 108);
    if (ndirs < 4) return fail("image has no exception directory");
    img->pdata_rva = rd_u32(opt + 112 + 3 * 8);
    img->pdata_size = rd_u32(opt + 112 + 3 * 8 + 4);
    img->size = size_image;
    img->base = calloc(1, size_image);
    if (!img->base) return fail("out of memory");
    memcpy(img->base, f, NC_MIN((size_t)size_headers, n));
    const u8 *sh = opt + opt_size;
    for (u32 i = 0; i < nsec; i++, sh += 40) {
        if ((size_t)(sh - f) + 40 > n) return fail("truncated section table");
        u32 va = rd_u32(sh + 12), raw_size = rd_u32(sh + 16), raw_ptr = rd_u32(sh + 20);
        u32 copy = raw_size;
        if ((size_t)raw_ptr + copy > n) return fail("section data beyond end of file");
        if ((size_t)va + copy > size_image) copy = size_image > va ? size_image - va : 0;
        if (raw_ptr && copy) memcpy(img->base + va, f + raw_ptr, copy);
    }
    return true;
}

/* ---- the OS unwinder (Windows only) ------------------------------------- */

enum { STACK_SIZE = 4 << 20 };
alignas(16) static u8 stack_buf[STACK_SIZE];

#if defined(_WIN32)
typedef struct OsCtx {
    Image *img;
    u32 begin;     /* function RVA */
    u32 pdata_off; /* offset of the function's .pdata entry */
} OsCtx;

static bool os_unwind(void *vctx, u32 off, const UwMachine *in, const u64 *addr, const u64 *val,
                      u32 nmem, UwMachine *out) {
    OsCtx *c = vctx;
    memset(stack_buf, 0xCC, sizeof stack_buf);
    for (u32 i = 0; i < nmem; i++) {
        if (addr[i] < (u64)(uintptr_t)stack_buf ||
            addr[i] + 8 > (u64)(uintptr_t)(stack_buf + sizeof stack_buf)) {
            fprintf(stderr, "ncc-uwcheck: emulated store outside the stack buffer\n");
            return false;
        }
        memcpy((void *)(uintptr_t)addr[i], &val[i], 8);
    }
    CONTEXT ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_FULL;
    for (u32 r = 0; r < 31; r++) ctx.X[r] = in->x[r];
    for (u32 r = 0; r < 32; r++) ctx.V[r].Low = in->d[r];
    ctx.Sp = in->sp;
    DWORD64 image_base = (DWORD64)(uintptr_t)c->img->base;
    ctx.Pc = image_base + c->begin + off;
    PVOID handler_data = nullptr;
    DWORD64 establisher = 0;
    PRUNTIME_FUNCTION rf = (PRUNTIME_FUNCTION)(c->img->base + c->pdata_off);
    RtlVirtualUnwind(0 /* UNW_FLAG_NHANDLER */, image_base, ctx.Pc, rf, &ctx, &handler_data,
                     &establisher, nullptr);
    for (u32 r = 0; r < 31; r++) out->x[r] = ctx.X[r];
    for (u32 r = 0; r < 32; r++) out->d[r] = ctx.V[r].Low;
    out->sp = ctx.Sp;
    out->pc = ctx.Pc;
    return true;
}
#endif

int main(int argc, char **argv) {
    bool strict = false, verbose = false;
    const char *path = nullptr;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--strict") == 0) strict = true;
        else if (strcmp(argv[i], "--verbose") == 0) verbose = true;
        else path = argv[i];
    }
    if (!path) {
        fprintf(stderr, "usage: ncc-uwcheck [--strict] [--verbose] image.exe\n");
        return 2;
    }
    u8 *file;
    size_t n;
    if (!os_read_file(path, &file, &n)) {
        fprintf(stderr, "ncc-uwcheck: cannot read %s\n", path);
        return 2;
    }
    Image img = {};
    if (!map_image(file, n, &img)) return 2;
    if (!img.pdata_rva || img.pdata_size % 8 || (u64)img.pdata_rva + img.pdata_size > img.size) {
        fprintf(stderr, "ncc-uwcheck: missing or malformed exception directory\n");
        return 2;
    }
#if defined(_WIN32)
    const char *oracle = "ncc + RtlVirtualUnwind";
#else
    const char *oracle = "ncc only (RtlVirtualUnwind needs Windows)";
#endif
    u64 entry_sp = ((u64)(uintptr_t)(stack_buf + STACK_SIZE) - 4096) & ~(u64)15;
    u32 nfunc = img.pdata_size / 8, ok = 0, partial = 0, skipped = 0, failed = 0, checks = 0;
    for (u32 i = 0; i < nfunc; i++) {
        u32 off = img.pdata_rva + 8 * i;
        u32 begin = rd_u32(img.base + off), word2 = rd_u32(img.base + off + 4);
        UnwindRecord rec;
        u32 fbytes = 0;
        const u8 *xdata = (word2 & 3) == 0 && word2 < img.size ? img.base + word2 : nullptr;
        size_t avail = xdata ? img.size - word2 : 0;
        char err[256] = "";
        UwCheckResult res;
        u32 nchk = 0;
        if (!unwind_record_from_pdata(word2, xdata, avail, &rec, &fbytes)) {
            res = UWC_SKIPPED;
            snprintf(err, sizeof err, "unsupported .pdata form (flag %u)", word2 & 3);
        } else if ((u64)begin + fbytes > img.size) {
            res = UWC_FAIL;
            snprintf(err, sizeof err, "function extends beyond the image");
        } else {
#if defined(_WIN32)
            OsCtx oc = {&img, begin, off};
            res = unwind_crosscheck(img.base + begin, fbytes, &rec, entry_sp, os_unwind, &oc, &nchk,
                                    err, sizeof err);
#else
            res = unwind_crosscheck(img.base + begin, fbytes, &rec, entry_sp, nullptr, nullptr,
                                    &nchk, err, sizeof err);
#endif
        }
        unwind_record_free(&rec);
        checks += nchk;
        static const char *const names[] = {"ok", "partial", "skipped", "FAIL"};
        if (res != UWC_OK || verbose)
            printf("  %-7s rva %08x  %5u bytes  %s%s%u checks%s%s\n", names[res], begin, fbytes,
                   (word2 & 3) == 1 ? "packed  " : "xdata   ", "", nchk, err[0] ? "  " : "", err);
        switch (res) {
        case UWC_OK: ok++; break;
        case UWC_PARTIAL: partial++; break;
        case UWC_SKIPPED: skipped++; break;
        case UWC_FAIL: failed++; break;
        }
    }
    printf("ncc-uwcheck: %s: %u functions: %u ok, %u partial, %u skipped, %u failed; "
           "%u unwinds checked (%s)\n",
           path, nfunc, ok, partial, skipped, failed, checks, oracle);
    free(img.base);
    nc_xfree(file);
    if (failed || (strict && (skipped || partial))) return 1;
    return 0;
}
