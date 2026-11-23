# 16 — Decisions and open questions

This started as a list of open questions. All but one were decided on 2026-11-23; the decisions are recorded here with their consequences, and the affected documents have been updated. New questions get appended with status *open*.

## Decided

| ID | Question | Decision | Consequence | Docs |
|----|----------|----------|-------------|------|
| Q-01 | Licence | **Keep private for now.** Decide before any public release. | Third-party code (CORE-MATH, musl) is MIT and only needs attribution, so the eventual choice stays open. Test inputs taken from GPL suites (GCC torture) are never shipped. The eventual licence for `libncrt` and the headers must not be copyleft. | — |
| Q-02 | Way of working | **Claude implements, you review.** Phase 0 first as a vertical slice, then re-plan using measured velocity. | Phases 3 and 4 are sequenced, not parallel, unless velocity says otherwise. Work arrives as PRs scoped to one component spec. | [15](15-roadmap.md) |
| Q-03 | C23 `printf`/`scanf` default | **On by default under `/std:c23`**, `/Qucrt-stdio` opts out. | `libncrt` formatter is phase-3 critical path. | [11](11-runtime-and-headers.md), [12](12-driver-and-toolchain.md) |
| Q-04 | Decimal FP | **Decide after v1.** | `__STDC_IEC_60559_DFP__` stays undefined. | [01](01-goals-scope-risks.md) |
| Q-05 | `BITINT_MAXWIDTH` | **65535.** | — | [01](01-goals-scope-risks.md) |
| Q-06 | Target baseline | **ARMv8.1-A from the start** (LSE atomics assumed). `/arch:armv8.0` remains as an opt-in fallback. | Atomics lower to `cas`/`ldadd`/`swp`/`casp` by default; the LL/SC path is kept for `/arch:armv8.0` only and becomes a secondary test configuration. Binaries will fault on ARMv8.0-only CPUs, which to our knowledge are outside Windows 11's supported hardware list (to verify). | [01](01-goals-scope-risks.md), [05](05-ir-and-optimizer.md), [06](06-arm64-backend.md) |
| Q-07 | Kernel mode | **Not in v1; keep it possible.** | Existing constraints stay (no red zone, `x18` untouched, probing). | [01](01-goals-scope-risks.md) |
| Q-08 | UTF-16 source | **Accept when a BOM is present.** | — | [03](03-frontend.md) |
| Q-09 | Common symbols | **ISO default; `/Qcommon` opt-in.** | — | [03](03-frontend.md) |
| Q-10 | Empty structs | **Always reject**, no switch. | No `/Qgnu-ext` empty-struct mode. | [04](04-msvc-compatibility.md) |
| Q-11 | ACLE `<arm_neon.h>` | **Yes, post-v1**, after MSVC `arm64_neon.h` intrinsics. | — | [04](04-msvc-compatibility.md) |
| Q-12 | `__ptr32` | **Parse; implement on demand.** | — | [04](04-msvc-compatibility.md) |
| Q-13 | Strict aliasing default | **Never on by default**; `/Qstrict-aliasing` only. | — | [05](05-ir-and-optimizer.md) |
| Q-14 | Argument evaluation order | **Left-to-right, documented.** | — | [05](05-ir-and-optimizer.md) |
| Q-15 | Pre-RA scheduler | **Not in v1**; measure first. | — | [06](06-arm64-backend.md) |
| Q-16 | Function fragments | **Not in v1**; diagnose functions over 1 MiB. | No shrink-wrapping in v1. | [06](06-arm64-backend.md) |
| Q-17 | Callee re-extension | **Skip at `-O2` for internal-linkage callees** where all callers are ours. | Must be disabled for any static function whose address escapes. | [07](07-windows-arm64-abi.md) |
| Q-18 | `/Gw` data COMDATs | **Default at all optimisation levels.** | — | [08](08-coff-object-writer.md) |
| Q-20 | SEH timing | **v1.0.** | Phase 6 includes funclets and scope tables; Q-19 must be resolved before phase 6. | [09](09-unwind-and-seh.md), [15](15-roadmap.md) |
| Q-21 | `_BitInt` in the debugger | **Struct of limbs + shipped natvis.** | — | [10](10-codeview.md) |
| Q-22 | Variadic `assert` | **Always**, in every language mode. | — | [11](11-runtime-and-headers.md) |
| Q-23 | Math sources | **CORE-MATH first, musl otherwise** (both MIT). | Attribution file in `runtime/THIRD_PARTY.md`. | [11](11-runtime-and-headers.md) |
| Q-24 | CMake | **Toolchain file now, upstream compiler id after v1.** | — | [12](12-driver-and-toolchain.md) |
| Q-25 | Commercial conformance suite | **Only if the project goes public**, before any external conformance claim. | Until then the product is described as "implements C23", not "conforms to C23". | [14](14-testing-strategy.md) |

## Open

| ID | Question | Plan | Docs |
|----|----------|------|------|
| Q-19 | Exact SEH funclet / establisher-frame convention on ARM64 | Not a preference: determine empirically from `cl` output and `__C_specific_handler` behaviour before phase 6. | [09](09-unwind-and-seh.md) |
| Q-27 | **Needs you.** Repository visibility versus Q-01: Q-01 says "keep private for now", but `Zwaliebaba/NeuronCompiler` is public on GitHub. Without a licence file the code is visible but no rights are granted. | Your decision: make the repository private (hosted Windows ARM64 runners are then billed against private-repository minutes, at 2 vCPUs), or keep it public and pick a licence now. | [12](12-driver-and-toolchain.md) |
| Q-26 | `S_FRAMEPROC` local/parameter base-pointer encoding on ARM64. Phase 0 writes 2 ("frame pointer"), following the x64 convention; LLVM's dumper cannot decode ARM64 values, so this is unverified. | Compare with `cl /Z7` output on Windows during phase-0 acceptance. Also check `dumpbin /unwindinfo`'s annotation "Expected opcode CB2053FF, got CB2F73FF" on the `__chkstk` allocation `sub sp, sp, x15, lsl #4`. Our instruction matches Microsoft's documented sequence and `RtlVirtualUnwind` accepts it at every boundary. `CB2053FF` decodes to `sub sp, sp, w0, uxtw #4`, so the annotation looks like an approximate template in dumpbin; confirm with a `cl`-compiled large-frame function. | [10](10-codeview.md) |
