# 05 — IR and optimiser

## 1. Design principles

There is one mid-level IR, `NIR`. It is typed, in SSA form with block parameters instead of φ-nodes, and deliberately close to the machine: by the time code is in NIR, C's type system, implicit conversions, aggregate assignment and the calling convention have all been lowered away. NIR has no first-class aggregates; structs live in memory and move with `memcpy`. This keeps the instruction set at roughly 90 opcodes, which is what a self-hosted C implementation can afford to optimise well (R3).

Block parameters rather than φ-nodes because they make CFG edits local (adding an edge means adding arguments to one branch, not patching φ-nodes in the successor), they map directly onto parallel copies at register allocation, and they are easier to verify. This is the Cranelift/MLIR/Swift SIL choice.

## 2. Types

`i1`, `i8`, `i16`, `i32`, `i64`, `i128`, `iN` (for `_BitInt` wider than 128, legalised before instruction selection), `f16`, `f32`, `f64`, `ptr` (64-bit, single address space), and `v64`/`v128` (opaque SIMD types, element shape carried by the operation, used by intrinsics and inline `memcpy`/`memset` expansion). Integer types carry no signedness; operations do (`sdiv`/`udiv`, `sext`/`zext`, `icmp slt`/`ult`), as in LLVM. `long double` is `f64`.

## 3. Instructions

| Group | Instructions |
|-------|-------------|
| Constants | `iconst`, `fconst`, `symaddr sym+off`, `tlsaddr sym`, `undef` |
| Integer | `add sub mul sdiv udiv srem urem and or xor shl lshr ashr`, `smulh umulh`, `neg not`, `clz ctz popcount bswap bitreverse rotl rotr`, overflow-checked `sadd.ov uadd.ov ssub.ov usub.ov smul.ov umul.ov` (for `<stdckdint.h>`) |
| Floating | `fadd fsub fmul fdiv fneg fabs fsqrt fma fmin fmax fminnum fmaxnum fround*`, constrained variants (§6) |
| Conversion | `trunc sext zext fptosi fptoui sitofp uitofp fpext fptrunc bitcast ptrtoint inttoptr` |
| Comparison / select | `icmp <pred>`, `fcmp <pred>` (ordered/unordered predicates), `select` |
| Memory | `load ty, ptr [align, volatile, atomic(order), unaligned]`, `store`, `alloca size, align` (entry block only, static), `dynalloca` (VLAs, `_alloca`), `memcpy memmove memset` (with constant-size fast paths), `ptradd ptr, offset` |
| Atomics | `atomicrmw <op> order`, `cmpxchg order_s order_f [weak]`, `fence order`, `signal_fence` |
| Calls | `call callee(args) attrs`, `call_indirect`, `tailcall`; call attributes `noreturn readnone readonly returns_twice nounwind cold` |
| Control | `br`, `brif cond, T(args), F(args)`, `switch val, default(args), [case → block(args)]`, `ret`, `unreachable`, `trap` |
| Intrinsics | `intrinsic id(args)` for target operations with no generic form (status registers, barriers, `x18` access, NEON) |
| Debug | `dbg.value var, value`, `dbg.declare var, slot` (consumed by CodeView emission, ignored by optimisations except for update) |

Pointer arithmetic is `ptradd` with a byte offset: there is no typed GEP. Sema has already computed element sizes, and alias analysis derives what it needs from the underlying object plus constant offset, which is simpler and sufficient for C.

Each instruction has a source location (32-bit), used for line tables and diagnostics emitted from the back end (e.g. "stack frame exceeds 1 GiB").

## 4. IR generation

IR generation walks the typed AST of one function. At `-O0` and as the input to `mem2reg` at higher levels, every local is an `alloca` and every access is a `load`/`store`, exactly like Clang's IRGen; SSA construction then promotes non-escaping scalars.

**Calling-convention lowering happens here**, not in the back end. IRGen classifies each parameter and return value per [07-windows-arm64-abi.md](07-windows-arm64-abi.md) and produces a NIR function signature in terms of scalar machine types plus explicit hidden pointers: an HFA of three `double`s becomes three `f64` parameters, a 24-byte struct becomes a `ptr` to a caller-made copy, a `struct` return larger than 16 bytes becomes a `ptr` parameter marked `sret` (assigned to `x8`). The back end then only has to assign NIR parameters to registers in order, which is trivial and makes the ABI auditable in one place. The cost, accepted, is that NIR is ARM64-Windows-specific; D-05 says that is fine.

C semantics that IRGen makes explicit: integer promotions and conversions; `_Bool` normalisation on store; bit-field access as load/shift/mask/store sequences on the allocation unit (MSVC unit size, §04); `volatile` accesses; sequence-point-free evaluation order (left-to-right for function arguments and operands, documented as implementation behaviour (Q-14)); VLA size computation and `dynalloca` with stack probing; `setjmp` call sites marked `returns_twice`; `_BitInt` arithmetic as `iN` operations; complex arithmetic expanded inline, with `__muldc3`/`__divdc3`-style runtime calls for the Annex G-correct multiplication and division unless `CX_LIMITED_RANGE` is on.

`switch` statements become NIR `switch`; the back end decides between jump table, binary search and bit tests.

## 5. Pass pipeline

### `-O0`
IRGen → verifier → instruction selection. No SSA construction. All variables live in memory, which is what makes `-O0` debugging exact.

### `-O1`
1. `mem2reg` (SSA construction, Braun et al. "Simple and Efficient Construction of SSA Form", which needs no dominance frontiers and works on the fly).
2. `simplifycfg` (merge blocks, remove unreachable code, fold constant branches, thread trivial jumps).
3. `sccp` (sparse conditional constant propagation).
4. `instsimplify` + `combine` (local algebraic simplification: identities, constant folding, strength reduction of `mul`/`udiv` by constants, comparison canonicalisation, `select` formation). Written as a table of rewrite rules in `combine.def` with a match/replace X-macro DSL.
5. `dce` (aggressive dead code elimination with control dependence).
6. `early-cse` (dominator-scoped hash-based value numbering, including `load`s not separated by clobbers).
7. `simplifycfg` again.

### `-O2` (adds)
8. `inline` — bottom-up over call-graph SCCs; cost model in estimated machine instructions; always inline `__forceinline`/`always_inline`; threshold 225 "units" at `-O2` with bonuses for constant arguments and single call sites; never inline into or out of functions containing `__try` or calling `setjmp` (v1 simplification).
9. `sroa` (scalar replacement of aggregates: split `alloca`s of structs/arrays accessed at constant offsets into independent scalars, then `mem2reg` them). Essential in C because struct-heavy code otherwise stays in memory.
10. `gvn` (global value numbering with load elimination using memory SSA-lite: per-block clobber summaries plus the alias analysis below).
11. Loop passes on natural loops in loop-simplify form (preheader, single latch, dedicated exits): `licm` (including hoisting of `readnone`/`[[unsequenced]]` calls), `indvars` (canonical induction variables, exit-value replacement), `lsr` (loop strength reduction targeting ARM64 addressing modes: base+imm, base+reg LSL #size, post-increment), `unroll` (full unroll of small constant-trip loops; partial unroll by 2/4 for tiny bodies).
12. `dse` (dead store elimination, including stores followed by `memset`/`memcpy` overwrite).
13. `jump-threading`, `tailcall` (sibling-call marking when the callee's stack argument area fits within the caller's).
14. Final `combine`, `dce`, `simplifycfg`.

The pipeline is a static array of pass descriptors; `/Qpasses:` lets tests run an arbitrary sequence on textual NIR, which is how passes are unit-tested.

## 6. Semantic policy for optimisation

These are deliberate positions, and they matter more than the pass list for whether real Windows code works (R5).

**Signed overflow.** Signed `add`/`sub`/`mul` from C carry an `nsw` flag and the optimiser may assume no overflow, as C permits. This enables induction-variable widening and is how every competitive compiler behaves. `/Qwrapv` clears the flag globally. MSVC also exploits signed overflow in loop optimisation, so this is not a compatibility hazard in practice.

**Strict aliasing.** Type-based alias analysis is **off by default** because MSVC has never performed it and Windows code depends on that. `/Qstrict-aliasing` enables it using C's effective-type rules (character types alias everything; signed/unsigned variants alias each other). The default alias analysis is "basic AA": distinct `alloca`s, distinct globals, an `alloca` that does not escape versus anything else, `restrict` parameters (C23 semantics: a `restrict` pointer's accessed objects are not accessed through other pointers within the block), and constant-offset disjointness from a common base.

**Null and out-of-bounds.** Dereferencing null is undefined, but we do not delete null checks after dereferences (the GCC `-fdelete-null-pointer-checks` behaviour) by default, because kernel-adjacent Windows code and SEH-based access patterns make page-zero access meaningful. Out-of-bounds analysis is not used for optimisation.

**Undefined values.** NIR has `undef` but not LLVM-style `poison`. Transformations that would need poison semantics (e.g. hoisting a division that might trap) are simply not performed. This loses a small amount of optimisation and removes a well-known class of miscompilation.

**Floating point.** Default `/fp:precise`: no reassociation, no contraction (`FP_CONTRACT OFF`), `-0.0`, NaN and infinity semantics preserved, `x + 0.0` not folded. `/fp:contract` allows `fma` formation. `/fp:fast` allows reassociation, reciprocal approximation, no-NaN/no-inf assumptions. Under `FENV_ACCESS ON` (or `/fp:strict`), FP operations use constrained forms that the optimiser treats as having side effects on FPCR/FPSR: no constant folding that could change exception flags, no speculation, no motion across calls.

**`volatile`.** ISO semantics: every access performed, exactly once, in program order relative to other volatile accesses, with no implied barriers. This matches MSVC's ARM64 default (`/volatile:iso`). `/volatile:ms` (acquire/release on volatile) is provided because some MSVC-era code assumes it.

**Memory model.** C23 atomics with the C/C++ memory model. Lowering to ARM64 follows the standard mapping (LDAR/STLR for acquire/release loads and stores, LSE `CAS`/`CASP`/`LDADD`/`LDCLR`/`LDSET`/`LDEOR`/`SWP` with the matching `A`/`L`/`AL` ordering suffix by default, `CAS` loops for operations without a direct LSE form, LDAXR/STLXR loops only under `/arch:armv8.0`, `DMB ISH` for `seq_cst` fences). The optimiser may not move non-atomic accesses across acquire/release operations in the forbidden directions and treats `seq_cst` as a full barrier.

**`[[unsequenced]]` and `[[reproducible]]`** are trusted: a call to an `[[unsequenced]]` function with identical arguments may be CSE'd and hoisted; `[[reproducible]]` calls may be CSE'd if no intervening store may alias.

## 7. Analyses

Dominator tree (Cooper–Harvey–Kennedy iterative algorithm, sufficient at our function sizes), post-dominators, natural loop forest, call graph (direct calls; indirect calls conservatively), basic alias analysis, escape analysis for `alloca`s, and a liveness analysis shared with the back end. Analyses are cached per function and invalidated by passes that declare what they preserve.

## 8. `_BitInt` legalisation

Before instruction selection, `iN` operations with N > 128 are expanded into loops or unrolled sequences over 64-bit limbs (add/sub with carry via `adds`/`adcs`, multiplication by schoolbook limb products for N ≤ 512 and a runtime call above that, division and remainder always via `__ncrt_udivmodbi`/`__ncrt_sdivmodbi` in `libncrt`). Widths between 65 and 128 use `i128`, legalised to register pairs by the back end. Non-power-of-two widths ≤ 128 are kept in the next container type with explicit re-extension after each operation, because AAPCS64 leaves the padding bits unspecified ([07-windows-arm64-abi.md](07-windows-arm64-abi.md) §5).

## Decisions

Type-based aliasing is never on by default in any mode (Q-13); argument evaluation is left-to-right (Q-14).
