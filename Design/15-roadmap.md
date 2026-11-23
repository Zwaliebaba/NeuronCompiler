# 15 — Roadmap

## 1. Strategy

The roadmap builds the pipeline **from the object file backwards**, then grows the front end. The tail of the pipeline (encoding, relocations, COMDATs, unwind data, `link.exe` acceptance) is where the platform-specific unknowns are, and it is the part that cannot be tested by comparison with another C compiler's semantics. Proving it first, with hand-built MIR, means every later phase rests on an object writer already known to produce linkable, unwindable, debuggable output.

Self-hosting comes early (phase 2), before C23 is complete and before `windows.h`, because the compiler is its own best test and because NC-Boot was designed to make that possible.

## 2. Phases

Each phase has an exit criterion that is a test result, not a judgement.

**Phase 0 — Foundations.** Core library (arenas, containers, interning, bigint, correctly rounded float parsing, diagnostics, OS layer); `arm64.def` with the initial instruction set, encoder and `-S` printer; COFF writer with COMDATs, relocations and `.drectve`; frame plans, unwind packer, full `.xdata` emitter and the static unwind verifier; minimal CodeView (`S_OBJNAME`, `S_COMPILE3`, `S_GPROC32_ID`, line tables). Test harness and CI on Linux and Windows ARM64.
*Exit:* a program built from hand-constructed MIR (several functions, a call into the UCRT, a large frame with `__chkstk`, a variadic call to `printf`) links with `link.exe`, runs, can be stepped by line in `cdb`, and survives `RtlVirtualUnwind` at every instruction. Encoding differential against `llvm-mc` green for all implemented forms.

**Phase 1 — C17 core at `-O0`.** Preprocessor (complete, conforming), parser and sema for C17 (without `_Complex`, atomics, VLAs), NIR, IRGen with ABI lowering, instruction selection, fast local register allocator, frame lowering, jump tables. Enough of the compatibility layer to compile the UCRT headers used by portable code (`stdio.h`, `stdlib.h`, `string.h`, `math.h`, `ctype.h`, `time.h`), which already requires `__declspec`, SAL-macro tolerance and MSVC `__inline` semantics (the UCRT's `printf` is an inline wrapper).
*Exit:* `c-testsuite` 100%; 100% of a curated, checked-in subset of GCC torture `execute` tests that use no GNU extensions; Lua and SQLite build and pass their test suites; ABI cross-check fixed corpus green against `cl`.

**Phase 2 — Self-hosting.** Remaining NC-Boot features, compiler robustness on its own source.
*Exit:* stage 2 == stage 3 objects at `-O0`, with and without `/Z7`, on every merge.

**Phase 3 — C23 complete, runtime and full `-O0` debug info.** All C23 language features (§01), `_Complex`, atomics, VLAs, `_BitInt` with legalisation and runtime, `#embed`, `constexpr`, attributes, `FENV_ACCESS`/`FENV_ROUND`; compiler-provided headers and overlays; `libncrt` including C23 formatted I/O and the math additions; full CodeView types and locals at `-O0`.
*Exit:* conformance matrix has no red clause; library gap probe empty; debugger test suite at `-O0` green.

**Phase 4 — MSVC compatibility breadth.** `windows.h` and every public SDK C header; MSVC layout rules verified by the generated cross-check; intrinsic groups 1–3; TLS; dllimport/dllexport; `#pragma` coverage; MSBuild toolset and CMake toolchain file.
*Exit:* every public header in the Windows SDK `um` and `shared` directories compiles in isolation; nightly ABI cross-check (≥ 10,000 generated cases) green; curl and the SDK Win32 samples build and run.

Phases 3 and 4 are independent. With the agreed way of working (Claude implements, you review; Q-02) they run sequentially by default; they can overlap if review throughput allows.

**Phase 5 — Optimiser.** SSA construction, the `-O1` and `-O2` pipelines (§05), SSA linear-scan allocation, post-RA passes, optimised-code CodeView (location ranges, inline sites).
*Exit:* bootstrap fixed point at `-O2`; 10 million Csmith/YARPGen programs with no `-O0`/`-O2` divergence; benchmark geometric mean ≤ 1.4× `cl /O2`; debugger `-O2` suite green.

**Phase 6 — v1.0.** SEH (`__try`/`__except`/`__finally`, in v1.0 per Q-20; requires Q-19 resolved first), `/GS`, `/guard:cf`, packaging (installer or zip with `ncc.exe`, `include/ncc`, `libncrt` variants, MSBuild props, natvis), documentation, seed-binary release process.
*Exit:* real-world corpus green at `-O0` and `-O2`; release candidate builds itself from the previous release and from Clang.

**After v1** (candidates, unordered): NEON intrinsics (`arm64_neon.h`, then ACLE `arm_neon.h`); shrink-wrapping and function fragments; pre-RA scheduling; PCH; decimal floating point; `_Float128`; ARMv9 features (SVE is not supported by Windows user mode today, so not a priority); kernel-mode code generation; in-process parallel codegen.

## 3. Size estimate

Order-of-magnitude, non-blank, non-comment lines of C excluding tests, to calibrate expectations rather than to plan against:

| Component | kLOC |
|-----------|------|
| core (incl. bigint, float conversion, OS layer) | 8–10 |
| preprocessor | 6–8 |
| parser + sema (incl. layout, constant evaluation, MSVC compat) | 25–32 |
| IRGen (incl. ABI lowering) | 7–9 |
| NIR + analyses + passes | 22–30 |
| ARM64 back end (isel, RA, frames, encoder, printer) | 18–24 |
| unwind + COFF + CodeView | 10–13 |
| driver | 3–4 |
| `libncrt` (incl. printf/scanf, math additions) | 12–18 |
| **Total** | **≈ 110–150** |

Tests and test tooling are of the same order again. For comparison, TCC is roughly 80k lines (no SSA optimiser, no CodeView), and Clang's C-relevant front end alone is several times our whole estimate. The estimate is plausible for the scope only because the design refuses retargetability, C++, a linker, and LLVM-class optimisation.

## 4. Phase 0 status (November 2026)

Implemented and tested on Linux. The core library covers arenas, vectors, byte buffers, CRC-32 and the OS layer. The ARM64 instruction table has 224 entries, with encoder, printer and constant materialisation. The COFF writer supports COMDATs, associative sections, long names, `.drectve`, bigobj and relocation overflow. Also done: frame plans, prologues and epilogues, packed and full unwind records, the unwind verifier, branch relaxation, minimal CodeView, and `p0demo`, which builds two programs from hand-written MIR.

Verified so far:

* All 1,489 generated instruction forms encode byte-identically to `llvm-mc`.
* All 6,636 logical immediates round-trip.
* 38,016 frame shapes pass the verifier, and its mutation tests behave as specified.
* The no-CRT program links with `lld-link` against an import library generated from a `.def` file, and LLVM's readers decode the unwind data, imports and PDB as intended.
* Output is byte-identical across runs and across GCC- versus Clang-built tools.
* Clean under ASan and UBSan.

Outstanding before the phase-0 exit criterion is met. All of these need Windows on Arm:

* Linking and running both programs with `link.exe` (`tests/windows/p0-accept.cmd`).
* Stepping them in the VS debugger.
* The `RtlVirtualUnwind` single-step harness.
* Confirming the `S_FRAMEPROC` frame-register encoding against `cl /Z7` (Q-26).

Moved to phase 1 because only the front end needs them: string interning, bigint, correctly rounded float parsing, SHA-256 file checksums. Jump tables move to phase 1 with `switch` lowering.

## 5. Critical path

The longest dependency chain is phase 0 → 1 → 3 → 5, with phase 4 (MSVC compatibility breadth) as the largest *uncertain* block (R1) running alongside 3. The earliest point at which the compiler is useful to someone else is the end of phase 4 at `-O0`; the earliest point at which it can be compared with MSVC on its own terms is the end of phase 5.
