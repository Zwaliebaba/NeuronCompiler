# 07 — Windows ARM64 ABI

This document is the single place where `ncc` decides how C values cross function boundaries and how C objects map to the platform. It follows Microsoft's "Overview of ARM64 ABI conventions" and, where that document defers, ARM's AAPCS64. Every rule here has a corresponding generated cross-compiler test against `cl` ([14-testing-strategy.md](14-testing-strategy.md) §4); where the two disagree, `cl` wins and this document is corrected.

## 1. Registers

| Registers | Role | Preserved across calls |
|-----------|------|------------------------|
| `x0–x7` | Arguments / results | No |
| `x8` | Indirect result location | No |
| `x9–x15` | Scratch (`x15`: `__chkstk` size, CFG target) | No |
| `x16–x17` | Intra-procedure-call scratch; clobbered by linker thunks | No |
| `x18` | TEB pointer in user mode. **Never touched** | — |
| `x19–x28` | Callee-saved | Yes |
| `x29` | Frame pointer, must point to the previous `{x29, x30}` pair | Yes |
| `x30` | Link register | No (callee preserves for its own return) |
| `v0–v7` | Arguments / results | No |
| `v8–v15` | Low 64 bits callee-saved, high 64 bits volatile | Partially |
| `v16–v31` | Scratch | No |

FPCR fields `AHP`, `DN`, `FZ` and `RMode` are non-volatile: a function that changes them (e.g. under `FENV_ROUND`) restores them before return. Trap-enable bits must remain zero.

## 2. Argument passing (non-variadic)

IRGen implements AAPCS64 stages A–C, which Windows adopts unchanged for non-variadic functions:

1. **Composite larger than 16 bytes** (and not an HFA/HVA): the caller makes a copy in its own frame and passes a pointer. The callee may modify the copy.
2. **HFA / HVA** (2–4 members of the same `float`, `double` or 64/128-bit short-vector type, recursively through nested structs and arrays): one SIMD/FP register per member if `NSRN + members ≤ 8`; otherwise `NSRN` is set to 8 and the whole aggregate goes on the stack.
3. **Floating scalar / short vector**: next `v` register while `NSRN < 8`, else stack (8-byte slot for `float`/`_Float16`, natural alignment ≥ 8).
4. **Integer or pointer ≤ 8 bytes**: next `x` register while `NGRN < 8`.
5. **16-byte-aligned argument** (`_BitInt(65..128)`, `alignas(16)` composite): `NGRN` rounded up to even before allocation.
6. **16-byte integer** (`_BitInt(65..128)`): register pair `x[n], x[n+1]` if `NGRN < 7`, low half in the lower register.
7. **Composite ≤ 16 bytes**: rounded up to a multiple of 8 and passed in consecutive `x` registers if they fit (`size/8 ≤ 8 − NGRN`), as if loaded by `ldr`s from an 8-aligned address; otherwise `NGRN` becomes 8 and it goes on the stack. A composite is never split between registers and stack.
8. **Stack**: `NSAA` rounded to max(8, natural alignment), arguments smaller than 8 bytes occupy 8-byte slots.

Unused bits in a register or slot are unspecified: callees must not assume that a `char` argument arrives sign- or zero-extended. AAPCS64 leaves them unspecified (Apple's arm64 variant, which requires caller extension, is the exception and does not apply here). We extend at the caller anyway, because it is nearly free and protects callees compiled by less careful compilers, and we re-extend at the callee, except at `-O2` in internal-linkage functions whose address never escapes, where every caller is ours (Q-17).

`_Complex float` and `_Complex double` are HFAs of two members. `_Complex long double` is `_Complex double`.

## 3. Variadic functions

Windows differs from AAPCS64 here, and this is the most common source of interoperability bugs in ports. For a variadic function, **SIMD/FP registers are not used for any argument** (named or variadic), and HFAs get no special treatment: every argument is laid out as if on an imaginary stack (rules C.12–C.15: 8-byte slots, natural alignment ≥ 8, composites >16 bytes by reference) whose first 64 bytes are loaded into `x0–x7` and the rest is passed on the real stack. A `double` variadic argument therefore travels in an `x` register.

The callee homes `x0–x7` into the 64-byte home area immediately below its incoming stack arguments, which makes the register-passed and stack-passed arguments one contiguous array. `va_list` is `char *` (as in UCRT's `vadefs.h`). `va_start` points it at the first variadic slot, `va_arg` reads `*(T *)ap` (or `**(T **)ap` for composites passed by reference) and advances by the slot size rounded up to 8 (16-aligning first for 16-byte-aligned types). UCRT's `vadefs.h` implements `va_start` for `_M_ARM64` with the `__va_start` intrinsic, which we provide with the same signature so the UCRT header works unmodified.

C23 has no unprototyped functions, so a call always knows whether the callee is variadic. Calling a variadic function through a non-variadic function pointer type (or vice versa) is undefined, and on this ABI it genuinely breaks for floating-point arguments; we warn on the cast.

## 4. Return values

| Type | Location |
|------|----------|
| Integer, pointer ≤ 8 bytes | `x0` |
| 16-byte integer (`_BitInt(65..128)`) | `x0`, `x1` |
| Floating scalar | `s0` / `d0` / `h0` |
| HFA/HVA ≤ 4 members | `s0–s3` / `d0–d3` / `v0–v3` |
| Composite ≤ 8 bytes | `x0` |
| Composite ≤ 16 bytes | `x0`, `x1` (low 8 bytes in `x0`) |
| Larger composite | Caller-allocated buffer, address in `x8`; callee need not preserve or return `x8` |

The C++-specific rules in Microsoft's document (non-trivial types returned via `x0`/`x1`) never apply to C.

## 5. `_BitInt(N)`

MSVC does not implement `_BitInt`, so we follow AAPCS64's published rules, which Clang also uses for `aarch64-pc-windows-msvc`:

| N | Size / alignment | Passed as |
|---|------------------|-----------|
| 1–8, 9–16, 17–32, 33–64 | As the smallest standard integer type that contains it | Integer of that size |
| 65–128 | 16 / 16 | 16-byte integer (register pair, even-aligned) |
| > 128 | Multiple of 16 bytes / 16 (container treated as an array of 128-bit units; to be confirmed against Clang) | Composite: by reference (> 16 bytes) |

Padding bits above N in the container are unspecified at ABI boundaries, so every `_BitInt` value received from a call, a parameter or memory is re-extended before use. `unsigned _BitInt(1)` is allowed (C23 permits width 1 for unsigned only). The ABI for N > 128 is validated against Clang since there is no MSVC reference.

## 6. Stack

The stack pointer is 16-byte aligned at all times (Windows enables SP-alignment checking, so a misaligned SP-relative access faults). There is no general red zone: only the 16 bytes immediately below `sp` are reserved for patching and analysis tools, and `ncc` never uses them. Any frame or dynamic allocation of 4 KiB or more is probed page by page via `__chkstk` with the size/16 in `x15`.

The frame pointer chain is maintained in every function that has a frame (see [06-arm64-backend.md](06-arm64-backend.md) §5). Leaf functions without a frame do not set `x29` and have no `.pdata`; the unwinder takes the return address from `x30`.

## 7. Thread-local storage

`thread_local` / `__declspec(thread)` objects live in the image's `.tls$` section and are accessed with the implicit-TLS sequence MSVC uses:

```asm
    ldr     x8, [x18, #0x58]               // TEB->ThreadLocalStoragePointer
    adrp    x9, _tls_index
    ldr     w9, [x9, :lo12:_tls_index]     // this module's TLS index
    ldr     x8, [x8, x9, lsl #3]           // this module's TLS block
    add     x8, x8, :secrel_hi12:var       // IMAGE_REL_ARM64_SECREL_HIGH12A
    add     x8, x8, :secrel_lo12:var       // IMAGE_REL_ARM64_SECREL_LOW12A
```

`_tls_index` and the TLS directory are provided by the CRT (`tlssup.obj`). The block address is CSE'd within a function, so repeated accesses cost one `add` pair each. Reading `x18` here is a read only; `x18` is never written.

## 8. DLL imports and exports

A call to a function declared `__declspec(dllimport)` loads the target from the import address table and calls indirectly:

```asm
    adrp    x16, __imp_GetLastError
    ldr     x16, [x16, :lo12:__imp_GetLastError]
    blr     x16
```

A call to an import *not* declared dllimport is emitted as a direct `bl GetLastError`, and the import library's thunk (an `adrp`/`ldr`/`br x16` stub) resolves it; this costs an extra branch but is correct. Data imported from a DLL must be declared dllimport; there is no thunk for data. Taking the address of a dllimport function yields the IAT value, which is not a link-time constant, so it cannot appear in a static initialiser (diagnosed, as MSVC does).

## 9. Object layout recap

Global and static objects are aligned per Microsoft's table (sizes 2–7 → 4, 8–63 → 8, ≥ 64 → 16), which exceeds the C minimum and allows `ldp`/`stp` and `ldr q` access to larger objects; explicit `alignas` raises it further. Locals follow the smaller local table (> 4 bytes → 8). Struct layout follows [04-msvc-compatibility.md](04-msvc-compatibility.md) §2.

## Decisions

Callee re-extension skipped at `-O2` only for non-escaping internal-linkage functions (Q-17).
