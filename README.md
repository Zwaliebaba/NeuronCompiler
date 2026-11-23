# NeuronCompiler

A self-hosting C23 compiler, written in C23, that targets Windows on ARM64 and
emits COFF objects for `link.exe` from Visual Studio 2026. The design is in
[`Design/`](Design/README.md); start there.

## Status

Phase 0 of the [roadmap](Design/15-roadmap.md) is implemented. That is the back-end
tail of the pipeline, driven by hand-built machine code rather than a front end:

| Component | Where | What it does |
|-----------|-------|--------------|
| Core library | `src/core` | Arenas, typed vectors, byte buffers, CRC-32, host OS layer |
| ARM64 instruction table | `src/target/arm64/arm64.def` | 224 instructions; the single source of opcodes, encodings and printing |
| Encoder and printer | `src/target/arm64/a64_enc.c`, `a64_print.c` | Binary encoding with COFF relocations; LLVM-syntax listing |
| Frames and unwind data | `src/target/arm64/unwind.c` | Canonical prologues and epilogues, packed `.pdata` or full `.xdata`, and a verifier that unwinds from every prologue and epilogue instruction |
| Module emission | `src/target/arm64/a64_module.c` | Pseudo expansion, branch relaxation, COMDAT text with associative unwind and debug sections |
| COFF writer | `src/obj/coff` | Regular and bigobj formats, relocation overflow, `.drectve`, `@feat.00` |
| CodeView | `src/debug/codeview` | Object and compiler records, function symbols, line tables, type records |
| `p0demo` | `src/tools/p0demo.c` | Builds two demo programs: one without a CRT (kernel32 only), one for the MSVC CRT (`printf`, TLS) |

There is no C front end yet. That is phase 1.

## Building and testing (Linux or macOS host)

Needs Clang ≥ 18 or GCC ≥ 13. The tests also use `llvm-mc`, `llvm-readobj`,
`llvm-objdump`, `llvm-pdbutil`, `llvm-dlltool` and `lld-link`. Tests that
need a missing tool are reported as skipped.

```bash
make
```

```bash
make test
```

```bash
make test-link
```

`make test` runs the unit tests. These include encoding every instruction form
and comparing the bytes with `llvm-mc`, and sweeping 38,016 frame shapes through
the unwind verifier, with mutation tests that check it rejects broken code.
`make test-link` generates the demo objects and checks they are reproducible.
It then links the no-CRT program with `lld-link` and inspects the image, unwind
data and PDB with LLVM's tools.

## Windows on Arm acceptance

From an *ARM64 Native Tools Command Prompt for VS 2026*:

```bat
tests\windows\p0-accept.cmd
```

This builds the tools with `clang` via `build.cmd`, links both demo programs
with `link.exe`, runs them and checks their output. Instead of building on
Windows, you can pass a directory containing `p0_nocrt.obj` and `p0_crt.obj`
from a Linux build or the CI artifact. The objects are byte-identical on every
host.
