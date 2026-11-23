# 01 — Goals, scope and risks

## 1. Goals

The compiler translates ISO/IEC 9899:2024 (C23) translation units into ARM64 COFF objects that `link.exe` 14.50 links, together with objects produced by MSVC and clang-cl, into PE32+ executables and DLLs that run on Windows 11 on Arm. Interoperability with MSVC-compiled code is a hard requirement: an `ncc` object and a `cl` object must be able to call each other, share structs, share thread-local data and unwind through each other's frames.

The secondary goals, in priority order, are: correctness that is demonstrable rather than asserted (every claim in the conformance matrix backed by a test); debuggability in the Visual Studio debugger at parity with MSVC `/Od /Z7`; generated code within a defined factor of MSVC `/O2` (target: geometric mean ≤ 1.4× runtime on the benchmark set in [14-testing-strategy.md](14-testing-strategy.md)); compile throughput at least comparable to MSVC `/Od` on `windows.h`-heavy code; and byte-reproducible output.

## 2. What "full C23 support" means

"Full C23" needs a precise definition, because the standard leaves a lot to the implementation and splits responsibility between compiler and library. We define it as follows.

**Language conformance.** Every mandatory language feature of C23 is implemented, and every constraint violation produces a diagnostic. This includes the features that are new in C23: `bool`/`true`/`false`/`nullptr`/`constexpr`/`static_assert`/`thread_local`/`alignas`/`alignof`/`typeof`/`typeof_unqual` as keywords; `auto` type inference for object definitions; `constexpr` objects; `nullptr_t`; enumerations with a fixed underlying type; `_BitInt(N)` with `wb`/`uwb` suffixes; standard attributes (`deprecated`, `fallthrough`, `maybe_unused`, `nodiscard`, `noreturn`, `unsequenced`, `reproducible`); empty initialisers `= {}`; storage-class specifiers in compound literals; labels before declarations and at the end of blocks; unnamed parameters in definitions; binary literals and digit separators; `u8` character constants; `char8_t`-typed `u8` string literals; single-argument `static_assert`; the removal of K&R definitions and unprototyped declarations (`int f();` now means `int f(void)`); and mandatory two's complement.

**Preprocessor conformance.** `#embed` with `limit`, `prefix`, `suffix` and `if_empty`; `__has_include`, `__has_embed`, `__has_c_attribute`; `#elifdef`/`#elifndef`; `#warning`; `__VA_OPT__`; `__STDC_VERSION__` = `202311L`; and the standard pragmas including the new `FENV_ROUND` and `FENV_DEC_ROUND`.

**Library conformance.** A hosted implementation must also provide the C23 library. The UCRT does not provide all of it (see [11-runtime-and-headers.md](11-runtime-and-headers.md)). We close the gap with compiler-provided headers and a supplementary static library, `libncrt.lib`. Without that, the honest claim would be "C23 language, C17-ish library", and we should not make it.

**Conditional features.** C23 lets an implementation opt out of several features by defining a macro. Our position:

| Feature | Macro | Position | Rationale |
|---------|-------|----------|-----------|
| Automatic-storage VLAs | `__STDC_NO_VLA__` | Supported (macro not defined) | Variably modified types are mandatory in C23 anyway; automatic VLAs add only dynamic stack allocation with probing. |
| Complex types | `__STDC_NO_COMPLEX__` | Supported | The UCRT's `<complex.h>` is non-conforming (struct types); we ship our own header, ABI-bridged to UCRT functions (§11). |
| Atomics | `__STDC_NO_ATOMICS__` | Supported | Compiler-implemented; lock-free for 1–16 bytes, `libncrt` lock table above that. |
| Threads | `__STDC_NO_THREADS__` | Supported via UCRT `<threads.h>` | Present in the UCRT since VS 2022 17.8; verify completeness in the gap probe. |
| IEC 60559 binary FP (Annex F) | `__STDC_IEC_60559_BFP__` | Supported | ARM64 FP is IEEE 754; `long double` is binary64 (MSVC ABI). Requires honouring `FENV_ACCESS` in the optimiser. |
| IEC 60559 decimal FP | `__STDC_IEC_60559_DFP__` | **Not supported in v1** | Needs a full decimal arithmetic runtime plus `printf`/`scanf`/`strto*` support the UCRT lacks. Conforming to omit. Revisit after v1. |
| Interchange FP types (Annex H) | `__STDC_IEC_60559_TYPES__` | Partial: `_Float32`, `_Float64`, `_Float32x`, `_Float16` | `_Float16` arithmetic via widen-compute-narrow on ARMv8.0, native with `+fp16`. `_Float128` and `_Float64x` deferred (soft-float runtime). The macro is not defined until complete. |
| Bounds-checking interfaces (Annex K) | `__STDC_LIB_EXT1__` | Not defined | The UCRT `*_s` functions differ from Annex K semantics; we do not claim conformance. |
| Analyzability (Annex L) | `__STDC_ANALYZABLE__` | Not defined | No value for the target audience. |

`BITINT_MAXWIDTH` is set to 65535 in v1. The standard requires only `ULLONG_WIDTH` (64); 65535 covers every realistic use (cryptographic widths, fixed-point accumulators) while keeping compile-time bigint arithmetic and the runtime division helper simple. Clang allows 8388608; matching that is possible later without ABI change.

## 3. Target platform

The target is user-mode Windows 11 on Arm, ARMv8.0-A with FP and Advanced SIMD as the guaranteed baseline (the documented Windows ARM64 minimum). Optional extensions (LSE atomics, CRC32, crypto, FP16, RCPC, dot product) are opt-in via `/arch:` style switches, never assumed. Kernel-mode code generation is a later possibility but not a v1 goal; the design avoids precluding it (no red-zone use beyond the 16 bytes Windows guarantees, `x18` never touched, `__chkstk` probing always emitted).

## 4. Non-goals

ARM64EC and ARM64X (different ABI, x64 interoperability thunks); x64, x86 and 32-bit ARM; C++ in any form; a linker, librarian or PDB writer; link-time optimisation and whole-program analysis; auto-vectorisation; profile-guided optimisation; Clang/GCC language extensions beyond those Windows headers or common portable code need (statement expressions and `__attribute__` are evaluated case by case in [04-msvc-compatibility.md](04-msvc-compatibility.md)); legacy MSVC preprocessor behaviour (we implement only the conforming preprocessor, see §03).

## 5. Risk register

The design's weakest points, ranked by how likely they are to hurt the schedule or the result.

**R1 — MSVC header compatibility is a long tail, not a feature.** `windows.h` and the UCRT headers exercise `__declspec` variants, MSVC bit-field layout, `#pragma pack`, `__unaligned`, SAL macros, around 300 intrinsics referenced by `winnt.h` for ARM64, and `_MSC_VER`-gated code paths. Clang-cl took years to get here. Mitigation: a header-compile corpus in CI from phase 2 (every public SDK header compiled in isolation and in combination), a struct-layout cross-check against `cl`, and an intrinsic table that is data-driven so adding one is a one-line change. Likelihood high, impact high.

**R2 — Unwind data errors are silent until they are catastrophic.** Wrong `.pdata`/`.xdata` does not fail at link time; it fails when an exception, `longjmp`, ETW stack walk or debugger unwinds through the frame, possibly mid-prologue. Mitigation: prologue/epilogue generation restricted to canonical forms with a 1:1 unwind-code mapping, and a static unwind verifier that symbolically executes every prologue/epilogue prefix and compares against the unwind interpretation ([09-unwind-and-seh.md](09-unwind-and-seh.md)). Likelihood medium, impact very high.

**R3 — Self-hosting in C makes the optimiser expensive to write.** Without templates, generic containers and pattern-matching DSLs become macros or code generators. Mitigation: invest in the core library first (D-07: X-macro tables, typed vector/hash macros with tested implementations) and keep the IR deliberately small. Likelihood high, impact medium.

**R4 — The UCRT gap is larger than it looks.** Formatted I/O cannot be patched from outside: C23 `printf` adds `%b`, `%wN`, `%wfN`, and the UCRT formatter will not grow them for us. Mitigation: `libncrt` owns a full C23 formatter and the `stdio.h` overlay routes to it in conformance mode ([11-runtime-and-headers.md](11-runtime-and-headers.md)). This is a real cost (a correct `printf`/`scanf` including float formatting is several thousand lines) and an open question whether it is default. Likelihood certain, impact medium.

**R5 — Optimiser semantics versus MSVC-era code.** MSVC has never exploited strict aliasing and Windows code freely type-puns. A strict-aliasing optimiser will miscompile real code that "works with MSVC". Mitigation: type-based alias analysis off by default, enabled by `/Qstrict-aliasing`; `char` signed (MSVC convention, not AAPCS64); `volatile` with ISO semantics (matches MSVC's ARM64 default `/volatile:iso`). Likelihood high if ignored, impact high.

**R6 — CodeView is under-documented.** The `microsoft-pdb` repository's `cvinfo.h` and LLVM's emitter are the de facto specification. Mitigation: validate every object with `cvdump`/`llvm-pdbutil` and with the VS debugger in automated tests; keep the emitter conservative (only record kinds MSVC itself emits for C). Likelihood medium, impact medium.

**R7 — `_BitInt` has no MSVC ABI.** MSVC does not implement `_BitInt`, so there is no Microsoft-defined layout or passing convention. Mitigation: follow AAPCS64's `_BitInt` rules, which clang targeting `aarch64-pc-windows-msvc` also follows, and cross-test against clang-cl. Likelihood low, impact low (only affects mixed-compiler `_BitInt` interfaces).

**R8 — Stage-0 dependency.** MSVC `cl /std:clatest` does not implement enough of C23 to build us. Mitigation: a restricted implementation dialect ([13-self-hosting-bootstrap.md](13-self-hosting-bootstrap.md)) buildable by Clang ≥ 19 and GCC ≥ 14, plus a published seed binary once we self-host. Likelihood certain, impact low.

## Open items from this document

Whether conformance mode routes `printf` to `libncrt` by default (Q-03); whether `BITINT_MAXWIDTH` should match clang (Q-05); whether the baseline should move to ARMv8.1 for LSE atomics (Q-06). See [16-open-questions.md](16-open-questions.md).
