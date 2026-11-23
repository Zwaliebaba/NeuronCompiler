/* unwind.h — frame plans, prologue/epilogue generation and Windows ARM64
 * unwind data (Design/09-unwind-and-seh.md).
 *
 * A frame plan is a list of steps, each of which is exactly one prologue
 * instruction and exactly one unwind code. Prologue, epilogue and unwind
 * codes are all generated from the same plan so they cannot disagree. */
#ifndef NC_UNWIND_H
#define NC_UNWIND_H

#include "../../core/buf.h"
#include "a64.h"

typedef struct FrameDesc
{
  u8 nGpr;        /* callee-saved x19.. (consecutive) to save, 0..10 */
  u8 nFpr;        /* callee-saved d8.. (consecutive) to save, 0..8 */
  bool homeArgs;  /* variadic: home x0-x7 next to the incoming stack arguments */
  bool dynamic;   /* has dynamic stack allocation (VLA / _alloca) */
  bool frameless; /* leaf with no stack use at all: no prologue, no unwind data */
  u32 locals;     /* bytes of locals and spill slots */
  u32 outgoing;   /* bytes of outgoing stack-argument area */
} FrameDesc;

typedef enum FrameStepKind : u8
{
  FrameStepKindStpXPre, /* stp xR, xR+1, [sp, #-off]! */
  FrameStepKindStpX,    /* stp xR, xR+1, [sp, #off]   */
  FrameStepKindStrXPre, /* str xR, [sp, #-off]!       */
  FrameStepKindStrX,    /* str xR, [sp, #off]         */
  FrameStepKindStpDPre,
  FrameStepKindStpD,
  FrameStepKindStrDPre,
  FrameStepKindStrD,
  FrameStepKindHome,    /* stp xR, xR+1, [sp, #off] (argument homing, unwind nop) */
  FrameStepKindFplrPre, /* stp x29, x30, [sp, #-off]! */
  FrameStepKindFplr,    /* stp x29, x30, [sp, #off]   */
  FrameStepKindSetFp,   /* mov x29, sp */
  FrameStepKindAlloc,   /* sub sp, sp, #off */
  FrameStepKindChkMovz, /* movz x15, #off */
  FrameStepKindChkMovk, /* movk x15, #off, lsl #16 */
  FrameStepKindChkBl,   /* bl __chkstk */
  FrameStepKindChkSub,  /* sub sp, sp, x15, uxtx #4   (off = bytes allocated) */
} FrameStepKind;

typedef struct FrameStep
{
  FrameStepKind kind;
  u8 reg;
  u32 off;
} FrameStep;

typedef enum FrameShape : u8
{
  FrameShapeNone, /* frameless leaf */
  FrameShapeA,    /* x29 at the bottom of the locals; packable */
  FrameShapeB,    /* x29 directly below the save area; general (large, dynamic, outgoing) */
} FrameShape;

enum
{
  FRAME_MAX_STEPS = 32
};

typedef struct FramePlan
{
  FrameShape shape;
  FrameStep steps[FRAME_MAX_STEPS];
  u32 nsteps;
  u8 nGpr, nFpr;
  bool homeArgs, dynamic;
  u32 saveSize;       /* register save + home area, 16-aligned */
  u32 frameSize;      /* total bytes the prologue allocates */
  u32 localsSpOffset; /* sp-relative offset of the locals area after the prologue */
  u32 localsSize;
  u32 homeSpOffset; /* sp-relative offset of homed x0 (if home_args) */
  u32 fpSpOffset;   /* x29 - sp after the prologue */
} FramePlan;

void FramePlanBuild(const FrameDesc* _d, FramePlan* _p);

/* Instruction counts of the generated sequences (epilogue includes ret). */
u32 FramePrologueLen(const FramePlan* _p);
u32 FrameEpilogueLen(const FramePlan* _p);

/* Generates the sequences into `out` (capacity FRAME_MAX_STEPS + 1).
 * `chkstk_sym` is the module symbol id for __chkstk (used only if needed). */
u32 FramePrologue(const FramePlan* _p, MirInst* _out, u32 _chkstkSym);
u32 FrameEpilogue(const FramePlan* _p, MirInst* _out);

/* ---- unwind records ----------------------------------------------------- */

typedef struct UnwindRecord
{
  bool none;   /* frameless: no .pdata entry */
  bool packed; /* .pdata second word is `packed_word` */
  u32 packedWord;
  Buf xdata; /* otherwise: the .xdata record */
} UnwindRecord;

/* Builds the unwind record for a function of `func_bytes` bytes whose
 * epilogues start at the given byte offsets. */
void UnwindBuild(const FramePlan* _p, u32 _funcBytes, const u32* _epilogs, u32 _nepilogs, UnwindRecord* _out);
void UnwindRecordFree(UnwindRecord* _r);

/* Verifies the record against the encoded function by emulating every
 * prefix of the prologue and every epilogue and unwinding from each
 * instruction boundary the way the Windows unwinder does. Returns false and
 * fills `err` on the first discrepancy. */
bool UnwindVerify(const FramePlan* _p, const u8* _code, u32 _funcBytes, const u32* _epilogs, u32 _nepilogs, const UnwindRecord* _r,
                  char* _err, size_t _errsz);

/* ---- checking records read back from a linked image -------------------- */

/* Register state handed to an external unwinder (RtlVirtualUnwind). */
typedef struct UwMachine
{
  u64 x[31]; /* x29 = fp, x30 = lr */
  u64 sp;
  u64 d[32]; /* low 64 bits of v0..v31 */
  u64 pc;    /* after unwinding: the return address */
} UwMachine;

/* Unwinds one frame from function-relative byte offset `func_offset` in
 * machine state `state`, whose stack memory consists of the given
 * (address, value) words (absolute addresses near the entry sp). */
typedef bool (*UwOsUnwindFn)(void* _ctx, u32 _funcOffset, const UwMachine* _state, const u64* _memAddr, const u64* _memVal, u32 _nmem,
                             UwMachine* _out);

typedef enum UwCheckResult : u8
{
  UwCheckResultOk,      /* every prologue/epilogue boundary checked */
  UwCheckResultPartial, /* prologue and body checked; epilogue location not recorded (E bit) */
  UwCheckResultSkipped, /* uses instructions or unwind codes the verifier does not model */
  UwCheckResultFail,    /* an unwinder recovered the wrong state */
} UwCheckResult;

/* Rebuilds a record from an image's .pdata second word and the bytes at the
 * referenced .xdata RVA (`xdata` may be null for packed entries). */
bool UnwindRecordFromPdata(u32 _word2, const u8* _xdata, size_t _avail, UnwindRecord* _out, u32* _funcBytes);

/* The verifier's RunSweep, driven only by the record: emulates every prologue
 * prefix and epilogue boundary with entry stack pointer `entry_sp` and checks
 * that our unwinder and, if `os` is non-null, the external one both recover
 * the entry state. */
UwCheckResult UnwindCrosscheck(const u8* _code, u32 _funcBytes, const UnwindRecord* _r, u64 _entrySp, UwOsUnwindFn _os, void* _osCtx,
                               u32* _checks, char* _err, size_t _errsz);

/* Decodes unwind-code bytes into a human-readable listing (for -S and tests). */
void UnwindFormatCodes(const u8* _codes, u32 _len, Buf* _out);

#endif
