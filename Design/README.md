# NeuronCompiler — Design

NeuronCompiler (`ncc`) is a self-hosting C23 compiler, written in C23, that targets Windows on ARM64 and emits COFF object files consumed directly by `link.exe` from Visual Studio 2026 (MSVC toolset 14.50). It does not emit assembly for an external assembler and it does not ship a linker: the object file is the product.

This directory is the living design. Documents are numbered in reading order; each one ends with the open items it raises. Contested points are collected in [16-open-questions.md](16-open-questions.md) so we can settle them explicitly rather than by drift.

## Documents

| # | Document | Covers |
|---|----------|--------|
| 01 | [Goals, scope and risks](01-goals-scope-risks.md) | What "full C23" means here, conditional features, non-goals, risk register |
| 02 | [Architecture](02-architecture.md) | Pipeline, components, source tree, core library conventions, determinism |
| 03 | [Front end](03-frontend.md) | Source manager, lexer, preprocessor, parser, AST, semantic analysis, constant evaluation |
| 04 | [MSVC compatibility](04-msvc-compatibility.md) | Data model, struct/bit-field layout, `__declspec`, keywords, intrinsics, predefined macros |
| 05 | [IR and optimiser](05-ir-and-optimizer.md) | SSA IR, IR generation, pass pipeline, UB/aliasing/FP policy |
| 06 | [ARM64 back end](06-arm64-backend.md) | Instruction selection, MIR, register allocation, frames, encoding |
| 07 | [Windows ARM64 ABI](07-windows-arm64-abi.md) | Calling convention, variadics, HFAs, `_BitInt`, stack probing, TLS, dllimport |
| 08 | [COFF object writer](08-coff-object-writer.md) | Sections, COMDATs, symbols, relocations, directives, bigobj |
| 09 | [Unwind data and SEH](09-unwind-and-seh.md) | `.pdata`/`.xdata` generation, unwind verifier, `__try`/`__except`, `setjmp` |
| 10 | [CodeView debug info](10-codeview.md) | `.debug$S`/`.debug$T`, line tables, types, variable locations |
| 11 | [Runtime and headers](11-runtime-and-headers.md) | Compiler-provided headers, `libncrt`, UCRT gap handling |
| 12 | [Driver and toolchain integration](12-driver-and-toolchain.md) | Command line, environment discovery, link.exe, MSBuild/CMake |
| 13 | [Self-hosting and bootstrap](13-self-hosting-bootstrap.md) | Implementation dialect, bootstrap stages, reproducibility |
| 14 | [Testing strategy](14-testing-strategy.md) | Conformance, ABI cross-testing, unwind and debug verification, fuzzing |
| 15 | [Roadmap](15-roadmap.md) | Phases, exit criteria, effort estimate |
| 16 | [Decisions and open questions](16-open-questions.md) | The decision log for questions raised by the design, plus anything still open |

## Baseline decisions

These were fixed at the outset and the rest of the design assumes them.

| ID | Decision | Consequence |
|----|----------|-------------|
| D-01 | Implementation language is C23, and the compiler self-hosts. | No C++ templates or RAII; a small in-house core library (arenas, vectors, hash maps, interning) is a first-class component. Stage-0 needs a C23-capable host compiler (Clang ≥ 18 or GCC ≥ 13, `-std=c2x`); MSVC `cl` is not a supported stage-0. |
| D-02 | Optimisation target is an SSA mid-end of roughly -O1/-O2 class. | Typed SSA IR with block parameters, a classic scalar pipeline, inlining, linear-scan register allocation with splitting. No auto-vectorisation or LTO in v1. |
| D-03 | The compiler must consume the real Windows SDK and UCRT headers. | An MSVC compatibility layer (layout, `__declspec`, intrinsics, `_MSC_VER`) is in scope from phase 2 and is the largest schedule risk. |
| D-04 | CodeView debug info is part of v1. | Full `/Z7`-style `.debug$S`/`.debug$T` emission; `link.exe /DEBUG` produces the PDB, so we never write PDBs ourselves. |
| D-05 | Single target: Windows ARM64 (`IMAGE_FILE_MACHINE_ARM64`, 0xAA64). | No retargetable back-end abstraction beyond what good engineering needs; ARM64EC and ARM64X are explicit non-goals. |
| D-06 | Direct object emission; no external assembler. | The compiler owns instruction encoding, fixups and relocations. A `-S` listing exists for humans and for differential testing, not as a build path. |
| D-07 | No build-time dependency other than a C23 compiler. | No Python, TableGen or CMake required to build `ncc`; tables are X-macros in C. |

## Status

Design draft 2, November 2026: the open questions from draft 1 are decided (see [16-open-questions.md](16-open-questions.md)), except Q-19, which gets settled by inspecting `cl` output. The project stays private for now (Q-01). Claude implements and you review (Q-02). Phase 0 (the back-end tail: encoder, COFF, unwind, minimal CodeView) is implemented and tested on Linux; its Windows acceptance steps are pending, see [15-roadmap.md](15-roadmap.md) §4.
