# 09 — Unwind data and structured exception handling

Unwind data is not optional on Windows ARM64. The OS unwinder is used by SEH dispatch, `longjmp`, `RtlCaptureStackBackTrace`, ETW sampling, WER crash dumps and the debugger; any function that manipulates the stack and lacks correct unwind data breaks all of them, often far away from the bug. This document is the specification for producing that data correctly, and for proving it correct (R2).

## 1. Principle: generate the prologue and the unwind codes from one description

Frame lowering ([06-arm64-backend.md](06-arm64-backend.md) §5) does not emit instructions directly. It produces a *frame plan*: an ordered list of abstract operations, each of which is exactly one instruction and exactly one unwind code:

| Frame-plan op | Instruction | Unwind code |
|---------------|-------------|-------------|
| `SaveR19R20X(off)` | `stp x19, x20, [sp, #-off]!` | `save_r19r20_x` |
| `SaveRegPX(r, off)` | `stp xr, xr+1, [sp, #-off]!` | `save_regp_x` |
| `SaveRegP(r, off)` | `stp xr, xr+1, [sp, #off]` | `save_regp` (or `save_next` after a pair) |
| `SaveReg(r, off)` / `SaveRegX` | `str xr, [sp, #off]` / pre-indexed | `save_reg` / `save_reg_x` |
| `SaveLrPair(r, off)` | `stp xr, lr, [sp, #off]` | `save_lrpair` |
| `SaveFRegP(r, off)` / `SaveFReg` | `stp dr, dr+1, [sp, #off]` / `str` | `save_fregp` / `save_freg` (and `_x` forms) |
| `SaveFpLrX(off)` | `stp x29, lr, [sp, #-off]!` | `save_fplr_x` |
| `SaveFpLr(off)` | `stp x29, lr, [sp, #off]` | `save_fplr` |
| `SetFp` | `mov x29, sp` | `set_fp` |
| `AddFp(off)` | `add x29, sp, #off` | `add_fp` |
| `Alloc(n)` | `sub sp, sp, #n` (one or two instructions) | `alloc_s` / `alloc_m` / `alloc_l` per size |
| `HomeArgs` | `stp x0, x1, [sp, #off]` …×4 | `nop` ×4 |
| `ChkStk` | `mov x15, #n` ; `bl __chkstk` ; `sub sp, sp, x15, lsl #4` | `nop`, `nop`, `alloc_*` |
| `PacSignLr` | `pacibsp` | `pac_sign_lr` |

The prologue emitter and the unwind-code emitter both iterate the same plan, so they cannot disagree by construction. Epilogues are generated from the reversed plan (loads instead of stores, post-indexed instead of pre-indexed, `add sp` instead of `sub sp`, `mov sp, x29` for `set_fp` in functions with dynamic allocation, `HomeArgs` dropped), and terminate in `ret` (unwind code `end`) or a tail-call `b`/`br` (which is the same size as `ret`, so the same `end` suffices).

## 2. Choosing packed versus full records

For each function, after final layout:

1. If the function has no frame (leaf, no saves, no stack), emit nothing.
2. Otherwise, try to express the frame plan as *packed unwind data* (`.pdata` flag 1): this requires the canonical order (int pairs, optional `lr`, FP pairs, optional home area, then the `x29`/`lr` allocation step), function length ≤ 8 KiB, frame size ≤ 8 KiB − 16, no outgoing stack-argument area below `x29`, a single epilogue that mirrors the prologue, no exception handler, and no `mov sp, x29` restore. The packer computes `RegI`, `RegF`, `CR`, `H` and `FrameSize`, then *re-expands* the packed form into a frame plan using the algorithm in Microsoft's documentation and checks it is identical to the plan actually emitted. Only if identical is the packed form used.
3. Otherwise emit a full `.xdata` record: header (function length / 4, `X` bit when there is a handler, `E` bit when there is exactly one epilogue whose codes are a suffix of the prologue's, epilogue count, code words), epilogue scopes (start offset / 4, start index), the unwind-code byte array (prologue codes first, in reverse execution order, terminated by `end`; then any epilogue-specific sequences), padded to a word, then the optional handler RVA and handler data.

Multiple epilogues are common at `-O2` (early returns duplicated by tail merging in reverse). Each is an epilogue scope; since our epilogues are always the exact mirror of the prologue, they share the prologue's codes, so all scopes point at index 0 (or at the index past `set_fp` when the epilogue omits `mov sp, x29`). The 255-code-index limit is therefore never approached.

## 3. Verifier

The unwind verifier (implemented in phase 0 as `unwind_verify`, `src/target/arm64/unwind.c`) runs on every function the module emits. It interprets both sides concretely rather than symbolically: it decodes the actual encoded instruction words, not the MIR, and seeds every register with a distinct sentinel value, which exposes the same errors more simply. It checks:

* It executes the emitted prologue instructions one at a time over a symbolic machine state (`sp`, `x29`, `lr`, saved-register stack slots, all as expressions over the entry state), and at each instruction boundary *k* it runs the unwind codes the way the Windows unwinder would for a PC at offset *k* within the prologue (skipping the codes for not-yet-executed instructions), checking that the recovered entry `sp`, return address and every callee-saved register equal their entry values.
* It does the same for every instruction boundary in every epilogue scope (skipping the already-executed codes from the epilogue's start index).
* For one representative body PC, it runs the full code sequence and checks the same.
* It decodes packed `.pdata` through the documented expansion and checks equivalence with the full form.

This covers exactly the class of bugs that are otherwise found only when a sampling profiler hits a prologue. The test suite proves the verifier has teeth by mutation: every single-field change to a prologue instruction, and swapping the first two prologue instructions, must be rejected. For every bit-flip in a full `.xdata` record's unwind codes, the test names the exact set allowed to survive: padding after the final `end`, which is never executed, and the size of an `alloc` that is immediately undone by `set_fp` in unwind order. That size is semantically dead for any unwinder, ours or the OS's. As an independent cross-check, CI also runs `llvm-readobj --unwind` and, on Windows, a harness that calls `RtlVirtualUnwind` at every instruction offset of every function in a test DLL against a context reconstructed by single-stepping (debug API), which tests against the real OS unwinder rather than our reading of the spec.

## 4. `setjmp`/`longjmp`

UCRT's ARM64 `longjmp` can restore state by unwinding through intervening frames, which is the reason correct unwind data matters even in pure C. On the compiler side, a call to `setjmp`/`_setjmp`/`_setjmpex` is marked `returns_twice` in NIR: the optimiser does not keep values in caller-saved registers across it, does not move loads/stores across it, does not tail-merge around it, and does not inline a function containing it. Locals that are modified between `setjmp` and `longjmp` without being `volatile` are indeterminate per the standard, so no further protection is required.

## 5. Structured exception handling (`__try`)

SEH is part of the MSVC compatibility surface ([04-msvc-compatibility.md](04-msvc-compatibility.md) §3) and ships in v1.0, in roadmap phase 6 (Q-20). Its representation is fixed now so the IR and frame design do not have to change when it lands.

**Language semantics.** `__try { } __except (filter) { }` and `__try { } __finally { }` with `__leave`, `GetExceptionCode()`, `GetExceptionInformation()` and `AbnormalTermination()`, following MSVC's C behaviour. Hardware faults inside the guarded region (access violations, `brk`-raised exceptions such as `__fastfail` and `__debugbreak`) must be catchable, which constrains optimisation (below). Integer division by zero does not trap on ARM64 (`sdiv` returns 0); whether to insert MSVC-style checks that raise `STATUS_INTEGER_DIVIDE_BY_ZERO` is decided by matching `cl`'s behaviour.

**Runtime model.** Functions with `__try` get a full `.xdata` record with `X = 1`, handler RVA = `__C_specific_handler` (from vcruntime), and handler data = a scope table: count, then per scope `BeginAddress`, `EndAddress` (RVAs of the guarded range), `HandlerAddress` (RVA of the filter function, or `1` for `EXCEPTION_EXECUTE_HANDLER` constant filters, or the `__finally` funclet), `JumpTarget` (RVA of the `__except` block, or 0 for `__finally`). All four are `ADDR32NB` relocations.

**Code shape.** Filters and `__finally` blocks are outlined into separate functions (*funclets*) that receive the establisher frame pointer and access the parent's locals through it; this matches what `__C_specific_handler` calls on ARM64 and is the same structure `cl` produces. The funclets have their own `.pdata`/`.xdata`. The exact parameter convention for the establisher frame is to be matched against `cl` disassembly before implementation (Q-19).

**Optimisation constraints.** Within a `__try` region, every instruction that may fault is an implicit edge to the handler. Rather than model that precisely (as LLVM does with `invoke`-like constructs), v1 takes MSVC's own conservative approach: any local referenced from a filter, `__except` block or `__finally` block, or live across a guarded region boundary, is kept in memory (treated as address-taken and `volatile` for the duration of the region); instructions are not moved into or out of guarded regions; and the guarded region's code is laid out contiguously so that it is a single `[Begin, End)` range.

## Decisions and open items

SEH is in v1.0 (Q-20). Still open: the establisher-frame convention for funclets, to be determined from `cl` output before phase 6 (Q-19).
