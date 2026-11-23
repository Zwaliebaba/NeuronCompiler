# 10 — CodeView debug information

## 1. Scope and model

`ncc /Z7` embeds CodeView C13 debug information in each object, in `.debug$S` (symbols, line tables, file checksums, string table) and `.debug$T` (type records). `link.exe /DEBUG` merges it into a PDB. We never write PDB files and never use `mspdbsrv`, so `/Zi` and `/ZI` are accepted as synonyms for `/Z7` (with a note in verbose mode). This removes the hardest and least documented part of the Microsoft debug format from our scope entirely.

References: `cvinfo.h` from Microsoft's `microsoft-pdb` repository for record layouts, LLVM's `CodeViewDebug` emitter for behaviour, and `cl /Z7` output decoded with `cvdump` and `llvm-pdbutil dump -all` for what the VS debugger actually expects from C. The emitter only produces record kinds that `cl` produces for C code; anything more exotic is a risk with no user benefit (R6).

The acceptance test is the debugger, not a dump tool: automated tests drive `cdb`/`windbg` (and the VS debugger via its automation where practical) to set breakpoints by line, step, inspect locals, structs, arrays, bit-fields, enums and globals, and walk the call stack, and compare against expected transcripts.

## 2. `.debug$S` structure

Each section begins with the `CV_SIGNATURE_C13` (4) word, followed by subsections (`DEBUG_S_SYMBOLS`, `DEBUG_S_LINES`, `DEBUG_S_FILECHKSMS`, `DEBUG_S_STRINGTABLE`), each 4-byte aligned.

**Per TU** (one non-COMDAT `.debug$S`): `S_OBJNAME` (object path), `S_COMPILE3` (language `CV_CFL_C`, machine `CV_CFL_ARM64`, front-end and back-end version numbers of `ncc`, version string "NeuronCompiler x.y"), `S_BUILDINFO` referencing an `LF_BUILDINFO` (cwd, compiler path, source file, command line), global data symbols (`S_GDATA32`, `S_LDATA32`, `S_GTHREAD32`, `S_LTHREAD32` with `SECREL`/`SECTION` relocations), user-defined type names (`S_UDT` for typedefs and tags), constants (`S_CONSTANT` for enumerators and `constexpr` objects where useful), the file-checksum table (SHA-256 of each source file as read, so the debugger can verify source matches) and the string table.

**Per function** (one `.debug$S` associative to the function's COMDAT): `S_GPROC32_ID`/`S_LPROC32_ID` (referring to an `LF_FUNC_ID` in `.debug$T`, with code start via `SECREL`/`SECTION` relocations, length, debug start/end = end of prologue / start of first epilogue), `S_FRAMEPROC` (frame size, flags including "uses frame pointer", local and parameter base-pointer register = `x29`), parameter and local symbols (§4), lexical blocks (`S_BLOCK32`) for nested scopes, `S_LABEL32` for user labels, `S_CALLSITEINFO` for indirect calls, and `S_PROC_ID_END`. The function's `DEBUG_S_LINES` subsection follows, with a `SECREL`/`SECTION`-relocated code offset, one file block per contributing source file, and `(offset, line, is_statement)` entries; column information is included (`CV_LINES_HAVE_COLUMNS`).

## 3. `.debug$T` type records

Types are emitted into one `.debug$T` per TU (types are not per function), deduplicated by content hash during emission, in an order where every referenced index precedes its referrer except where forward references are required (self-referential structs use a forward-reference `LF_STRUCTURE` with the `fwdref` property, then the full definition, exactly as `cl` does). The C mapping:

| C type | CodeView |
|--------|----------|
| Basic types | Built-in type indices (`T_INT4`, `T_UINT8`, `T_REAL64`, `T_CHAR`, `T_WCHAR`, `T_BOOL08`, `T_64PVOID`, …). `long` → `T_LONG`, `long double` → `T_REAL64`. |
| `char8_t`, `char16_t`, `char32_t` | `T_CHAR8`, `T_CHAR16`, `T_CHAR32` (are typedefs in C, so emitted as `S_UDT` over the underlying type plus those indices where the debugger benefits) |
| Pointers | `LF_POINTER` (64-bit, `CV_PTR_64`, mode, `const`/`volatile`/`restrict` bits) |
| Qualifiers | `LF_MODIFIER` |
| Arrays | `LF_ARRAY` (element type, index type `T_UQUAD`, size in bytes); VLAs as described below |
| Struct / union | `LF_FIELDLIST` of `LF_MEMBER` / `LF_BITFIELD` + `LF_STRUCTURE` / `LF_UNION` with size and unique name |
| Enums | `LF_FIELDLIST` of `LF_ENUMERATE` + `LF_ENUM` with the underlying type |
| Function types | `LF_ARGLIST` + `LF_PROCEDURE` (calling convention `CV_CALL_NEAR_C`; variadic as trailing `T_NOTYPE`) |
| Function ids | `LF_FUNC_ID` (name, type, scope) |
| `_Complex double` | A synthesised `struct _Dcomplex`-shaped record named `_Complex double` with members `real`, `imag`, so the debugger shows both parts |
| `_BitInt(N)` | No CodeView equivalent. N ≤ 64: the container integer type, named via `S_UDT` `_BitInt(N)`; N > 64: a struct of `T_UQUAD` limbs named `_BitInt(N)`, plus a natvis visualiser shipped with `ncc` that renders the value (Q-21) |
| `nullptr_t` | `T_64PVOID`-based typedef `nullptr_t` |
| `_Float16` | `T_REAL16` |

VLAs have no static size; locals of VLA type are described as pointers to the element type with an `S_LOCAL` for the hidden size variable, matching the practical behaviour of other compilers on Windows.

## 4. Variable locations

**At `-O0`** every parameter and local lives in a stack slot at a fixed offset from `x29` for the function's whole lifetime, so each is an `S_LOCAL` with one `S_DEFRANGE_FRAMEPOINTER_REL_FULL_SCOPE`, or `S_REGREL32` relative to `x29`, whichever `cl` uses for ARM64 C (to be matched). Parameters that arrive in registers are stored to their slots in the first instructions after the prologue; the debug-start address of the function is after these stores, so a breakpoint on the function shows correct values.

**At `-O1`/`-O2`** variables move between registers and stack slots and may have no location at all. `dbg.value` instructions in NIR are maintained by optimisation passes (each pass that deletes or replaces a value updates or kills the debug uses; the verifier checks no `dbg.value` refers to a dead value). After register allocation, a location-list pass computes, for each variable, the ranges of code addresses over which it lives in a specific register or frame slot, and emits `S_LOCAL` with `S_DEFRANGE_REGISTER`, `S_DEFRANGE_REGISTER_REL` (for spill slots relative to `sp` or `x29`), `S_DEFRANGE_SUBFIELD_REGISTER` (for SROA'd struct fields living in separate registers) and gaps. Variables whose value is a known constant over a range get `S_CONSTANT`-like treatment where CodeView allows, otherwise "optimised away". This is v1-scope at basic quality (correct where present, sometimes absent), not at MSVC `/O2 /Zo` parity.

Inlined functions are described with `S_INLINESITE` / `S_INLINESITE_END` and `LF_FUNC_ID` inlinee records, plus `DEBUG_S_INLINEELINES`, so stepping and the call stack show inline frames. This is required for `-O2` debugging to be usable and is in v1 scope.

## 5. Registers

CodeView ARM64 register numbering (`CV_ARM64_*` in `cvconst.h`) is mapped from our physical register enum through a table generated from `arm64.def`. Sub-register views (`w` versus `x`, `s`/`d`/`q` views of `v`) are expressed by the record's type, not by distinct register numbers, except where `cvconst.h` defines distinct numbers.

## 6. Path handling and reproducibility

Paths recorded in `S_OBJNAME`, `LF_BUILDINFO` and the file-checksum table are absolute by default (the debugger needs them), remapped by `/pathmap:old=new` (as `cl` supports), and normalised to backslashes. The SHA-256 file checksums let the debugger reject mismatched sources. Debug sections contain no timestamps.

## Open items from this document

Natvis-based display of `_BitInt` (Q-21).
