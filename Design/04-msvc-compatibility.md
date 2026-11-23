# 04 — MSVC compatibility layer

The compatibility layer exists for one reason: so that `#include <windows.h>` and the UCRT headers compile, lay out every type exactly as `cl` does, and link against libraries built by `cl`. It is not an attempt to accept arbitrary MSVC-dialect C. Every extension here is either used by the SDK/UCRT headers or needed for binary compatibility; each is listed so it can be tested.

The authoritative references, in order, are: observed `cl` 14.50 behaviour (by cross-check test), Microsoft Learn documentation, and Clang's MS compatibility implementation (`MicrosoftRecordLayoutBuilder`, `-fms-extensions`), which encodes a decade of reverse-engineered behaviour.

## 1. Data model

Fixed by [03-frontend.md](03-frontend.md) §5.1: LLP64, 16-bit `wchar_t`, 64-bit `long double`, signed `char` by default. Two choices deserve emphasis because they diverge from AAPCS64 and from what a "generic AArch64 C compiler" would do. `char` is signed because the Windows headers and every MSVC-compiled library assume it; AAPCS64 says unsigned, and following AAPCS64 here would silently change the meaning of `CHAR`-typed comparisons in SDK code. `long double` is the same representation as `double` because UCRT's `printf("%Lf")`, `<math.h>` `l`-suffixed functions and every MSVC ABI expect 8 bytes.

## 2. Struct, union and bit-field layout

Non-bit-field members follow the standard rule: each member at the next offset aligned to `min(member alignment, current pack value)`, struct alignment is the maximum member alignment (capped by pack, raised by `__declspec(align)` / `alignas`), size rounded up to alignment. MSVC-specific rules:

**Bit-fields.** A bit-field's *allocation unit* is its declared type (for an enum, its underlying type; for `bool`, one byte). Consecutive bit-fields share an allocation unit only if their declared types have the same size and the new field fits in the remaining bits; otherwise a new unit is started at the next offset aligned to the unit type's alignment (capped by pack). A bit-field never straddles units. This is the rule that makes `struct { char a:4; int b:4; }` 8 bytes under MSVC versus 4 under AAPCS64, and it is pervasive in `winnt.h`.

**Zero-width bit-fields.** A zero-width bit-field terminates the current allocation unit. Whether it also affects the struct's alignment depends on whether it follows a non-zero-width bit-field; this is one of the cases where we implement Clang's MS layout rule and then confirm with the cross-check suite rather than trust prose.

**`#pragma pack(n)`** caps the alignment of every member and allocation unit, and therefore the struct alignment. `__declspec(align(n))` on a member or type raises its alignment and is *not* capped by pack. `alignas` behaves as `__declspec(align)` in MSVC C mode.

**Zero-length and flexible arrays.** `T x[0]` (MS extension) and `T x[]` are accepted as the final member and contribute their alignment but no size. MSVC also accepts a flexible array member in a struct that is itself the last member of another struct, and in unions; we accept those with a pedantic warning.

**Microsoft anonymous members.** `struct S { struct T; };` with a previously declared tagged type and no declarator embeds `T`'s members anonymously (the `-fms-extensions` behaviour). Accepted in compatibility mode because some SDK headers rely on it when `NONAMELESSUNION` is not defined.

**Empty structs.** MSVC C rejects empty structs (C2016); the GNU extension gives them size 0, and C23 still makes them a constraint violation. We reject them in strict mode and, under `/Qgnu-ext`, accept them with size 0. There is no MSVC precedent to be compatible with, so the only question is whether to accept them at all (Q-10).

The layout engine is a single function with a table-driven test suite: for each generated struct, `cl /d1reportSingleClassLayout`-equivalent information is obtained by compiling a probe TU with `cl` that emits `sizeof`, `_Alignof` and `offsetof` (and bit offsets via union-punning) for every member, and the same TU is compiled with `ncc`; the outputs must be identical ([14-testing-strategy.md](14-testing-strategy.md) §4).

## 3. Keywords and declaration syntax

| Construct | Semantics |
|-----------|-----------|
| `__int8`, `__int16`, `__int32`, `__int64` | Aliases for `char`, `short`, `int`, `long long` (with `unsigned` combinations). |
| `i8`, `i16`, `i32`, `i64`, `ui8`…`ui64` literal suffixes | MSVC integer-literal suffixes, used in some SDK macros. |
| `__inline`, `__forceinline` | **MSVC inline semantics, not C23 `inline`**: a referenced, non-inlined definition is emitted as an external COMDAT with `SELECT_ANY`. The UCRT's `_CRT_STDIO_INLINE` `printf` family depends on this; C23 semantics would leave `printf` undefined at link time. `__forceinline` additionally sets the always-inline attribute. |
| `__cdecl`, `__stdcall`, `__fastcall`, `__vectorcall`, `__thiscall`, `__clrcall` | Parsed and ignored (MSVC also ignores them on ARM64). |
| `__unaligned` | Type qualifier. No codegen effect on ARM64 since Windows enables hardware misalignment handling for normal memory, but it participates in type compatibility and is preserved for diagnostics. |
| `__ptr32`, `__ptr64`, `__sptr`, `__uptr`, `__w64` | `__ptr64` and `__w64` are no-ops; `__ptr32` produces a 32-bit pointer type (sign- or zero-extended on conversion), needed only for a handful of WOW64-related declarations. |
| `__restrict` | Same as `restrict`. |
| `__alignof` | Same as `alignof`. |
| `__FUNCTION__`, `__FUNCDNAME__`, `__FUNCSIG__`, `__COUNTER__`, `__TIMESTAMP__` | As MSVC. |
| `__try`, `__except`, `__finally`, `__leave` | Structured exception handling ([09-unwind-and-seh.md](09-unwind-and-seh.md) §5). |
| `__asm` | Rejected: MSVC has no ARM64 inline assembler either. |
| `__assume(e)`, `__noop(...)`, `__annotation(...)` | `__assume(0)` → `unreachable()`; `__assume(e)` → optimiser assumption; `__noop` evaluates nothing and yields `0`. |

## 4. `__declspec`

| `__declspec(...)` | Effect |
|-------------------|--------|
| `dllimport` | Functions: called through `__imp_name` (indirect call); data: accessed through `__imp_name`. Address of a dllimport object is not an address constant. |
| `dllexport` | Emits `/EXPORT:name` (or `/EXPORT:name,DATA`) in `.drectve`. |
| `thread` | Same as `thread_local` (implicit TLS, [07-windows-arm64-abi.md](07-windows-arm64-abi.md) §7). |
| `align(n)` | Alignment, not capped by pack. |
| `selectany` | Data definition emitted as COMDAT `SELECT_ANY`. |
| `allocate("sec")` | Places the object in the named section (must have been declared with `#pragma section`). |
| `noinline`, `noreturn`, `nothrow`, `noalias`, `restrict`, `allocator`, `safebuffers`, `deprecated[("msg")]` | Attributes in IR / diagnostics. `safebuffers` disables `/GS` for the function. |
| `code_seg("sec")` | Function section. |
| `guard(nocf)`, `guard(ignore)` | Control-flow-guard opt-outs. |
| `intrin_type` | Marks MSVC's `__n64`/`__n128` NEON unions as native vector types, which changes their ABI classification to short vectors passed in SIMD registers. Behaviour to be confirmed by ABI cross-check. |
| `naked` | Rejected (not supported by MSVC on ARM64). |
| `uuid`, `novtable`, `empty_bases`, `property`, `process`, `appdomain`, `jitintrinsic`, `spectre(...)`, `hybrid_patchable` | Accepted and ignored with a diagnostic in user code (C++-only, managed-code or ARM64EC). |

## 5. Intrinsics

`winnt.h` on ARM64 includes `arm64intr.h`, which in turn declares the NEON intrinsics from `arm64_neon.h`. Merely including `windows.h` therefore declares several thousand intrinsic functions. Our model separates *declaration* from *implementation*: any function declared in a system header whose name is in the intrinsic table is bound to its intrinsic id at declaration time; a call to an intrinsic that has a table entry but no lowering yet produces a precise error ("intrinsic `neon_xyz` is not yet supported") at the call, never at the declaration. This lets us compile `windows.h` from day one and fill in intrinsics by demand.

The intrinsic table is `src/sema/intrinsics.def`: name, signature, attributes, and a lowering kind (IR opcode, IR call to a runtime helper, or direct MIR instruction pattern). Priority groups for implementation:

1. **Required by common SDK inline functions and macros**: `__readx18byte/word/dword/qword` (used by `NtCurrentTeb()` and `GetCurrentThreadId`-style inline code), `__writex18*`, `__addx18*`, `__incx18*`, `_ReadStatusReg`, `_WriteStatusReg`, `__getReg`, `__dmb`/`__dsb`/`__isb` with the `_ARM64_BARRIER_*` constants, `__yield`, `__nop`, `__debugbreak` (`brk #0xF000`), `__fastfail` (`brk #0xF003` with code in `x0`), `_ReadWriteBarrier`, `_ReturnAddress`, `_AddressOfReturnAddress`, `__va_start`, `_alloca`.
2. **Interlocked family**: `_InterlockedAdd`, `And`, `Or`, `Xor`, `Exchange`, `ExchangeAdd`, `CompareExchange`, `CompareExchange128`, `Increment`, `Decrement`, `ExchangePointer`, `CompareExchangePointer`, `_interlockedbittestandset`/`reset`, each in 8/16/32/64-bit forms and `_acq`, `_rel`, `_nf` ordering variants. These lower to the same IR atomic operations as `<stdatomic.h>` with the corresponding memory order (`_nf` = relaxed, plain = sequentially consistent, which on ARM64 MSVC means a full barrier after the operation).
3. **Bit and arithmetic**: `_BitScanForward/Reverse(64)`, `_CountLeadingZeros(64)`, `_CountLeadingOnes`, `_CountLeadingSigns`, `_CountOneBits(64)`, `_byteswap_ushort/ulong/uint64`, `_rotl`, `_rotr`, `_rotl64`, `_rotr64`, `__mulh`, `__umulh`, `_bittest(64)` and variants, `_CopyDoubleFromInt64` and friends, `__iso_volatile_load8/16/32/64`, `__iso_volatile_store*`, `__prefetch`, `__crc32*` (with `+crc`).
4. **NEON** (`arm64_neon.h`): implemented in bulk as table entries mapping directly to MIR vector instructions, after v1. Our own `<arm_neon.h>` (ACLE names, `vaddq_s32` etc.) is a later, separate decision (Q-11).

## 6. Predefined macros (compatibility mode)

| Macro | Value |
|-------|-------|
| `_MSC_VER` | `1950` (toolset 14.50, VS 2026); overridable with `/Qmsc-version:` |
| `_MSC_FULL_VER`, `_MSC_BUILD` | Matching the emulated toolset |
| `_MSC_EXTENSIONS` | `1` (unless `/Za`-like strict mode) |
| `_MSVC_TRADITIONAL` | `0` (conforming preprocessor) |
| `_WIN32`, `_WIN64` | `1` |
| `_M_ARM64` | `1` |
| `_INTEGRAL_MAX_BITS` | `64` |
| `_MT` | `1` for `/MT`, `/MD` and debug variants |
| `_DLL` | `1` for `/MD`, `/MDd` |
| `_DEBUG` | `1` for `/MTd`, `/MDd` |
| `_CHAR_UNSIGNED` | `1` under `/J` |
| `_M_FP_PRECISE` / `_M_FP_STRICT` / `_M_FP_FAST` | Per `/fp:` |
| `__STDC_NO_*__`, `__STDC_IEC_*__` | Per [01-goals-scope-risks.md](01-goals-scope-risks.md) §2 |

`_M_ARM64EC`, `_M_X64`, `_M_AMD64`, `_M_IX86` and `_M_ARM` are never defined. Defining `_MSC_VER` is a commitment: SDK headers then assume the full MSVC feature set for that version. Lying about the version to dodge a header code path is not allowed; if a header path needs a feature, we implement the feature.

## 7. Linker-facing conventions

Objects carry `.drectve` directives the way `cl` objects do: `/DEFAULTLIB:` for the selected CRT (`msvcrt`/`libcmt` and debug variants), `oldnames`, and `libncrt`; `/EXPORT:` for `dllexport`; `/FAILIFMISMATCH:` for `#pragma detect_mismatch`; `/ALTERNATENAME:` where we need weak defaults (for example, a default `__ncrt_printf_hook`). The `@feat.00` absolute symbol is emitted with the feature bits matching the compiled options (`/GS`, `/guard:cf`), as MSVC does, because `link.exe` uses it to decide whether an image can be marked CFG-compatible. `@comp.id` (the Rich-header product id) is not emitted; we have no registered product id and `link.exe` does not require it.

## Open items from this document

Empty-struct size (Q-10); whether to ship ACLE `<arm_neon.h>` (Q-11); whether `__ptr32` is worth implementing at all (Q-12).
