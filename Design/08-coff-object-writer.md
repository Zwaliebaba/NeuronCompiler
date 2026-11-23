# 08 — COFF object writer

The object writer turns encoded functions, data definitions, unwind records and debug records into a COFF relocatable object that `link.exe` 14.50 accepts on equal terms with a `cl` object. The reference is the PE/COFF specification on Microsoft Learn; where it is silent, observed `cl` output (`dumpbin /all`) is the reference.

## 1. Model

The writer accumulates, during compilation, a list of *sections* (name, characteristics, alignment, byte buffer or BSS size, relocation list, optional COMDAT descriptor), a *symbol table* (in insertion order, with stable indices) and a *string table*. Nothing is written until the TU is finished; then layout assigns file offsets and the file is serialised into memory and written atomically (temporary file + rename), so an interrupted build never leaves a truncated `.obj` that an incremental build would trust.

## 2. Header

`IMAGE_FILE_HEADER` with `Machine = IMAGE_FILE_MACHINE_ARM64` (0xAA64), `SizeOfOptionalHeader = 0`, `Characteristics = 0`, and `TimeDateStamp = 0` (reproducibility; `link.exe` does not depend on it). When the section count exceeds 65,279 (the 16-bit section-number space minus reserved values), the writer switches to the *bigobj* format (`ANON_OBJECT_HEADER_BIGOBJ`, version 2, the well-known class GUID, 32-bit section count, 20-byte `IMAGE_SYMBOL_EX` records), as `cl /bigobj` does. With one COMDAT section per function plus associative `.pdata`, `.xdata` and `.debug$S` sections, a TU with ~16k functions crosses that limit, which generated code reaches; the switch is automatic.

## 3. Sections

| Section | Characteristics | Content |
|---------|-----------------|---------|
| `.text$mn` | `CNT_CODE`, `MEM_EXECUTE`, `MEM_READ`, align 4 (16 for function entry under `/Qalign-functions`) | One COMDAT section per function (function-level linking, MSVC `/Gy` behaviour, always on). |
| `.data` | `CNT_INITIALIZED_DATA`, `MEM_READ`, `MEM_WRITE` | Initialised writable data. Per-object COMDAT sections (`/Gw`), on by default at every optimisation level (Q-18); `/Gw-` disables. |
| `.bss` | `CNT_UNINITIALIZED_DATA`, `MEM_READ`, `MEM_WRITE` | Zero-initialised data. |
| `.rdata` | `CNT_INITIALIZED_DATA`, `MEM_READ` | Constants, string literals, `constexpr` objects with linkage. |
| `.pdata` | `CNT_INITIALIZED_DATA`, `MEM_READ`, align 4 | Function table entries, one associative COMDAT per function. |
| `.xdata` | `CNT_INITIALIZED_DATA`, `MEM_READ`, align 4 | Full unwind records and SEH scope tables, associative per function. |
| `.tls$` | `CNT_INITIALIZED_DATA`, `MEM_READ`, `MEM_WRITE` | Thread-local object templates (zero-initialised ones included: `.tls$` has no BSS form). |
| `.drectve` | `LNK_INFO`, `LNK_REMOVE`, align 1 | Linker directives (§6). |
| `.debug$S` | `CNT_INITIALIZED_DATA`, `MEM_READ`, `MEM_DISCARDABLE`, align 1 | CodeView symbols; a per-TU section plus one associative per function COMDAT. |
| `.debug$T` | as above | CodeView type records. |
| `.gfids$y` | Matched to `cl /guard:cf` output | CFG address-taken function list, as symbol-table indices (post-v1). |

Section names with a `$` suffix are grouped and ordered by the linker; we use MSVC's names so `ncc` and `cl` objects interleave predictably.

## 4. COMDATs and symbols

Each function's text section is a COMDAT. The selection is `IMAGE_COMDAT_SELECT_NODUPLICATES` for ordinary external definitions (duplicate definitions are a link error, as they should be), `SELECT_ANY` for MSVC-semantics `__inline` functions, `selectany` data and deduplicated literals, and `SELECT_ASSOCIATIVE` (associated with the function's section) for that function's `.pdata`, `.xdata` and `.debug$S` sections. Associativity is what makes `/OPT:REF` discard the unwind and debug data of an unreferenced function together with its code; getting it wrong produces either bloat or, worse, `.pdata` entries that point at discarded code.

The symbol table contains, in this order: `@feat.00` (absolute, `IMAGE_SYM_ABSOLUTE`, value = feature bits), a `.file` record when debug info is off (otherwise the file is described in CodeView), each section's section symbol with its COMDAT auxiliary record (length, relocation count, checksum, associated section number, selection), defined symbols (`IMAGE_SYM_CLASS_EXTERNAL` or `STATIC`, function symbols with type `0x20`), undefined externals, and common symbols (`IMAGE_SYM_UNDEFINED` with `Value = size`, only under `/Qcommon`). Names longer than 8 bytes go to the string table. C symbol names are undecorated on ARM64 (no leading underscore, no `@n` suffix).

Compiler-generated symbols use reserved prefixes that cannot collide with C identifiers or MSVC names: `$ncc$str$<hash>` for string literals, `$ncc$jt$...` for jump-table labels if they ever need a symbol, `$unwind$<func>` and `$pdata$<func>` mirroring MSVC's naming for readability in `dumpbin`. Floating-point constants use MSVC's `__real@<hex-bits>` names with `SELECT_ANY`; since the name encodes the value, sharing them with `cl` objects is safe and saves space.

## 5. Relocations

COFF ARM64 relocations carry no explicit addend: the addend lives in the instruction's immediate field, and the linker adds the symbol value to it. This bounds the addend to the field width, so for `symbol + offset` beyond the representable range the code generator materialises the offset with a separate `add` rather than relying on the relocation (LLVM does the same).

| Relocation | Value | Used for |
|------------|-------|----------|
| `IMAGE_REL_ARM64_ABSOLUTE` | 0x00 | Not emitted |
| `IMAGE_REL_ARM64_ADDR32` | 0x01 | 32-bit VA (rare; not in normal code) |
| `IMAGE_REL_ARM64_ADDR32NB` | 0x02 | RVAs: `.pdata` function start and unwind-info words, `.xdata` handler and SEH scope-table entries |
| `IMAGE_REL_ARM64_BRANCH26` | 0x03 | `bl`/`b` to another section or external symbol |
| `IMAGE_REL_ARM64_PAGEBASE_REL21` | 0x04 | `adrp` |
| `IMAGE_REL_ARM64_REL21` | 0x05 | `adr` to another section (rare) |
| `IMAGE_REL_ARM64_PAGEOFFSET_12A` | 0x06 | `add Xd, Xn, :lo12:sym` |
| `IMAGE_REL_ARM64_PAGEOFFSET_12L` | 0x07 | `ldr`/`str` with `:lo12:sym` (the linker scales by the access size encoded in the instruction) |
| `IMAGE_REL_ARM64_SECREL` | 0x08 | CodeView symbol and line-table offsets |
| `IMAGE_REL_ARM64_SECREL_LOW12A` | 0x09 | TLS offset, low 12 bits |
| `IMAGE_REL_ARM64_SECREL_HIGH12A` | 0x0A | TLS offset, bits 12–23 |
| `IMAGE_REL_ARM64_SECREL_LOW12L` | 0x0B | TLS offset folded into a load (not used by our sequence) |
| `IMAGE_REL_ARM64_SECTION` | 0x0D | CodeView section index |
| `IMAGE_REL_ARM64_ADDR64` | 0x0E | Pointer-sized data initialisers (`static int *p = &x;`) |
| `IMAGE_REL_ARM64_BRANCH19` | 0x0F | `b.cond`/`cbz` across sections (not emitted: conditional branches are intra-function) |
| `IMAGE_REL_ARM64_BRANCH14` | 0x10 | `tbz` across sections (not emitted) |
| `IMAGE_REL_ARM64_REL32` | 0x11 | 32-bit PC-relative data (not used in v1) |

A PE image is relocatable, so `ADDR64` data relocations become base relocations at link time; that is `link.exe`'s job, not ours. `PAGEOFFSET_12L` requires the target to be aligned to the access size; the code generator only folds a `:lo12:` into a load or store when the symbol's alignment and offset make that true, and otherwise uses `adrp` + `add` + `ldr [Xn]`.

## 6. Linker directives

`.drectve` holds a space-separated ASCII list: `/DEFAULTLIB:"msvcrt"` (or `libcmt`, `msvcrtd`, `libcmtd` per `/MD`, `/MT`, `/MDd`, `/MTd`), `/DEFAULTLIB:"oldnames"` and `/DEFAULTLIB:"libncrt"` (or `libncrtd`). The UCRT and vcruntime libraries are then pulled in by the CRT libraries' own directives, exactly as with `cl` objects. Beyond default libraries it carries `/EXPORT:` entries, `/FAILIFMISMATCH:` from `#pragma detect_mismatch`, and verbatim strings from `#pragma comment(linker, ...)`. `/Zl` suppresses the default-library entries, as with `cl`.

## 7. Validation

Every object produced by the test suite is checked by three independent readers: `llvm-readobj --file-headers --sections --relocations --symbols --unwind`, `dumpbin /all` (on Windows CI), and an in-house structural checker (`ncc-objcheck`) that validates invariants `link.exe` does not report clearly: every `.pdata` entry has an associative relationship with its function's section, every relocation's target range fits, no COMDAT is associated with a non-COMDAT, alignment fields match buffer requirements, and `.xdata` records parse under the unwind verifier ([09-unwind-and-seh.md](09-unwind-and-seh.md)). Final acceptance is always "`link.exe` links it and the program runs".

## Decisions

`/Gw` data COMDATs on at all levels (Q-18).
