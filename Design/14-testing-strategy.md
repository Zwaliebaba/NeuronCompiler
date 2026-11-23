# 14 — Testing strategy

A compiler is only as trustworthy as its test oracle. There is no free official C23 conformance suite, MSVC is not a C23 reference, and Clang/GCC are not Windows-ABI references for everything. So the strategy combines several oracles, each covering what the others cannot.

## 1. Test layers

| Layer | What it tests | Oracle | Where it runs |
|-------|---------------|--------|---------------|
| Unit | Core library, bigint, float conversion, encoder forms, unwind packer, layout engine | Expected values in tests | Every build, every host |
| FileCheck-style | Front-end diagnostics, `-E` output, NIR after each pass, MIR after isel/RA, `-S` output | Checked-in expectations with pattern matching (`tests/lit`, a small in-house runner written in NC-Boot C) | Every build |
| Encoding differential | Every instruction form × operand class | `llvm-mc --show-encoding` | CI (Linux) |
| Object structure | Every `.obj` produced by any test | `ncc-objcheck`, `llvm-readobj`, `dumpbin` | CI |
| Execution | Compiled programs produce expected output | Expected output files | Windows ARM64 runners |
| ABI cross-compile | Calls and data between `ncc` and `cl` objects | Mutual agreement, generated tests | Windows ARM64 runners |
| Unwind | Every prologue/epilogue boundary | Static verifier + `RtlVirtualUnwind` harness | CI + Windows |
| Debug info | Debugger-visible behaviour | Scripted `cdb` sessions | Windows ARM64 runners |
| Differential random | Whole-program semantics | Csmith / YARPGen programs: `ncc -O0` vs `ncc -O2` vs `clang-cl` vs `cl` | Nightly |
| Fuzzing | Robustness | No crash, verifier clean | Continuous (libFuzzer-style harness built with Clang stage 0) |
| Real-world | Large codebases build and pass their own tests | Projects' own test suites | Nightly |
| Bootstrap | Self-compilation fixed point | Stage 2 == stage 3 objects | Every merge |
| Performance | Runtime and compile time | Tracked against baseline and `cl /O2` | Nightly |

## 2. Conformance

The conformance matrix (`tests/conformance/MATRIX.md`, generated) lists every C23 clause with normative requirements, mapped to the tests that exercise it; a clause with no test is red. Sources of tests:

* Our own clause-by-clause tests, written alongside each feature (positive and constraint-violation cases).
* The `c-testsuite` corpus and the execution tests of the GCC torture suite (`gcc.c-torture/execute`, GPL test data used only as test input, not linked into anything we ship).
* Tests adapted from Clang's `test/C/C23` and `test/Sema` C files (Apache-2.0 with LLVM exception), for C23 features specifically.
* Commercial suites (Plum Hall, Perennial, SuperTest) if budget allows: they are the only independent conformance oracles and would be the strongest evidence for a "full C23" claim. Recommended before any external claim of conformance (Q-25).

## 3. Execution environment

Execution tests need Windows on ARM64. CI uses hosted Windows 11 ARM64 runners where available plus at least one self-hosted Arm64 device (Snapdragon X class) for debugger, performance and unwind-harness tests, which need a stable interactive environment. Linux CI runs everything that does not need execution: front end, IR, encoding, object structure, and links with `lld-link` against a locally provisioned SDK as an early warning (final acceptance is always `link.exe`).

## 4. ABI and layout cross-checking

This is the test layer most likely to catch the bugs that would otherwise surface only in users' mixed `ncc`/`cl` builds. A generator (`tests/abi/gen.c`) produces, from a seed, a set of struct/union types (mixing integer, float, `double`, pointer, bit-field, array and nested members, with random `#pragma pack` and `alignas`), and functions taking and returning them in random positions, including variadic functions. For each case it emits:

1. A *layout probe*: `sizeof`, `alignof` and `offsetof` (plus bit-field bit positions via union-punning) for every type and member, printed.
2. A *caller TU* and a *callee TU*: the caller constructs values, calls, and checks the callee's return; the callee checks every received argument byte-for-byte and returns a constructed value.

Each case is compiled four ways — caller and callee each with `ncc` and `cl` — and linked into one executable: (ncc→ncc), (ncc→cl), (cl→ncc), (cl→cl). All four must pass, and the layout probes from `ncc` and `cl` must print identical output. The same generator in `_BitInt`/`_Complex` mode cross-checks against `clang-cl --target=aarch64-pc-windows-msvc`, since `cl` lacks those types. Thousands of cases run nightly; a fixed corpus of a few hundred runs on every change.

The same harness checks TLS (a `thread_local` defined by one compiler and accessed by the other, across threads and across a DLL boundary), dllimport/dllexport in both directions, and SEH unwinding through mixed frames.

## 5. Unwind testing

Described in [09-unwind-and-seh.md](09-unwind-and-seh.md) §3: the static verifier on every compiled function, plus a Windows harness that loads a test DLL, single-steps every function under the debug API, and at every instruction calls `RtlVirtualUnwind` on the captured context and checks that the unwound `sp`, `pc` and non-volatile registers match those recorded at the call. A third test throws SEH exceptions (`RaiseException`, access violation) from deep inside `ncc`-compiled call chains with every frame shape and checks that `__except` in a `cl`-compiled caller catches them and that `__finally` blocks in between run.

## 6. Debug-info testing

Scripted `cdb` sessions: each test is a C file with annotations (`// BREAK`, `// EXPECT: x = 5`, `// EXPECT-STACK: f, g, main`) that a runner turns into a debugger script and checks. Run at `-O0` (must be exact) and `-O2` (expectations marked as "if available": a variable may be optimised away, but if shown, its value must be correct). `llvm-pdbutil dump` on the linked PDB checks structural invariants (every type index resolves, every symbol has a valid scope).

## 7. Real-world corpus

Projects chosen to stress different parts of the compiler and to have their own test suites: SQLite (amalgamation: big TU, heavy optimisation), Lua (interpreter dispatch, `setjmp`), zlib and zstd (bit manipulation, performance), libpng, curl (Win32 API, `windows.h`), cJSON, stb libraries (macro-heavy single-header), QuickJS (large, C11/C2x features), the Windows-classic samples from the SDK (GUI, `windows.h` breadth), and `ncc` itself. Nightly: build each with `ncc` at `-O0` and `-O2`, run its tests, compare against the `cl` build.

## 8. Performance tracking

Runtime: a benchmark set (SQLite speedtest1, Lua benchmarks, zstd compression, CoreMark, a subset of Embench and the integer and floating-point kernels from Polybench) measured against `cl /O2` on the same hardware; the target in [01-goals-scope-risks.md](01-goals-scope-risks.md) §1 is geometric mean ≤ 1.4× runtime. Compile time: `windows.h` empty TU, SQLite amalgamation at `-O0`/`-O2`, and the self-build, tracked per commit with regressions over 3% flagged.

## Open items from this document

Commercial conformance suite (Q-25).
