# 06 — ARM64 back end

## 1. MIR

Machine IR is a per-function list of blocks containing target instructions whose operands are virtual registers (`%v123`, class GPR64/GPR32/FPR16/FPR32/FPR64/FPR128), physical registers, immediates, symbol references with addend and relocation kind, block references, frame-slot references, and condition codes. The instruction set is defined once in `src/target/arm64/arm64.def`:

```c
/*     name        form            operands                flags                       */
INSN(ADDXrs,      DP_ADDSUB_SR,   (Rd64, Rn64, Rm64, SH),  0)
INSN(SUBSXri,     DP_ADDSUB_IMM,  (Rd64, Rn64SP, Imm12),   DEF_NZCV)
INSN(LDRXui,      LDST_UIMM,      (Rt64, Rn64SP, Uimm12s8),MAY_LOAD)
INSN(STPXpre,     LDST_PAIR_PRE,  (Rt64, Rt64, Rn64SP, Simm7s8), MAY_STORE | WB)
INSN(BL,          BR_IMM,         (Sym26),                 IS_CALL)
INSN(Bcc,         BR_COND,        (Cond, Label19),         IS_BRANCH | USE_NZCV)
...
```

From this single table we generate (by X-macro expansion) the opcode enum, operand-kind tables used by the verifier and register allocator, def/use and implicit-flag information, the encoder dispatch, and the `-S` printer. Encoding *forms* are a smaller set (~70) of C functions that pack fields; each instruction names its form plus fixed bits. Pseudo-instructions (`COPY`, `PARCOPY`, `CALL_SEQ`, `ATOMIC_CMPXCHG_LOOP`, `PROLOGUE`, `EPILOGUE`, `JUMPTABLE`) are expanded at defined points.

## 2. Instruction selection

Selection is per basic block, bottom-up maximal munch over expression trees formed from single-use SSA values within the block (a value with multiple uses, or used in another block, is a tree root and gets a virtual register). This "tree tiling over SSA" approach is simpler than a DAG selector, deterministic, and gets nearly all of the ARM64 folding opportunities that matter:

| Pattern | Selected as |
|---------|-------------|
| `add x, (shl y, k)` | `add Xd, Xn, Xm, lsl #k` (also `sub`, logical ops, `asr`/`lsr` forms) |
| `add x, (mul y, z)` | `madd`; `sub x, (mul y, z)` → `msub`; `smull`/`umull` for 32×32→64 |
| `load (ptradd b, (shl i, log2 size))` | `ldr Xt, [Xn, Xm, lsl #s]`; sign-/zero-extended index forms `sxtw`/`uxtw` |
| `load (ptradd b, c)` | scaled unsigned-offset (`[Xn, #imm12*size]`) or unscaled (`ldur`) |
| `brif (icmp eq x, 0)` | `cbz`/`cbnz` |
| `brif (icmp ne (and x, 1<<k), 0)` | `tbnz Xn, #k` |
| `brif (icmp p x, y)` | `cmp` + `b.cond`; compare with negated immediate via `cmn` |
| `select (icmp ...)` | `cmp` + `csel`/`csinc`/`csinv`/`csneg`; `cset`/`csetm` for boolean results |
| `and (lshr x, k), mask` | `ubfx`; sign-extending variants → `sbfx`; bit-field insert → `bfi`/`bfxil` |
| `iconst` | `movz`/`movn` + up to three `movk`, or `orr` with a logical immediate; the cheapest sequence is chosen by a small search |
| `fconst` | `fmov` with 8-bit immediate when representable, `movi` for zero, else `adrp`+`ldr` from a `.rdata` literal (COMDAT, see §08) |
| `symaddr g` | `adrp Xd, g` + `add Xd, Xd, :lo12:g`; folded into the load/store as `ldr Xt, [Xd, :lo12:g]` when the address is only dereferenced |
| dllimport `symaddr` | `adrp` + `ldr` from `__imp_g` |
| `fma` | `fmadd`/`fmsub`/`fnmadd`/`fnmsub` |
| `sadd.ov` etc. | `adds` + `cset vs` (signed) / `cs` (unsigned); `smul.ov` via `smulh` + compare |

`switch` lowering chooses among a jump table (≥ 4 cases and density ≥ 40%), a balanced binary decision tree, and bit-test clusters (cases within a 64-value window with ≤ 3 distinct targets). Jump tables are placed in the function's own `.text` section immediately after the function body as 32-bit offsets relative to the table base, which needs no relocations and keeps the table within the function's `.pdata` range:

```asm
    cmp     w0, #N
    b.hs    .Ldefault
    adr     x16, .Ljt
    ldrsw   x17, [x16, w0, uxtw #2]
    add     x16, x16, x17
    br      x16
```

## 3. Calls

IR calls are lowered with the parameter assignment decided at IRGen ([05-ir-and-optimizer.md](05-ir-and-optimizer.md) §4, [07-windows-arm64-abi.md](07-windows-arm64-abi.md)): argument values are copied into fixed physical registers immediately before the `bl`/`blr`, and stack arguments are stored into the outgoing-argument area at `[sp, #off]`, whose size is the maximum over all calls in the function and is reserved once by the prologue. The call instruction carries the ABI clobber mask: `x0–x17`, `x30`, `v0–v7`, `v16–v31` fully, and the upper 64 bits of `v8–v15`.

That last detail has a consequence for register allocation: a 128-bit SIMD value live across a call cannot live in `v8–v15`, because only their low halves are preserved. The allocator models `v8–v15` as callee-saved for FPR16/32/64 classes and caller-saved for FPR128.

Sibling calls (`tailcall` in NIR) become `b`/`br` after the epilogue when the callee's stack-argument area fits within the caller's incoming area and no `alloca` address escapes to the callee.

## 4. Register allocation

**Reserved registers.** `x18` (TEB pointer) is never allocated or written. `x29` is the frame pointer and always maintained as a valid frame chain (Windows requires this for ETW/fast stack walking; we follow it at every optimisation level, as MSVC does). `sp` is not allocatable. `x16` and `x17` are reserved as scratch for back-end expansions that occur after allocation (large immediates, out-of-range frame offsets, parallel-copy cycle breaking, jump tables) and are never live across a call, since `link.exe` range-extension thunks may clobber them. `x15` is used by the `__chkstk` sequence only within the prologue.

**`-O0`: fast local allocator.** Each block is allocated independently; every virtual register is assigned a stack slot, values are loaded into registers on first use within the block and stored at definition. This is quick to compile, trivially correct for debugging, and produces code comparable to MSVC `/Od`.

**`-O1`/`-O2`: SSA linear scan with splitting**, following Wimmer & Franz ("Linear Scan Register Allocation on SSA Form", CGO 2010). Blocks are ordered in a linearisation that keeps loops contiguous; lifetime intervals are built from SSA liveness in one backward pass; intervals are split at use positions to fit holes, with spill/reload placed at block boundaries with the lowest loop depth; spill slots are shared between non-overlapping intervals. Register hints come from fixed-register constraints (call arguments, return values) and from block-parameter copies, which removes most copies without a separate coalescing pass. The choice of callee-saved versus caller-saved registers is driven by whether an interval crosses a call: crossing intervals prefer `x19–x28` and `d8–d15`, others prefer `x0–x15` and `v0–v7`, `v16–v31`. With 28 allocatable general registers and 32 FP/SIMD registers, the quality gap to graph colouring on ARM64 is small and the compile-time saving is large.

After allocation, block parameters become parallel copies on edges (split critical edges first), sequentialised with cycle breaking via `x16`/`v31`.

**Post-RA passes.** Load/store pairing (`ldr`/`ldr` and `str`/`str` at adjacent offsets → `ldp`/`stp`), redundant copy elimination, compare elimination (reuse the flags of a preceding `subs`/`adds`/`ands` instead of a separate `cmp`), and expansion of the `ATOMIC_*_LOOP` pseudos (`CAS` loops for read-modify-write operations without an LSE instruction, and LL/SC loops under `/arch:armv8.0`). LL/SC loops are expanded *after* allocation so that no spill code can land between `ldaxr` and `stlxr` (which would clear the exclusive monitor and livelock).

## 5. Frame layout

Frame lowering selects one of a small number of *canonical shapes* that map 1:1 to Windows unwind codes ([09-unwind-and-seh.md](09-unwind-and-seh.md)). Non-canonical prologues are never emitted. From high to low addresses:

```
        +-------------------------------+  <- caller's sp at entry (16-aligned)
        | incoming stack arguments      |
        +-------------------------------+
        | saved x19..x28 (pairs)        |   int save area
        | saved d8..d15 (pairs)         |   FP save area
        | homed x0..x7 (variadic only)  |   home area, 64 bytes
        +-------------------------------+
        | locals, spill slots           |
        | saved x29, x30                |  <- x29 (frame pointer)
        +-------------------------------+
        | outgoing stack arguments      |  <- sp (only if any call passes args on the stack)
        +-------------------------------+
        | dynamic area (VLA / _alloca)  |  grows below; sp moves, x29 does not
```

The common case (non-variadic, no outgoing stack arguments, locals plus the `x29`/`x30` pair ≤ 504 bytes) produces exactly the canonical packed form. Microsoft's packed-unwind table allows the pre-indexed `stp x29, x30, [sp, #-N]!` form up to N = 512, but the mirrored epilogue instruction `ldp x29, x30, [sp], #512` is not encodable (the scaled 7-bit offset tops out at +504), so at exactly 512 we use the `sub sp` form and a full record. The phase-0 test sweep found this.

```asm
    stp     x19, x20, [sp, #-32]!     // save_regp_x  (int save area)
    stp     x21, x22, [sp, #16]       // save_regp
    stp     x29, x30, [sp, #-48]!     // save_fplr_x  (locals + fp/lr)
    mov     x29, sp                   // set_fp
    ...
    ldp     x29, x30, [sp], #48
    ldp     x21, x22, [sp, #16]
    ldp     x19, x20, [sp], #32
    ret
```

Locals are then addressable from both `sp` and `x29` with positive scaled offsets. Frames with locals > 512 bytes use the `sub sp` + `stp x29, x30, [sp]` + `add x29, sp, #0` form. Frames ≥ 4096 bytes probe the stack: after saving registers and `x29`/`x30`, `mov x15, #(size/16)`, `bl __chkstk`, `sub sp, sp, x15, lsl #4` (`__chkstk` is provided by the MSVC CRT libraries and preserves `x15`). Functions with a dynamic area allocate it with the same `__chkstk` protocol at run time, address locals through `x29` only, and restore `sp` from `x29` in the epilogue (`mov sp, x29`, which has its own unwind code).

Leaf functions that need no stack and save no registers get no prologue and no `.pdata` entry, as the ABI recommends.

Shrink-wrapping (saving callee-saved registers only on paths that need them) is not in v1: it requires function-fragment unwind records (`end_c` phantom prologues) and is a measurable but modest win.

## 6. Layout and branch relaxation

Blocks are laid out in reverse post-order with loop bodies contiguous and cold blocks (`unreachable`-terminated, `noreturn` call paths, `[[gnu::cold]]`) moved to the end of the function. Branch ranges are ±128 MiB for `b`/`bl`, ±1 MiB for `b.cond`/`cbz`/`cbnz`/`ldr literal`/`adr`, ±32 KiB for `tbz`/`tbnz`. Relaxation computes block offsets, and for each out-of-range conditional branch inverts the condition around an unconditional `b`, iterating to a fixed point (offsets only grow, so it terminates). Calls and branches to other sections are always emitted as relocations; `link.exe` inserts range-extension thunks for `BRANCH26` targets beyond ±128 MiB.

A single function larger than 1 MiB of code exceeds the `.xdata` function-length field and must be split into fragments. This is rare in hand-written C but possible in generated code; v1 emits a diagnostic, fragments are a later addition.

## 7. Encoder

The encoder walks final MIR and writes 32-bit little-endian words into the section buffer. Intra-function label references (branches, `adr`, jump tables) are resolved when the function is complete. References to symbols produce relocation records ([08-coff-object-writer.md](08-coff-object-writer.md) §5) with the addend encoded in the instruction's immediate field, as COFF ARM64 uses implicit addends. Logical immediates use the standard bitmask-immediate encoder (rotate/replicate search). Every encoding form is covered by a differential test that encodes all operand classes and compares against `llvm-mc --triple=aarch64-pc-windows-msvc --show-encoding`.

The `-S` / `/FA` output is GNU/LLVM-syntax assembly with source-line annotations. It is produced from the same final MIR the encoder consumes, so it is exact, and it is accepted by `llvm-mc` so listings can be reassembled for comparison. It is not a build path.

## 8. Target features

| Switch | Effect |
|--------|--------|
| `/arch:armv8.0` | Fallback for ARMv8.0-only CPUs: FP, Advanced SIMD, LL/SC atomics |
| `/arch:armv8.1` (default) | Adds LSE atomics (`cas`, `casp`, `ldadd`, `swp`…) |
| `/arch:armv8.2` | adds `+fp16` arithmetic, `+rcpc` (`ldapr` for acquire loads) when also requested |
| `/feature:+crc,+crypto,+dotprod,+lse,+fp16,+rcpc` | Individual features |

Code that uses a feature not enabled by the target switch is still allowed through intrinsics (the programmer is responsible for a runtime check), as MSVC does.

## 9. Security features (post-v1, designed in now)

`/GS` stack cookies: the prologue loads `__security_cookie`, mixes it with the frame pointer and stores it below the locals' arrays; the epilogue reloads, unmixes and calls `__security_check_cookie` (exact sequence to be matched against `cl` output). `/guard:cf`: indirect calls go through `__guard_check_icall_fptr` with the target in `x15`, address-taken functions are listed in `.gfids$y`, and `@feat.00` gets the CFG bit. Pointer authentication (`pacibsp`/`autibsp` with the `pac_sign_lr` unwind code) is supported by the unwind format and can be offered as `/Qpac-ret`. All three affect prologue/epilogue shape, which is why the frame lowering's canonical-shape table has room for them from the start.

## Decisions

No pre-RA scheduler in v1; measure on hardware first (Q-15). No function fragments in v1; functions over 1 MiB of code are diagnosed (Q-16). ARMv8.1 baseline (Q-06).
