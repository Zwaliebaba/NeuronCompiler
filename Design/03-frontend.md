# 03 — Front end

## 1. Source manager

Files are read whole into the session arena and never modified. A source location is a 32-bit offset into a global, monotonically assigned location space (each file inclusion and each macro expansion gets a range), in the style of Clang. A location therefore identifies file, line, column and the full macro-expansion and include stack without storing any of it per token. Line tables are built lazily per file the first time a diagnostic or debug line record needs them.

Input encoding is UTF-8, with BOM accepted and skipped. UTF-16 source files (still found in some Windows projects, notably `.rc`-adjacent headers) are transcoded on load when a UTF-16 BOM is present; without a BOM no detection is attempted (Q-08). The execution character set is UTF-8 for narrow strings, UTF-16 for `wchar_t` (MSVC ABI: 16-bit `wchar_t`) and for `u""`, UTF-32 for `U""`. Narrow string literals are emitted as UTF-8 bytes regardless of the system code page, which matches `cl /utf-8`; we will not emulate the code-page-dependent default.

## 2. Lexer

The lexer is hand-written, operates on the raw buffer, and produces tokens on demand for the preprocessor. Line splicing (`\` newline) and the absence of trigraphs (removed in C23) are handled in a slow path that is only entered when a backslash is seen, so the common path is a tight byte loop. Identifiers are hashed while scanned and interned once. C23 lexical changes that need care: digit separators (`1'000'000`, also in hex and binary), binary literals, `wb`/`uwb` suffixes, `u8` character constants, and the rule that `'` inside a pp-number is a separator only between digits.

Numeric literals are not converted in the lexer; sema converts them, because the target type depends on suffix and value. Floating literals use a correctly rounded decimal-to-binary conversion implemented in `core` (Eisel–Lemire fast path, big-decimal fallback), not the host `strtod`, so cross-compiles on Linux and native compiles on Windows produce identical bits.

## 3. Preprocessor

### 3.1 Model

The preprocessor is a pull-based token stream with a macro expansion stack, implementing the C23 algorithm exactly (including `__VA_OPT__` and the painted-blue rule). Only the conforming preprocessor is implemented. The Windows SDK and UCRT headers have been clean under MSVC's conforming preprocessor (`/Zc:preprocessor`) since SDK 10.0.20348; older SDKs are not supported. This avoids the considerable cost of emulating the traditional MSVC preprocessor's comma-swallowing and token-pasting quirks.

### 3.2 Include handling

Search order follows MSVC: for `"..."`, the directory of the including file, then the directories of the files that included it (MSVC's include-stack search), then `/I` paths, then the `INCLUDE` environment variable, then compiler-provided headers. For `<...>`, skip the first two. The compiler-provided directory is searched before `INCLUDE` for headers we own outright (`stdarg.h`, `stddef.h`, `stdatomic.h`, `stdbit.h`, `stdckdint.h`, `float.h`, `limits.h`, `iso646.h`, `stdalign.h`, `stdnoreturn.h`, `complex.h`, `tgmath.h`), and overlay headers use `#include_next` to reach the UCRT original ([11-runtime-and-headers.md](11-runtime-and-headers.md)). `#include_next` is implemented as an extension.

Throughput depends on not re-reading headers. Each file records whether it has the classic include-guard shape (`#ifndef X` / `#define X` … `#endif` with nothing outside); if so, a later inclusion is skipped without opening the file when `X` is defined. `#pragma once` is honoured by file identity (volume serial + file index on Windows, device + inode elsewhere), not by path string, so symlinks and differing case do not defeat it.

### 3.3 `#embed`

`#embed` reads the resource as bytes and, rather than producing one token per byte, produces a single synthetic *embed token* carrying a pointer to the byte range. The parser, when it meets an embed token in an initialiser list, expands it directly into the initialiser's element storage. Anywhere else (e.g. inside a macro argument that is stringified, or `-E` output) it is materialised as a comma-separated list of integer literals. This keeps `#embed` of a 50 MB file linear in time and memory. `limit`, `prefix`, `suffix`, `if_empty` and the vendor-namespaced `ncc::offset` parameter are supported; `__has_embed` returns the standard tri-state.

### 3.4 Pragmas

| Pragma | Handling |
|--------|----------|
| `STDC FP_CONTRACT`, `STDC FENV_ACCESS`, `STDC CX_LIMITED_RANGE`, `STDC FENV_ROUND`, `STDC FENV_DEC_ROUND` | Converted to scoped annotations consumed by sema (see §5.6). `FENV_DEC_ROUND` is accepted and ignored with a diagnostic, since decimal FP is not supported. |
| `once` | §3.2 |
| `pack(push/pop/n/show)` | Maintained as a stack in sema; affects struct layout. |
| `comment(lib, "x")`, `comment(linker, "...")` | Emitted to `.drectve` (`/DEFAULTLIB:x`, verbatim linker option). |
| `warning(push/pop/disable/default/error: n)` | Mapped onto our diagnostic ids where a mapping exists; MSVC warning numbers without a mapping are accepted silently. |
| `section`, `data_seg`, `const_seg`, `bss_seg`, `code_seg`, `alloc_text` | Section naming for subsequent definitions. |
| `intrinsic`, `function` | Accepted, no effect (our intrinsics are always intrinsic). |
| `optimize("", off/on)` | Per-function optimisation level override. |
| `deprecated`, `push_macro`, `pop_macro`, `message`, `region`/`endregion`, `detect_mismatch` (→ `.drectve /FAILIFMISMATCH:`), `include_alias` | Implemented; the Windows SDK uses all of them. |
| Unknown | Ignored with a warning, never an error. |

`_Pragma` and MSVC's `__pragma` are both supported; `__pragma` is required by SDK headers.

### 3.5 Predefined macros

The standard set (`__STDC__`, `__STDC_VERSION__` = `202311L`, `__STDC_HOSTED__`, `__STDC_UTF_16__`, `__STDC_UTF_32__`, `__STDC_EMBED_*__`, the feature macros in [01-goals-scope-risks.md](01-goals-scope-risks.md) §2) plus the MSVC compatibility set, which is listed in [04-msvc-compatibility.md](04-msvc-compatibility.md) §6. `__NCC__`, `__NCC_MAJOR__`, `__NCC_MINOR__` identify the compiler.

## 4. Parser

Hand-written recursive descent, with precedence climbing for expressions. The parser consults the scope stack to resolve the typedef-name ambiguity (`T * x;`) and creates declarations as it goes, so sema runs interleaved with parsing at declaration granularity rather than as a separate tree walk. This is necessary in C anyway (a declaration's type affects how later tokens parse) and lets constant expressions in array bounds, `_Static_assert`, `alignas`, enum values and `_BitInt(N)` widths be evaluated immediately.

C23-specific parsing points: attribute specifier sequences `[[...]]` in all the positions the grammar allows (start of declarations, after declarators, on statements, on labels, on enum constants, on struct members), with balanced-token skipping for unknown attributes; `typeof`/`typeof_unqual` taking either an expression or a type name; `auto` as a type-inference specifier when no other type specifier is present (and as the storage class otherwise); `constexpr` as a storage-class specifier; `enum E : T` fixed underlying types including forward declaration `enum E : T;`; compound literals with storage-class specifiers; labels before declarations and at the end of compound statements.

MSVC syntactic extensions parsed here: `__declspec(...)` in declaration specifiers, calling-convention keywords in declarators (`__cdecl`, `__stdcall`, `__fastcall`, `__vectorcall` — parsed and ignored on ARM64, as MSVC does), `__unaligned`, `__ptr32`/`__ptr64`, `__restrict`, `__int8`…`__int64`, `__forceinline`/`__inline`, `__try`/`__except`/`__finally`/`__leave`, `__asm` is rejected with a clear diagnostic (MSVC does not support inline assembly on ARM64 either).

## 5. Semantic analysis

### 5.1 Types

Types are hash-consed in the session arena: structurally identical types are the same handle, so type equality is pointer equality and composite-type computation is cheap. Qualifiers are stored in the low bits of the handle (`const`, `volatile`, `restrict`, `_Atomic`, `__unaligned`), so qualified and unqualified versions share the underlying node.

The type kinds are: void, bool, char/signed char/unsigned char, the standard integer types, `_BitInt(N)` and `unsigned _BitInt(N)`, enumerations (with fixed or implied underlying type), float/double/long double, `_Float16`/`_Float32`/`_Float64`/`_Float32x`, the complex counterparts, `nullptr_t`, pointers, arrays (complete, incomplete, VLA with a size expression handle), functions (always prototyped in C23; `...` variadic flag), struct/union (tag, members, layout computed on completion), and atomic.

Data model and sizes are fixed by the MSVC ABI: `char` 8 (signed by default; `/J` makes it unsigned), `short` 16, `int` 32, `long` 32 (LLP64), `long long` 64, pointers 64, `wchar_t` 16 (as `unsigned short`), `long double` 64 (same representation as `double`, distinct type), `size_t` = `unsigned long long`, `ptrdiff_t` = `long long`, `max_align_t` alignment 8 (as UCRT defines it; `malloc` provides 16).

### 5.2 Layout

Struct layout implements the MSVC algorithm, not the Itanium/AAPCS one, because the Windows headers are only correct under MSVC rules. The differences that matter are bit-field allocation (a bit-field starts a new allocation unit whenever its declared type's size differs from the previous bit-field's), the treatment of zero-width bit-fields, the effect of `#pragma pack` and `__declspec(align)` on members and on the struct, and flexible/zero-length array members. Layout is verified against `cl` by a generated cross-check suite ([14-testing-strategy.md](14-testing-strategy.md) §4); the algorithm is specified in [04-msvc-compatibility.md](04-msvc-compatibility.md) §2.

### 5.3 Conversions and expressions

Usual arithmetic conversions, integer promotions and the C23 rules for `_BitInt` (no promotion of `_BitInt` to `int`; mixed `_BitInt`/standard integer conversions by rank), `nullptr` conversions, `bool` conversions, enum-with-fixed-type conversions. Every expression node carries its type, value category (lvalue, rvalue, function designator) and a flag for "is an integer constant expression" / "is an arithmetic constant expression" / "is an address constant", computed bottom-up as nodes are built.

### 5.4 Constant evaluation

One evaluator serves integer constant expressions, `constexpr` object initialisers, static initialisers (address constants with offsets) and `static_assert`. It operates on the typed AST and uses `core`'s arbitrary-precision integers for `_BitInt` up to `BITINT_MAXWIDTH`, and exact IEEE arithmetic for floating constants, with the rounding mode determined by an enclosing `FENV_ROUND` (default round-to-nearest). Overflow in a signed integer constant expression is a constraint violation and is diagnosed. The evaluator must accept the `((size_t)&((T*)0)->m)` form used by the UCRT's `offsetof` in C mode as an address constant expression of integer type, which strictly speaking is undefined behaviour but is relied on by the headers; we also provide `__builtin_offsetof` and our `stddef.h` uses it.

`constexpr` objects in C23 are deliberately narrow: no `constexpr` functions, the initialiser must be a constant expression, the value must be exactly representable in the declared type (e.g. `constexpr float f = 0.1;` is a constraint violation since 0.1 is not exactly representable as float — implemented by checking that the conversion is exact), and pointer `constexpr` objects may only be null. A `constexpr` object is itself usable in integer constant expressions.

### 5.5 Declarations, linkage and inline

Tentative definitions at file scope become COFF common symbols only under `/Qcommon`; by default they follow the ISO semantics (at the end of the TU a tentative definition becomes a zero-initialised definition in `.bss`), as GCC has done since 10 with `-fno-common`. MSVC itself emits common ("communal") symbols for tentative definitions, so code that defines `int x;` in a header included by several TUs links under MSVC and fails under our default; `/Qcommon` exists for that code. C99/C23 `inline` semantics are implemented exactly: an `inline` definition without `extern` in the TU provides no external definition; `extern inline` or a non-inline declaration in the TU emits one. Inline definitions that must be emitted (address taken, not inlined) are placed in COMDAT sections with `IMAGE_COMDAT_SELECT_ANY`, matching MSVC's handling of `__inline`.

`auto` inference requires a single declarator with an initialiser; the type is the initialiser's type after lvalue conversion and array/function decay. `typeof` of a VLA-typed expression evaluates the expression (as required); `typeof` of anything else does not.

### 5.6 Floating-point environment annotations

`#pragma STDC FENV_ACCESS ON` marks the enclosing block or function; IR generation then emits FP operations as *constrained* operations that the optimiser may not reorder, fold or speculate. `#pragma STDC FENV_ROUND dir` is implemented by saving FPCR at scope entry, setting `RMode`, and restoring at every scope exit (including `goto` and `return`), because ARM64 has no per-instruction static rounding for general arithmetic; constant folding inside the scope uses the specified direction. `FP_CONTRACT` defaults to `OFF` (no implicit `fmadd` fusion) for reproducibility; `/fp:contract` turns it on, matching MSVC's flag.

### 5.7 Attributes

Standard attributes map to semantic flags: `nodiscard` and `deprecated` produce diagnostics; `fallthrough` suppresses the implicit-fallthrough warning and is validated for position; `maybe_unused` suppresses unused warnings; `noreturn` marks the function type for the optimiser and for unreachable-code analysis; `unsequenced` and `reproducible` become IR function attributes (`readnone`-like and `readonly`-like respectively, with the C23 caveats about pointed-to state) that enable CSE and hoisting of calls. Vendor-namespaced attributes: `[[msvc::...]]` mapped to their `__declspec` equivalents where one exists, `[[gnu::...]]` and `[[clang::...]]` accepted for a curated set (`always_inline`, `noinline`, `cold`, `aligned`, `packed`, `section`, `unused`, `format`) and otherwise ignored with a warning. `__has_c_attribute` reports the version each standard attribute was adopted (`202311L` etc.) and `0` for vendor attributes we ignore.

### 5.8 Diagnostics

Every C23 constraint violation has a diagnostic id and a test. Warnings include the usual set (implicit conversion loss, sign comparison, unused, shadowing, format-string checking against the C23 `printf` grammar, implicit fallthrough, missing `return`), each individually controllable with `/wd`, `/we`, `/w1`…`/w4`-compatible levels. In system headers (those found via `INCLUDE` or the compiler directory) warnings are suppressed and MSVC extensions are accepted silently; in user code, MSVC extensions produce a pedantic warning under `/permissive-`.

## Decisions

UTF-16 source accepted with a BOM (Q-08); ISO tentative-definition semantics by default, `/Qcommon` opt-in (Q-09).
