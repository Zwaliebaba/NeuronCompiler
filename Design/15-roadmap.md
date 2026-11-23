# 15 — Roadmap

## 1. Strategy

The roadmap builds the pipeline **from the object file backwards**, then grows the front end. The tail of the pipeline (encoding, relocations, COMDATs, unwind data, `link.exe` acceptance) is where the platform-specific unknowns are, and it is the part that cannot be tested by comparison with another C compiler's semantics. Proving it first, with hand-built MIR, means every later phase rests on an object writer already known to produce linkable, unwindable, debuggable output.

Self-hosting comes early (phase 2), before C23 is complete and before `windows.h`, because the compiler is its own best test and because NC-Boot was designed to make that possible.

## 2. Phases

Each phase has an exit criterion that is a test result, not a judgement.

**Phase 0 — Foundations.** Core library (arenas, containers, interning, bigint, correctly rounded float parsing, diagnostics, OS layer); `arm64.def` with the initial instruction set, encoder and `-S` printer; COFF writer with COMDATs, relocations and `.drectve`; frame plans, unwind packer, full `.xdata` emitter and the static unwind verifier; minimal CodeView (`S_OBJNAME`, `S_COMPILE3`, `S_GPROC32_ID`, line tables). Test harness and CI on Linux and Windows ARM64.
*Exit:* a program built from hand-constructed MIR (several functions, a call into the UCRT, a large frame with `__chkstk`, a variadic call to `printf`) links with `link.exe`, runs, can be stepped by line in `cdb`, and survives `RtlVirtualUnwind` at every instruction. Encoding differential against `llvm-mc` green for all implemented forms.

**Phase 1 — C17 core at `-O0`.** Preprocessor (complete, conforming), parser and sema for C17 (without `_Complex`, atomics, VLAs), NIR, IRGen with ABI lowering, instruction selection, fast local register allocator, frame lowering, jump tables. Enough of the compatibility layer to compile the UCRT headers used by portable code (`stdio.h`, `stdlib.h`, `string.h`, `math.h`, `ctype.h`, `time.h`), which already requires `__declspec`, SAL-macro tolerance and MSVC `__inline` semantics (the UCRT's `printf` is an inline wrapper).
*Exit:* `c-testsuite` 100%; 100% of a curated, checked-in subset of GCC torture `execute` tests that use no GNU extensions; Lua and SQLite build and pass their test suites; ABI cross-check fixed corpus green against `cl`. The order of work is §5.

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

Order-of-magnitude, non-blank, non-comment lines of C excluding tests, to calibrate expectations rather than to plan against. Phase 0 measured about 4,240 compiler-library lines, all in core, the ARM64 back end and unwind/COFF/CodeView (§5.1):

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
* `ncc-uwcheck --strict` passes on the `lld-link`-linked image, with every record read back from the final PE.
* Every source, including the `_WIN32` code paths, compiles for Windows ARM64 against the mingw-w64 headers (`make check-windows`, in CI).

**Windows on Arm acceptance, in CI.** The `windows-arm64` job has run on GitHub's hosted `windows-11-arm` runner since PR #3: Windows 11 26200, Visual Studio 2026 18.12, `link.exe` 14.52, clang 22.1. On the current `main`:

* `link.exe` links both programs, the no-CRT one and the one using the MSVC CRT (`printf`, TLS, the CRT's `__chkstk`).
* Both programs run and print exactly the expected output.
* `ncc-uwcheck --strict` on the `ncc`-built image passes, with `RtlVirtualUnwind` agreeing with our unwinder at every one of 96 prologue, body and epilogue boundaries.
* On the CRT image, 383 mostly MSVC-compiled functions are checked: none fails, and forms the checker does not model are reported as skipped.
* All unit tests pass natively on ARM64.

The job found one real defect on its first run: the TLS `add` carrying `SECREL_HIGH12A` lacked its `lsl #12` bit. `link.exe` rejected it (LNK2040); `lld-link` had accepted it silently. It was fixed and is now covered by the `encode_sym` differential test.

Outstanding before the phase-0 exit criterion is fully met. Both are manual:

* Stepping through `p0_nocrt.exe` in the VS debugger: break on `p0_main`, step by line through the pseudo-source, check the call stack.
* Confirming the `S_FRAMEPROC` frame-register encoding against `cl /Z7`, and `dumpbin /unwindinfo`'s "Expected opcode CB2053FF" annotation on the `__chkstk` allocation (Q-26).

Moved to phase 1 because only the front end needs them: string interning, bigint, correctly rounded float parsing, SHA-256 file checksums. Jump tables move to phase 1 with `switch` lowering.

## 5. Phase 1 plan (re-planned at phase-0 exit, Q-30)

### 5.1 What phase 0 measured

All times UTC, from the session transcript and `git log --format=%aI` (merge commits carry the reviewer's `+01:00` offset and are converted).

* **Elapsed:** 4 h 22 min, from the start of the session (15:58) to the merge of the last phase-0 PR (20:20), on 2026-11-23. That covers the design (first commit 16:21), the Q-01…Q-25 decisions (16:32), the implementation (17:08), four PRs and both CI jobs.
* **Code:** 5,343 non-blank, non-comment lines under `src/`, of which about 4,240 are the compiler library and 1,100 the tools (`p0demo`, `ncc-uwcheck`, `ncc-check-tree`), plus 1,203 under `tests/`, counted at `52f1133` over the `.c`, `.h` and `.def` files, excluding blank lines and lines that start a comment. Against §3's 110–150k, the library is about 3–4% of the compiler.
* **Review:** PR #1 (5,035 lines of code) was merged 53 minutes after its last commit, and PRs #2, #3 and #4 41, 12 and 19 minutes after theirs.
* **Defects after the first merge:** three. The `SECREL_HIGH12A` encoding and an unwind form misreported as a failure were found only by the `windows-arm64` job; `lld-link` had accepted the bad object. A verifier blind spot was found while building `ncc-uwcheck`. The other defects phase 0 found were caught by `lld-link`, the mingw-w64 syntax check, the `llvm-mc` differential and the verifier's own mutation tests.

**What it does not license** is extrapolating the elapsed time. Phase 1 is roughly 45–60k lines (the phase-1 share of each §3 component, summed: a judgement, not a measurement). Linearly that would take about two working days, and that figure is wrong for three reasons. Phase 0's oracles were exact and bit-level (`llvm-mc`, `link.exe`, `RtlVirtualUnwind`), while a front end's are statistical: a corpus passes or it does not, and the tail is long. Phase 0 had no integration with anything above it, and phase 1 is mostly integration. And the review times above are not line-by-line review of 5,000 lines; they are a check of scope and design. Correctness has to come from the oracles, so every milestone below is defined by the oracle that proves it.

**What it does license:** the tail-first strategy (§1) worked, the Windows job is the gate that finds what Linux cannot, and work goes fastest when an executable oracle exists before the code does.

### 5.2 Rules for phase-1 work

* **A walking skeleton first.** The pipeline runs end to end (C source to a program that runs on Windows) at milestone 1.6, on a small C subset, and widens from there. Integration defects are found while the pieces are small, and the `windows-arm64` job executes compiled C from that point on.
* **The back end is fed textual NIR before the front end exists** (1.3–1.4), so instruction selection, register allocation and calls are tested against execution on Windows without a parser in the way.
* **One milestone is one or more PRs, each with a test result as its exit.** A PR adds at most about 2,500 lines of non-test, non-table code. That is a judgement, not a measurement: it is the size at which a scope-and-design review still sees the shape of the change. A milestone that needs more is split along its component boundary.
* **Tests that need the UCRT or SDK headers run only in the `windows-arm64` job.** Linux CI has no licensed copy of the headers (Design/12 §2), so front-end and IR tests that do not include them carry the Linux side.
* **New directories** (`src/pp`, `src/parse`, `src/sema`, `src/irgen`, `src/ir`, `src/driver`, `tests/lit`, `tests/abi`) go into the Makefile and `build.cmd` in the PR that creates them; `make check-tree` enforces it.
* **Velocity is re-measured at 1.4 and 1.6**, against the figures in §5.1, and this section is revised if the order or the sizing is wrong.

### 5.3 Milestones, in order

| # | Milestone | Scope | Exit (the oracle) |
|---|-----------|-------|-------------------|
| 1.1 | Core II | `HMAP` (`core/hmap.inc`), string interning (`Sym`), the diagnostics table (`diag.def`, stable `NC` ids, MSVC rendering), source manager with the location space and lazy line tables (03 §1) | Unit tests; rendering checked against the MSVC format the VS error list parses |
| 1.2 | `ncc-lit` | The FileCheck-style runner in NC-Boot C (14 §1), driving `tests/lit` | Its own positive and negative self-tests; a failing check fails CI |
| 1.3 | NIR | Types, the `-O0` instruction subset (no atomics, SIMD or constrained FP), builder, verifier, textual printer and parser (05 §1–3) | Print/parse round trip on a lit corpus; verifier negative tests |
| 1.4 | `-O0` back end | Tree-tiling instruction selection for that subset, call lowering from assigned parameters, the fast local allocator, frame finalisation on the existing frame plans, `switch` lowering with jump tables (06 §1–4); `ncc-nir` compiles textual NIR to `.obj`. Settles Q-29: MIR gets its own header | A NIR execution corpus links with `link.exe` and runs on `windows-arm64`; `ncc-uwcheck --strict` on every image; `lld-link` and LLVM's readers on Linux |
| 1.5 | Lexer and preprocessor | C23 lexing (03 §2), directives, macro expansion including `__VA_OPT__`, conditionals, the MSVC include-stack rule, pragma dispatch, predefined macros, `/E` output. `#embed` waits for phase 3. Likely two PRs: lexer and macros, then includes, pragmas and predefined macros | Lit tests; `/E` output differential against `clang -E` on a checked-in corpus, with whitespace normalised |
| 1.6 | Walking skeleton | Parser, sema and IRGen for integer and pointer scalars, functions, statements and expressions, integer constant evaluation, declarations and linkage; a minimal `ncc /c /Fo` driver | The first C programs compile, link with `link.exe` and run on `windows-arm64`; the `c-testsuite` cases that include no headers |
| 1.7 | Aggregates and layout | `struct`, `union`, `enum`, MSVC layout including bit-fields and `#pragma pack` (04), initializers (designated), compound literals, string literals in UTF-8/16/32 | Layout probes identical to `cl` on `windows-arm64`, and to `clang --target=aarch64-pc-windows-msvc` on Linux |
| 1.8 | Floating point | bigint, correctly rounded decimal-to-binary conversion (Eisel–Lemire with a big-decimal fallback, 03 §2), floating types through sema, IRGen and instruction selection | Conversion tested against the host C library's `strtod` as a test-only oracle (never in the build, R15), plus boundary cases; FP execution tests on Windows |
| 1.9 | ABI completion | Aggregates by value, HFA/HVA, `sret`, variadics and `va_list` (07); the generator `tests/abi/gen.c` with a fixed corpus of a few hundred cases (14 §4) | All four caller/callee pairings (`ncc`/`cl`) pass on `windows-arm64`, and layout probes agree |
| 1.10 | UCRT compatibility and driver | `__declspec`, SAL tolerance, MSVC `__inline` semantics, the MSVC keywords of 03 §4; `INCLUDE` discovery; the `cl` option subset phase 1 needs; `link.exe` invocation (12) | `stdio.h`, `stdlib.h`, `string.h`, `math.h`, `ctype.h` and `time.h` compile; a `printf` program builds with `ncc` alone and runs |
| 1.11 | CodeView for C | Line tables from real source, `S_GPROC32_ID` per function, SHA-256 file checksums; full types wait for phase 3 | `llvm-pdbutil` on Linux; on Windows, a scripted `cdb` step-by-line session (14 §6) |
| 1.12 | Conformance | `c-testsuite` to 100%; the curated GCC torture subset, checked in as test input only (Q-01); then Lua and SQLite | The phase-1 exit criterion in §2 |

1.7 and 1.8 are independent and may swap. 1.11 may move earlier: once 1.6 runs, a debugger session is the cheapest way to see what a compiled program does.

## 6. Critical path

The longest dependency chain is phase 0 → 1 → 3 → 5, with phase 4 (MSVC compatibility breadth) as the largest *uncertain* block (R1) running alongside 3. The earliest point at which the compiler is useful to someone else is the end of phase 4 at `-O0`; the earliest point at which it can be compared with MSVC on its own terms is the end of phase 5.
