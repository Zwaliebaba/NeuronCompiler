# AGENTS.md — Engineering Rules

Operating instructions for every agent (and human) writing code in this repository. **Read this before generating a single line.**

This repository is **NeuronCompiler** (`ncc`): a self-hosting C23 compiler, written in a restricted C23 dialect (NC-Boot), that targets Windows on ARM64 and emits COFF objects for `link.exe`. It is developed on Linux, macOS and Windows hosts and built by Clang or GCC until it can build itself. This file is about **how code is written here** — naming, layout, build settings and the standing rules of the codebase. It is not the design: what the compiler *is* lives in [`Design/`](Design/README.md), and that is where to start.

**Where these rules come from.** The naming and layout conventions are carried over from two sibling repositories, `Outpost.Warzone`, where the formatter and linter settings were measured against roughly 223,000 lines, and `Nomad-Commander`. Those trees are C++ and this one is C, so every rule here has been restated for C: the C++-only parts (namespaces, classes, access control, templates, RAII) are gone, and where C forces a different answer — enumerators share one namespace, a constant is an anonymous enum, a struct field has no access specifier — the rule says so. **What did not come across is the other trees' design, their decisions or their plan.** A decision taken there binds nothing here.

**The tree is not yet conformant.** Phase 0 (the back-end tail: encoder, COFF writer, unwind, minimal CodeView) was written before these files arrived, in a different style. §8 says what that means, what the plan is and what to do in the meantime. Everything else in this file describes code as it must be written.

**What is authoritative, in order:**

1. **This file** — conformance: naming, style, build settings, and how to work here.
2. **[`Design/`](Design/README.md)** — what is built: the baseline decisions `D-01`…`D-07` in `Design/README.md`, the numbered component documents, and the decision log in [`Design/16-open-questions.md`](Design/16-open-questions.md).
3. **The surrounding code** — for anything neither of the above covers, match the file you are editing.

The design sits alongside this file rather than below it: it says what is built and this file says how. Where a design document spells an identifier in another style (`nc_assert`, `arena_alloc`), it is naming the thing, not prescribing its spelling. A task that needs a design answer the documents do not give asks the owner, and gets the answer written into `Design/` before the code is (§6).

If a rule here conflicts with a habit from another codebase, this file wins. If you think a rule is wrong or your task cannot be done without deviating, **say so in your report — never deviate silently.**

---

## 1. Naming convention (normative — no exceptions)

| Kind | Convention | Example |
|---|---|---|
| Type (struct, union, enum, typedef) | `PascalCase` | `CoffSection`, `ArenaMark` |
| Function | `PascalCase`, led by its module | `ArenaAlloc()`, `CoffAddReloc()` |
| Struct or union field | `camelCase` | `bytesReserved` |
| Variable at file scope, or function-local `static` | `g_camelCase` | `g_testFailures` |
| Parameter | `_camelCase` | `_arena`, `_sizeBytes` |
| Local | `camelCase` | `chunkBytes` |
| Compile-time constant — anonymous-enum enumerator, `const` object at file scope, `static const` local | `UPPER_CASE` | `ARENA_CHUNK_BYTES`, `A64_INSN_INFO` |
| Enumerator of a named enum | `PascalCase`, led by its type | `A64CondEq`, `A64KindXsp` |
| Macro, include guard | `UPPER_CASE` | `NC_ASSERT`, `VEC(T)`, `NC_ARENA_H` |
| Fixed-width shorthand (the one exception) | as in `core/core.h` | `u8` `u16` `u32` `u64` `i8` `i16` `i32` `i64` |
| File | lowercase `snake_case.c` / `.h` | `a64_enc.c`, `coff.h` |
| Directory | lowercase | `src/target/arm64` |

**Note the split that catches people out: a constant is `UPPER_CASE`, an enumerator of a named enum is `PascalCase`.** They are both compile-time and they are spelled differently on purpose — an enumerator is a *value of a type* and reads as one at the use site (`A64CondEq`), while a constant is a number with a name and is meant to look like one. C adds a twist: NC-Boot has no `constexpr` objects yet (Design/13 §1), so **an anonymous enum is how a constant is written**, and its enumerators are constants, not values of a type:

```c
enum
{
  ARENA_CHUNK_BYTES = 64 * 1024 // anonymous: a constant → UPPER_CASE
};

typedef enum A64Cond : u8
{
  A64CondEq, // named: a value of A64Cond → PascalCase, led by the type
  A64CondNe,
} A64Cond;
```

[`.clang-tidy`](.clang-tidy) enforces the table, and it is the single source of truth for the option values; this document states the rules in prose and does not repeat the settings, so there is nothing to drift. It cannot tell a named enum from an anonymous one, so it accepts either shape for an enumerator and review checks which one applies.

### The rules behind the table

**R1 — The leading underscore on parameters is deliberate.** It is legal C: C23 §7.1.3 reserves `_Uppercase` and anything beginning with `__` for any use, and `_lowercase` **only at file scope**. A parameter has block or prototype scope, never file scope, so `_fileName` is safe. Never introduce a reserved form — no `_Impl`, no `__helper`, no file-scope `_cache` (use `g_cache`, and `static`).

**R2 — A type name carries no prefix or affix.** PascalCase means the name and nothing else. This bans `CFoo`, `SFoo`, `EFoo`, `IFoo`, `FooBase`, `FooImpl`, and the `_t` suffix (which POSIX reserves anyway). The module word that leads a C name — `Coff` in `CoffSection`, `A64` in `A64Op` — is not an affix: it is C's namespace (R9). An interface in C is a struct of function pointers, and it is named for the concept, not `IWriter` or `WriterVtbl`; the implementations say what they are. A dispatch struct with one implementation is ceremony: call the function, and add the indirection when a second implementation needs it.

clang-tidy can require a case but cannot see a banned affix, so the repository checker carries that half (§6).

**R3 — Compile-time constants are `UPPER_CASE`.** That covers anonymous-enum enumerators, `const` objects at file scope (lookup tables included: `A64_INSN_INFO[]`), `static const` locals, and `constexpr` objects once NC-Boot allows them. A `const` local in a function body is a local and stays `camelCase` — it is a value computed at run time that happens not to change. Mutable state at file scope is `g_` and rare: the compiler core is single-threaded per process (Design/02 §1), but a global is state every function can reach and every test has to reset, so say in a comment why it is global.

**R4 — Acronyms capitalize as words**: `CoffReloc`, `CvTypeRecord`, `Crc32`, `Sha256`, `MirBlock` — never `COFFReloc`. Identifiers whose spelling is fixed outside this tree keep it and are never renamed to fit:

- the PE/COFF specification: `IMAGE_FILE_MACHINE_ARM64`, `IMAGE_SCN_CNT_CODE`, `IMAGE_REL_ARM64_BRANCH26`;
- CodeView: `S_GPROC32`, `LF_PROCEDURE`, `CV_CFL_ARM64`;
- the C standard, the UCRT and the ABI: `main`, `memcpy`, `__chkstk`, `va_list`;
- Win32 functions the OS layer declares by hand: `MoveFileExA`;
- the instruction ids in `arm64.def` (`ADDXri`, `LDRXui`), which are LLVM's opcode names. An enumerator generated from a table is its type's name followed by the table's id — `A64OpADDXri` — so the external spelling survives inside a PascalCase name.

**R5 — Type parameters of generator macros are single capitals or PascalCase**: `VEC(T)`, `HMAP(K, V)`, `ARENA_NEW(a, T)`. The macro itself is `UPPER_CASE`.

**R6 — Units belong in names; types do not.** `sizeBytes`, `alignBytes`, `offsetBytes`, `frameBytes`, `sizeWords`, `accessSizeLog2`, `bitWidth` are encouraged. A back end is a machine for converting between units — byte offsets, 4-byte instruction words, immediates scaled by the access size, unwind codes that count in 8- or 16-byte steps, branch ranges in words — and confusing two of them is a real defect class that the compiler cannot catch for you and that a test only catches at the boundary. A field called `sz` that holds the log2 of an access size is the case this rule exists for. Never encode the type: no `pArena`, `uCount`, `szName`.

**R7 — Files and directories are lowercase.** `.c` and `.h`, named for the module they hold, in `snake_case`: `arena.c`, `a64_enc.c`, `os.h`. Two more kinds exist, both from Design/02 §3: **`.def`** for an X-macro table (`arm64.def`) and **`.inc`** for an include-with-parameters template (`core/hmap.inc`). Nothing else — no `.cpp`, `.hpp` or `.inl`; this tree is C. The rule is lowercase rather than PascalCase because the tree is built on case-insensitive Windows and case-sensitive Linux: an include whose case does not match the file compiles on one host and fails on the other, and an all-lowercase tree makes that mistake impossible to write. Executables are `ncc` and `ncc-<tool>` (`ncc-test`, `ncc-uwcheck`).

**R8 — Struct and union fields are plain `camelCase`, with no `m_`.** In the C++ trees `m_` marks encapsulated class state and a public aggregate's fields go unprefixed. C has no access control, so every field is the second kind. Encapsulation in C is an *opaque* struct — declared in the `.h`, defined in the `.c`, like `ArenaChunk` — and its fields are `camelCase` too. A record whose layout is fixed by a format (a COFF symbol, a CodeView record) names its fields for the meaning; the specification's spelling goes in a comment.

**R9 — One module word per module, and the edges run one way.** C has no namespaces, so the module word leading every name with external linkage *is* the namespace: everything `arena.h` exports begins `Arena`, everything `coff.h` exports begins `Coff`. Anything not used outside its `.c` is `static`. It also keeps a name clear of Win32's PascalCase API, which the OS layer declares by hand in the same translation unit. The layering is the design's (Design/02 §2): `core` is below everything, the pipeline's dependencies point strictly forward, **the back end never sees the AST and the IR never refers to sema types**. If a back-end function needs a front-end concept by name in order to do its job, it is in the wrong layer.

**R10 — Every header is self-contained, include-guarded and minimal.** The guard is `NC_<FILE>_H` (`NC_ARENA_H`); `#pragma once` is not used. A `.c` includes its own header first, which is what proves the header compiles on its own. A header includes what its declarations need and nothing for the convenience of its includers, and a forward declaration (`typedef struct ArenaChunk ArenaChunk;`) beats an include when a pointer is all that is needed.

**R11 — One spelling per family, and it is the C standard's.** `initializer`, `serialize`, `normalize`, `optimize`, `behavior`, `color`, `center`, `canceled`. Neither spelling is wrong English; the defect is a tree where a reader has to know which half they are in and a grep for one finds half the uses. The C standard writes *initializer* and the PE/COFF specification writes `IMAGE_SCN_CNT_INITIALIZED_DATA`, which settles which half wins. Prose is not checked — the design documents and the comments say *optimiser* and *serialisation*, and may go on doing so; an identifier says `Optimizer` and `Serialize`.

### Worked example — this is the target style

This is `core/arena.h` and a fragment of `core/arena.c` as they read after conversion (§8). It is formatted by the repository's `.clang-format` and is clean under its `.clang-tidy`; `ArenaLifetime` is an illustration, not a planned type.

```c
/* arena.h — region allocator (Design/02-architecture.md §3). */
#ifndef NC_ARENA_H
#define NC_ARENA_H

#include "core.h"

/* R3: an anonymous-enum enumerator is NC-Boot's compile-time constant → UPPER_CASE.
 * R6: the unit is in the name. */
enum
{
  ARENA_CHUNK_BYTES = 64 * 1024
};

/* A named enum is a taxonomy: PascalCase enumerators, led by the type's name,
 * because C enumerators share one namespace. */
typedef enum ArenaLifetime : u8
{
  ArenaLifetimeSession,
  ArenaLifetimeUnit,
  ArenaLifetimeFunction
} ArenaLifetime;

typedef struct ArenaChunk ArenaChunk; /* opaque: defined in arena.c */

/* R2: no prefix on the type. R8: C has no access control, so fields are plain camelCase. */
typedef struct Arena
{
  ArenaChunk* head;
  u8* cur;
  u8* end;
  size_t bytesReserved; /* R6 */
  ArenaLifetime lifetime;
} Arena;

/* R9: the module leads the name. R1: `_` on parameters. */
void ArenaInit(Arena* _arena, ArenaLifetime _lifetime);
[[nodiscard]] void* ArenaAlloc(Arena* _arena, size_t _sizeBytes, size_t _alignBytes);
[[nodiscard]] char* ArenaStrdup(Arena* _arena, const char* _text); /* R17: a string you do not write is const */

#define ARENA_NEW(a, T) ((T*)ArenaAlloc((a), sizeof(T), alignof(T)))

#endif
```

```c
/* arena.c */
#include "arena.h"

#include <string.h>

/* A file-scope `const` table is a constant (R3); mutable file-scope state is g_ and rare. */
static const char* const LIFETIME_NAMES[] = {"session", "unit", "function"};
static u32 g_chunkCount;

void ArenaInit(Arena* _arena, ArenaLifetime _lifetime)
{
  memset(_arena, 0, sizeof *_arena);
  _arena->lifetime = _lifetime;
  ...
}
```

### Enforcement

| Rule | Enforced by |
|---|---|
| The naming table, R1, R3, R5, R8 | [`.clang-tidy`](.clang-tidy) — `make tidy`, to be gated in CI over `src/` and `tests/` |
| R2 affixes, R7 file and directory names, R11 spellings, the enumerator shape (named vs anonymous), build and test registration (§2) | the tree checker, `make check-tree`, to be gated in CI |
| R4, R6, R9, R10 | Review. Check your own diff against the table before handing it back. |

**None of the three gates exists yet** (§6, §8). `.clang-tidy` is configured and verified against clang-tidy 22.1.8, but nothing runs it in CI, and the Makefile has no `tidy`, `check-format` or `check-tree` target. Until each lands, the rules in its row are review's problem and nothing else. A rule nobody can run is a rule that rots, so writing them is early work rather than housekeeping.

---

## 2. Repository shape

The layout is the design's ([Design/02-architecture.md](Design/02-architecture.md) §2): one directory per component under `src/`, nested where the design nests them, with the compiler-provided headers in `include/`, `libncrt` in `runtime/` and the test suites in `tests/`. Today `src/` holds `core`, `target/arm64`, `obj/coff`, `debug/codeview` and `tools`; the other components arrive with the phases that build them.

**Headers live beside their `.c`, at whatever depth the component sits.** There is no `include/` split inside a component — the top-level `include/` is for headers `ncc` *ships*, not for its own. `.clang-tidy`'s `HeaderFilterRegex` matches every `.h` under `src/` and `tests/` at any depth, so nesting costs nothing; a header anywhere else is not linted.

**Includes are quoted and relative to the including file** — `#include "../../core/arena.h"` — and there are no `-I` flags anywhere. That is what lets every source compile with a plain `cc -std=c2x -c file.c` (D-07), and what lets `make check-windows` and clang-tidy run with no project-specific switches. Do not add an include directory.

**Platform headers appear only in the OS layer** (Design/13 §1, §5). The compiler does not include `windows.h` at all: the OS layer declares the few `kernel32` functions it needs itself. Today that layer is `src/core/os.c`, with its `_WIN32` half behind `#if defined(_WIN32)`; the design names the eventual split `core/os_win32.c` and `core/os_posix.c`. The one sanctioned exception is a test tool that exists to compare against the operating system — `src/tools/uwcheck.c` includes `windows.h` to call `RtlVirtualUnwind` — and it says so in a comment. The compiler proper never takes that exception.

**A table lives in exactly one `.def` file** (Design/02 §3, D-07). Instruction encodings, IR opcodes, intrinsic signatures, diagnostics, CodeView record kinds and keywords are each an X-macro table, expanded wherever the table is needed — the enum, the name array, the encoder, the printer, the test. A fact written down in two places drifts. A `.def` is aligned into columns by hand and is not run through clang-format.

**The build files are a registry, and there are two of them.** The [`Makefile`](Makefile) and [`build.cmd`](build.cmd) each list the source *directories* and glob within them, so a new file in an existing directory is picked up by both. **A new directory must be added to both** — `LIB_SRC` in the Makefile (which `check-windows` inherits) and the `for` loop in `build.cmd`. A directory in one and not the other builds on one host and silently not on the other.

**Tests register by hand.** A suite in `tests/unit/` is a `test_<module>.c` with one entry point, declared in `tests/unit/test.h` **and** listed in the `tests[]` table in `tests/unit/test_main.c`. An entry point missing from the table compiles, links, and never runs — `make test` stays green over code nobody exercised.

**There is no third-party code in the compiler and no package manager.** See R14.

**Build output is never committed** — `build/`, `build-*/`, `*.ncc-tmp` (see `.gitignore`). Because `/build/` is the output directory and Windows is case-insensitive, **no source directory may be named `build` or `Build`**: on a Windows checkout the ignore rule swallows it.

---

## 3. Build and verify

**Windows ARM64 is the only target** (D-05): `IMAGE_FILE_MACHINE_ARM64`, no ARM64EC, no ARM64X, no x64 back end. **The host is anything** a supported stage-0 compiler runs on — Linux and macOS for development and CI, Windows x64 or ARM64 — and the output must be byte-identical across hosts (Design/13 §3). Do not write code whose result depends on the host: no `long` (LP64 against LLP64), no host `strtod`, no host locale, no unstable `qsort`, no reliance on the signedness of plain `char` (signed on x64, unsigned on Linux ARM64).

**The stage-0 compilers are Clang ≥ 18 and GCC ≥ 13, in `-std=c2x` mode, and both are supported.** MSVC `cl` is not a stage-0 (D-01). The settings are the settings: `-std=c2x -Wall -Wextra -Werror -Wshadow -Wno-unused-parameter`, as the Makefile states them. If a build error tempts you to lower the standard, drop `-Werror`, add a `-Wno-…` or silence a warning with a `#pragma`, **stop and report instead.**

**Build with both compilers before you claim anything.** CI builds and tests with each, and the roadmap holds the tree to *byte-identical output from GCC-built and Clang-built tools*. A change that passes under one compiler and was never built with the other is half verified, and the report says which half.

**The Makefile and `build.cmd` agree by rule, not by luck.** They state the same standard and the same warnings. The only sanctioned difference is what the host forces — `-D_CRT_SECURE_NO_WARNINGS`, because the UCRT deprecates the ISO functions the tree uses. (At the time of writing `build.cmd` lacks `-Wshadow`: that is a defect to fix, not a second sanctioned difference.) CI never runs `build.cmd`, so the two drifting apart is found only by whoever next builds on Windows.

```sh
# Linux or macOS, from the repository root.
make                                   # build/ncc-test, build/p0demo, build/ncc-uwcheck
make test                              # unit tests, including the llvm-mc encoding differential
make test-link                         # lld-link, LLVM's readers and ncc-uwcheck --strict on the linked image
make check-windows                     # every source syntax-checked for Windows ARM64 against mingw-w64

# The other stage-0, and the sanitizers, each in its own output directory.
make CC=gcc BUILD=build-gcc test
make BUILD=build-asan OPT="-O1 -g -fsanitize=address,undefined" test
```

```bat
rem Windows, from an ARM64 Native Tools Command Prompt for VS 2026.
build.cmd
tests\windows\p0-accept.cmd
```

**A skipped test is not a passed test.** A unit test that needs a missing tool (`llvm-mc`, say) reports itself as skipped and the run still exits 0; `make test-link` exits 0 with `link test SKIPPED` if any of the LLVM tools it needs is absent. Read the output. CI installs every tool, so a skip in CI is a CI defect; a skip on your machine goes in your report as a skip.

**A green Linux run says nothing about whether `link.exe` accepts the object or Windows can unwind through it.** `lld-link`, `llvm-readobj`, `llvm-pdbutil` and our own unwinder are independent cross-checks, not the target. For anything touching object emission, relocations, unwind data, CodeView or the generated code, run `make test-link`, and where you can, `tests/windows/p0-accept.cmd` on Windows on Arm — it links with `link.exe`, runs the programs and checks every function against `RtlVirtualUnwind`. CI runs that acceptance on every push and pull request, in the `windows-arm64` job (§6). Locally, say which of the two you ran; on a PR, say whether the `windows-arm64` job was green on its head.

**Run the checkers before you push**, once they exist (§6): `make check-format`, `make tidy`, `make check-tree`.

**Report what you actually did.** "Builds clean with Clang, not tested with GCC", "unit tests pass, link test skipped (no `lld-link`)" and "accepted on Windows on Arm" are different claims. Never imply a stronger one than you made, and say which host and which compilers.

---

## 4. Layout and formatting

[`.clang-format`](.clang-format) is the authority for C layout: 2-space indent, 140 columns, Allman braces, pointer bound left (`Arena* _arena`), includes never reordered. [`.editorconfig`](.editorconfig) covers everything clang-format does not — line endings, UTF-8, final newline, trailing whitespace, the tab-indented Makefile and the other non-C formats — and repeats the two numbers an editor needs before the first save. [`.gitattributes`](.gitattributes) pins the working-tree line ending of each format so that every checkout on every host holds the same bytes: CRLF for C, the `.def` tables and the `cmd` scripts; LF for the Makefile, shell, YAML and Markdown.

**The phase-0 tree is not formatted to `.clang-format` yet** (§8). Until the conversion lands, **write every new file to it, and do not run the formatter over an existing file** as part of an unrelated change: the whole-file diff buries the change it came with. After the conversion the tree is formatted, CI keeps it that way, and if the check fires you run the formatter and commit the result rather than arguing with it.

- **Do not reformat what your task did not touch.** A drive-by reformat produces pure churn and buries your actual change.
- **Include order is grouped by hand**, which is why `SortIncludes` is `Never`: in a `.c`, its own header first (R10), then other project headers, then the standard library. The own-header-first position is the header's self-containment check, and in the include-with-parameters pattern (`#define HMAP_K …` then `#include "core/hmap.inc"`) the position of an include is part of its meaning. A formatter reordering these behind a change's back is a correctness risk, not a style preference.
- **X-macro tables are hand-aligned and never formatted.** A run of hand-aligned rows inside a `.c` or `.h` is fenced with `// clang-format off` / `// clang-format on`, and nothing else is.
- Do not silence a diagnostic with `#pragma GCC diagnostic`, `#pragma clang diagnostic` or a new `-Wno-…` to make a build pass. Fix the cause, or report it. (Those pragmas are compiler-specific, and NC-Boot does not allow them anyway.)

---

## 5. Rules for this codebase

These are conformance rules with a design source. Each cites it; if the design changes, the rule changes with it.

**R12 — Compiler sources are NC-Boot** ([Design/13](Design/13-self-hosting-bootstrap.md) §1). All of C17, plus the C23 features listed there: `bool`, `nullptr`, `static_assert`, `alignas`/`alignof`, `typeof`, the standard attributes, enums with a fixed underlying type, `{}` initializers, `#elifdef`, `__VA_OPT__`. **Not allowed:** `_BitInt`, `auto` inference, VLAs and `alloca`, `#embed`, `_Complex`, `_Float16`, `[[unsequenced]]`/`[[reproducible]]`, `constexpr` objects (until the stage-0 minimum passes Clang 18), `long`, and every compiler extension — `__attribute__`, `__declspec`, statement expressions — except through the shims in `core/compiler.h`. The dialect exists so that every stage-0 compiler, and `ncc` itself from the earliest phase it can, accepts the whole tree; a feature outside it is a feature that delays self-hosting. `libncrt` (`runtime/`) and the shipped headers (`include/`) are not compiler sources and are held to the ABI instead.

**R13 — Memory is arenas** (Design/02 §3). Three lifetimes — session, translation unit, function — with the function arena reset after each function. There is no individual `free` in the compiler proper. Heap memory comes only from `nc_xrealloc`/`nc_xfree`, for the growable vectors that are not arena-owned. No other allocator, pool or free list without a design decision (§6).

**R14 — No third-party dependencies** (D-07). The build needs a C23 compiler and nothing else: no Python, no CMake, no TableGen, no package manager, and `make` is a convenience — every target is a plain compiler invocation. If you believe something is unavoidable, propose it in your report with what it buys and what it costs; do not add it. **The test tooling is not the build**: `llvm-mc`, `lld-link`, `llvm-readobj`, `llvm-objdump`, `llvm-pdbutil` and `llvm-dlltool` are used by tests, which skip loudly without them, and must never become a build requirement. **Third-party code in the shipped runtime is the owner's decision and has been taken** (Q-23): CORE-MATH first, musl otherwise, both MIT, attributed in `runtime/THIRD_PARTY.md`. Test inputs from GPL suites are used but never shipped (Q-01). Anything else under a licence needs the owner's approval before it lands, with the licence text travelling with the bytes.

**R15 — Output is a pure function of the input** (Design/02 §4, Design/13 §3): the input bytes, the command line and the compiler binary, and nothing else. No hash map is iterated for output — anything emitted is sorted or kept in insertion order. No pointer value reaches the output or a sort key. No wall clock: the COFF `TimeDateStamp` is zero or a content hash, and `__DATE__`/`__TIME__` honour `SOURCE_DATE_EPOCH`. Temporary names come from per-function counters. Nothing host-dependent (§3). `make test-link` checks that two runs agree; the bootstrap (stage-2 objects byte-identical to stage-3) checks it once the compiler can build itself.

**R16 — References are handles** (Design/02 §3). IR values, blocks, MIR instructions and virtual registers are 32-bit indices into per-function arrays, not pointers, so that side tables are plain vectors indexed by id and printing is deterministic. AST nodes may be pointers, because the AST is immutable after sema.

**R17 — A string you do not write is `const`.** In C a string literal has type `char[N]`, but writing through it is undefined behaviour, and it lives in a read-only section. A parameter or field that holds text it does not modify is `const char*`; a cast that drops `const` to pass one is a defect, not a fix.

**R18 — Errors are diagnostics; invariants are assertions** (Design/02 §3). Anything the *user* got wrong is a diagnostic with a stable id (`NC1234`) from the diagnostics table, rendered in MSVC format (`file(line,col): error NC1234: …`) so Visual Studio and MSBuild can parse it, and compilation continues where the front end can recover. Anything the *compiler* got wrong is `NC_ASSERT` / `NC_UNREACHABLE`, which is fatal in every build — never `<assert.h>`, which `NDEBUG` removes.

**R19 and up are reserved.** A new rule here needs a source in `Design/` to cite. Do not invent one, and do not import one from another tree: a rule with no source behind it is a rule nobody can settle an argument with.

---

## 6. Working rules

**Stay in scope.** Do what the task asks. Adjacent code that offends you is not part of the task — note it in your report and move on. Unrequested "while I was in there" changes are the main way a young tree acquires regressions it cannot bisect.

**The way of working is decided** (Q-02): Claude implements, the owner reviews. Work arrives as pull requests scoped to one component spec.

**Record decisions in `Design/`, not beside it.** An engineering decision — a file format, a subsystem's shape, an exception to a rule here — is recorded in [`Design/16-open-questions.md`](Design/16-open-questions.md) under the next free `Q-` number, with its consequence and the documents it touched, and the affected numbered document is updated **in the same commit** as the change that implements it. A question you cannot settle goes into the *Open* table rather than into the code as an assumption. Figures in the design are measured, not estimated — if you quote one, say how you measured it. A decision nobody wrote down gets re-litigated every few months by whoever forgot it.

**Write the checkers early.** Three gates are missing, and each is a Makefile target, so that no tool beyond the ones CI already installs is needed:

- `make check-format` — `clang-format --dry-run --Werror` over every tracked `.c` and `.h`, pinned to clang-format 18.1.3, the version ubuntu-24.04 installs.
- `make tidy` — clang-tidy over every `.c` under `src/` and `tests/`, once for the host and once for Windows ARM64 against the mingw-w64 headers (the way `check-windows` compiles), so the `_WIN32` half of the OS layer is linted too. The version is pinned when the gate is wired.
- `make check-tree` — R2's banned affixes, R7's file and directory names, R11's spelling families, the enumerator shape, and the two registries in §2 (a source directory missing from either build file; a test entry point missing from `test_main.c`). It is a small tool in `src/tools`, built by stage 0 like everything else, so that D-07 holds for the checker too.

Until each lands, the rules it would enforce are review's problem — which is exactly why they are early work rather than housekeeping.

**What CI runs.** [`.github/workflows/ci.yml`](.github/workflows/ci.yml) has two jobs. **`linux`** runs on ubuntu-24.04, once with GCC and once with Clang: build, unit tests and the link test. On the Clang leg it also runs `make check-windows` and uploads the phase-0 objects as an artifact. **`windows-arm64`** runs natively on GitHub's hosted `windows-11-arm` runner (image `windows-11-vs2026-arm64`), which is free because the repository is public; if the repository is made private (Q-27), this job is billed against private-repository minutes. It sets up the VS 2026 ARM64 environment, builds the tools with clang and runs `tests/windows/p0-accept.cmd`: it links both demo programs with `link.exe`, runs them and compares their output, and checks every function against `RtlVirtualUnwind` with `ncc-uwcheck`. It then runs the unit tests natively. What remains manual is stepping through in the VS debugger and the `cl /Z7` comparison (Q-26). Format, tidy and the tree check are not gated yet; they are wired by the conversion (§8). Nothing is `continue-on-error`, and nothing should be: a step that exists and fails fails the build.

**Commits and PRs.** Branch off `main`; small, focused commits with an imperative subject describing the change, not the process. One change per PR. CI must be green. Never commit build output.

---

## 7. Before you hand work back

- [ ] Naming conforms to §1 — `PascalCase` functions led by their module, `_` on parameters, `g_` on globals, `UPPER_CASE` constants, `PascalCase` enumerators of named enums, no `I`/`C`/`Base`/`_t` affixes, lowercase file names.
- [ ] Only the lines the task required were changed; no reformatting, no drive-by fixes.
- [ ] A new source directory is in the Makefile **and** `build.cmd`; a new test entry point is in `test.h` **and** `test_main.c`'s table.
- [ ] No `-std`, `-Werror` or warning flag was changed, and no warning was silenced with a pragma or a `-Wno-…`.
- [ ] The Makefile and `build.cmd` still agree on everything §3 says they must.
- [ ] The code is NC-Boot (R12), and no platform header appears outside the OS layer.
- [ ] No new third-party dependency (R14), and nothing host-dependent or order-dependent reaches the output (R15).
- [ ] The checkers pass — or, for one not yet written, the report says which and why.
- [ ] It builds with Clang **and** GCC, and `make test` passes with no skips you did not report.
- [ ] If it touches object emission, unwind, CodeView or generated code: `make test-link` was run, and the report says whether Windows acceptance was.
- [ ] `Design/` is updated in the same commit if the change *was* a decision.
- [ ] Your report states plainly what you verified, on which host and compilers, what you assumed, and any rule here you had to bend.

---

## 8. The phase-0 tree and its conversion

The phase-0 sources — `src/` and `tests/`, about 5,000 lines — were written before these conventions arrived, and do not follow them. Functions are `snake_case` with a module prefix (`arena_init`, `coff_align_flags`, `nc_fatal`); the enumerators of named enums are prefixed `UPPER_CASE` (`CC_EQ`, `K_XSP`, `TK_LABEL`); parameters carry no `_`; fields are `snake_case`; file-scope `const` tables are lowercase; and the layout is 4-space K&R with the pointer bound right. Measured on 2026-11-23 with clang-format and clang-tidy 22.1.8: **all 29 `.c` and `.h` files differ from `.clang-format`**, and clang-tidy reports **more than 2,300 naming findings** and 16 others. That count is a lower bound, because it was taken on a Windows host without the UCRT headers on the include path, so some translation units only partly parsed.

**The plan is one dedicated conversion, before phase-1 code lands**: rename every identifier to §1, reformat every file to §4, fix or explicitly record the non-naming clang-tidy findings, and wire `make check-format` and `make tidy` into CI in the same change, so that the tree cannot drift back. Renames are mechanical but not blind — R4's external spellings stay, and the enumerators that are format values stay `UPPER_CASE`. The design documents are not rewritten for it: they name things, not spellings.

**Until then:**

- **New files follow this file.** A new file calls the existing API by its current name (`arena_alloc`); the conversion renames the callers along with the callee.
- **An edit to an existing file matches that file.** Do not convert piecemeal — a half-converted file, or one module renamed inside a feature PR, is the worst of both styles and a merge conflict for everyone else.
- **Known gaps the conversion also closes**: `core/compiler.h` does not exist yet, so `core/core.h` calls `__builtin_clzll` and friends directly rather than through the shim R12 asks for; and `build.cmd` lacks `-Wshadow` (§3).
