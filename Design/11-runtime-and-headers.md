# 11 — Runtime and headers

## 1. Position

The hosted C library on Windows is the UCRT plus vcruntime, both closed and both versioned with the OS and Visual Studio respectively. We build on them, not around them: startup (`mainCRTStartup`, `WinMainCRTStartup`, DLL entry), `stdio` buffering, locale, heap, `errno` and most of `<math.h>` come from the UCRT, so `ncc` objects and `cl` objects share one CRT state within a process. What the UCRT lacks or gets wrong relative to C23 is supplied by two things we own: **compiler-provided headers** in `include/` and a static library, **`libncrt`**.

We never copy or redistribute Microsoft headers. Where we need to change what a UCRT header provides, we use an *overlay header* that `#include_next`s the original and then adds or redefines, so our files contain only our code.

## 2. The gap table is generated, not maintained by hand

A probe suite (`tests/libprobe`) contains one small TU per C23 library identifier (function, macro, type, version macro), checking that it is declared with the C23 signature, links, and behaves per a short conformance check. Running it against plain UCRT headers produces the *gap table*; running it against our overlays must produce an empty table. The table, not this document, is the source of truth for what `libncrt` contains, and it is re-run against every new Windows SDK. The list below is the expected result based on current knowledge, and every entry is to be confirmed by the probe.

## 3. Compiler-provided headers

**Owned outright** (the compiler defines their content; UCRT versions are not used): `<float.h>` (including C23 `*_NORM_MAX`, `*_SNAN`, `*_IS_IEC_60559`), `<limits.h>` (including `*_WIDTH`, `BOOL_WIDTH`, `BITINT_MAXWIDTH`), `<stdarg.h>` (C23 `va_start` with optional second argument, built on `__builtin_va_*`, and type-compatible with UCRT's `va_list` = `char *`), `<stddef.h>` (`nullptr_t`, `unreachable()`, `max_align_t`, `offsetof` via `__builtin_offsetof`), `<stdbool.h>`, `<stdalign.h>`, `<stdnoreturn.h>`, `<iso646.h>`, `<stdbit.h>` (all type-generic macros and functions; inline implementations over `clz`/`ctz`/`popcount` builtins, plus external definitions in `libncrt`), `<stdckdint.h>` (`ckd_add`/`ckd_sub`/`ckd_mul` as type-generic builtins), `<stdatomic.h>` (all of C23 atomics over compiler builtins), `<complex.h>` and `<tgmath.h>` (see §5).

These coexist with UCRT internals: `corecrt.h` and `vcruntime.h` also typedef `size_t`, `ptrdiff_t`, `va_list` and so on. C11 allows identical typedef redefinition, and ours are identical.

**Overlays** (`#include_next` the UCRT header, then extend):

| Header | C23 additions supplied |
|--------|------------------------|
| `<assert.h>` | Variadic `assert(...)` so commas in the expression work; expands to UCRT's `_wassert`. Applied in every language mode (Q-22). |
| `<stdint.h>`, `<inttypes.h>` | `INTn_WIDTH`, `UINTn_WIDTH` and the other `*_WIDTH` macros; `__STDC_VERSION_*` macros. |
| `<stdio.h>` | C23 formatted I/O (§4); `__STDC_VERSION_STDIO_H__`. |
| `<stdlib.h>` | `strfromd`/`strfromf`/`strfroml`, `free_sized`, `free_aligned_sized`, `memalignment`, `once_flag`/`call_once` if missing, `__STDC_VERSION_STDLIB_H__`. `aligned_alloc`: see §6. |
| `<string.h>` | `memset_explicit` (never elided: implemented out of line with a compiler barrier), `strdup`, `strndup`, `memccpy` under their ISO names without `_CRT_NONSTDC` deprecation, `__STDC_VERSION_STRING_H__`. |
| `<time.h>` | `timegm` (→ `_mkgmtime64`), `gmtime_r` and `localtime_r` (→ `gmtime_s`/`localtime_s` with the arguments swapped and the return convention adapted), `timespec_getres`, `TIME_MONOTONIC` etc. if missing. |
| `<math.h>` | The C23 additions (from TS 18661): `exp10`, `exp2m1`, `expm1`-family completions, `log10p1`, `log2p1`, `rootn`, `rsqrt`, `compoundn`, `pown`, `powr`, `sinpi`/`cospi`/`tanpi` and inverse forms, `fmaximum`/`fminimum` and `_num`/`_mag` variants, `nextup`/`nextdown`, `roundeven`, `fromfp`/`ufromfp` families, `canonicalize`, `iscanonical`, `issignaling`, `iszero`, `issubnormal`, `iseqsig`, `FP_INT_*` macros. Implemented in `libncrt` from CORE-MATH (correctly rounded, MIT) where available, otherwise ports from musl (MIT), with attribution in `runtime/THIRD_PARTY.md` (Q-23). |
| `<fenv.h>` | `femode_t`, `fegetmode`/`fesetmode`, `FE_DFL_MODE`, `fesetexcept`, `fetestexceptflag`, `FE_TONEAREST`-family values consistent with FPCR `RMode`. |
| `<uchar.h>` | `char8_t`, `mbrtoc8`/`c8rtomb` if missing. |
| `<threads.h>` | Only if the probe finds the UCRT version incomplete. |

`#include_next` resolution searches the include path *after* the directory in which the overlay was found, so the overlay must sit before the UCRT path; the driver guarantees that order ([12-driver-and-toolchain.md](12-driver-and-toolchain.md) §3).

## 4. Formatted I/O

C23 extends `printf`/`scanf` with `%b`/`%B`, `%wN` and `%wfN` length modifiers (exact-width and fast types), and `%H`/`%D`/`%DD` for decimal types (not applicable, since we do not support DFP). The UCRT's formatter is internal to `ucrtbase.dll` (`__stdio_common_vfprintf`), and the UCRT headers define `printf` and friends as inline wrappers around it. We cannot add conversions to it.

The design uses a property of those headers: defining `_NO_CRT_STDIO_INLINE` turns `printf` and family into plain external declarations. Our `<stdio.h>` overlay, in C23 conformance mode, defines `_NO_CRT_STDIO_INLINE` before `#include_next` and `libncrt` provides external definitions of the whole `printf`/`scanf` family (narrow and wide, `v`-, `s`-, `sn`-, `f`-variants). The UCRT import and static libraries do not define the plain names (that is what `legacy_stdio_definitions.lib` exists for), so there should be no ambiguity at link time; the probe suite confirms this per SDK, and fails loudly if a project also links `legacy_stdio_definitions.lib`. Our implementation writes and reads through the UCRT `FILE` object using the UCRT's own locking primitives (`_lock_file`/`_fwrite_nolock`…), so output from `ncc` code and from `cl` code in the same process interleaves correctly on the same stream. Floating-point formatting uses a shortest-round-trip and fixed-precision algorithm (Ryū and Ryū-printf) to be exact; parsing reuses the compiler's own correctly rounded decimal converter from `core`.

This is the most expensive item in `libncrt` (an estimated 4–6k lines including tests). It is the default under `/std:c23` (Q-03); `/Qucrt-stdio` opts out and leaves the UCRT inline formatter in place (no `%b`, otherwise C17-level). In `/std:c17` and `/std:c11` modes the UCRT formatter is the default.

## 5. Complex numbers

The UCRT's `<complex.h>` declares `_Dcomplex`/`_Fcomplex` structs and functions over them, because MSVC does not implement `_Complex`. We ship our own `<complex.h>` declaring the standard `double complex` interfaces. The bridge is the ABI: `_Complex double` is an HFA of two `double`s, passed and returned in `d0`/`d1`, and `_Dcomplex` (`struct { double _Val[2]; }`) is also an HFA of two doubles. The two are call-compatible on ARM64, so our header can declare `double complex csqrt(double complex);` and link directly to the UCRT's `csqrt`. The probe suite verifies this per function; any function where the UCRT result is not C23-conforming (branch cuts, special values per Annex G) gets a `libncrt` replacement. `CMPLX`, `I`, `_Complex_I` and `imaginary` support (Annex G `_Imaginary` is optional; not supported) are header-level.

Complex multiplication and division with Annex G infinity/NaN recovery are `libncrt` helpers (`__ncrt_muldc3`, `__ncrt_divdc3` and float variants), called by generated code unless `CX_LIMITED_RANGE` is on.

## 6. Memory allocation: a known conformance hazard

C requires that memory from `aligned_alloc` be releasable by `free`. The UCRT does not provide `aligned_alloc` at all, precisely because its `free` cannot release over-aligned blocks (`_aligned_malloc` memory must go to `_aligned_free`). Our options are: provide `aligned_alloc` only for alignments ≤ 16 (which `malloc` already satisfies) and reject larger alignments at run time with `NULL`/`EINVAL` (conforming, since an implementation may refuse unsupported alignments); or replace the allocator, which is invasive and breaks memory exchange with `cl`-compiled code. The design takes the first option. `free_aligned_sized` and `free_sized` forward to `free`. `memalignment` is trivial.

## 7. Compiler support routines

`libncrt` also provides the out-of-line helpers that generated code calls:

| Routine family | Purpose |
|----------------|---------|
| `__ncrt_udivmodbi`, `__ncrt_sdivmodbi`, `__ncrt_mulbi` | `_BitInt` division/remainder (all widths > 64) and multiplication above 512 bits |
| `__ncrt_bitint_to_f64`/`_f32`, `__ncrt_f64_to_bitint` (and unsigned forms) | `_BitInt` ↔ floating conversions with correct rounding |
| `__ncrt_udivti3`-style 128-bit division | `_BitInt(65..128)` division |
| `__ncrt_muldc3`, `__ncrt_divdc3`, `__ncrt_mulsc3`, `__ncrt_divsc3` | Annex G complex arithmetic |
| `__ncrt_atomic_load/store/exchange/compare_exchange` (sized and generic) | Atomics for objects larger than 16 bytes, via a striped lock table |

Half-precision conversion (`fcvt` between `h` and `s`/`d` registers) is in the ARMv8.0 base FP set, so `_Float16` needs no conversion helpers. `__chkstk`, `__security_check_cookie`, `memcpy`/`memset`/`memmove` and `__C_specific_handler` come from the MSVC CRT libraries, as they do for `cl`.

## 8. Build and variants

`libncrt` is written in C23 and compiled by `ncc` itself (stage 1 onward), which makes it a permanent dogfooding target. It ships in four variants matching the CRT selection: `libncrt.lib` (`/MT`), `libncrtd.lib` (`/MTd`), `libncrt_md.lib` (`/MD`), `libncrt_mdd.lib` (`/MDd`), because UCRT references differ between static linking and DLL import (`__imp_` symbols). The driver's `.drectve` default-library entry selects the matching one. It has no dependency other than the UCRT, vcruntime and `kernel32`.

## Decisions

C23 formatted I/O on by default under `/std:c23` (Q-03); variadic `assert` always (Q-22); math from CORE-MATH, then musl (Q-23).
