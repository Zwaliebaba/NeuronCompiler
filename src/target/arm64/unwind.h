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

typedef struct FrameDesc {
    u8 n_gpr;       /* callee-saved x19.. (consecutive) to save, 0..10 */
    u8 n_fpr;       /* callee-saved d8.. (consecutive) to save, 0..8 */
    bool home_args; /* variadic: home x0-x7 next to the incoming stack arguments */
    bool dynamic;   /* has dynamic stack allocation (VLA / _alloca) */
    bool frameless; /* leaf with no stack use at all: no prologue, no unwind data */
    u32 locals;     /* bytes of locals and spill slots */
    u32 outgoing;   /* bytes of outgoing stack-argument area */
} FrameDesc;

typedef enum FrameStepKind : u8 {
    FS_STP_X_PRE, /* stp xR, xR+1, [sp, #-off]! */
    FS_STP_X,     /* stp xR, xR+1, [sp, #off]   */
    FS_STR_X_PRE, /* str xR, [sp, #-off]!       */
    FS_STR_X,     /* str xR, [sp, #off]         */
    FS_STP_D_PRE, FS_STP_D, FS_STR_D_PRE, FS_STR_D,
    FS_HOME,      /* stp xR, xR+1, [sp, #off] (argument homing, unwind nop) */
    FS_FPLR_PRE,  /* stp x29, x30, [sp, #-off]! */
    FS_FPLR,      /* stp x29, x30, [sp, #off]   */
    FS_SET_FP,    /* mov x29, sp */
    FS_ALLOC,     /* sub sp, sp, #off */
    FS_CHK_MOVZ,  /* movz x15, #off */
    FS_CHK_MOVK,  /* movk x15, #off, lsl #16 */
    FS_CHK_BL,    /* bl __chkstk */
    FS_CHK_SUB,   /* sub sp, sp, x15, uxtx #4   (off = bytes allocated) */
} FrameStepKind;

typedef struct FrameStep {
    FrameStepKind kind;
    u8 reg;
    u32 off;
} FrameStep;

typedef enum FrameShape : u8 {
    SHAPE_NONE, /* frameless leaf */
    SHAPE_A,    /* x29 at the bottom of the locals; packable */
    SHAPE_B,    /* x29 directly below the save area; general (large, dynamic, outgoing) */
} FrameShape;

enum { FRAME_MAX_STEPS = 32 };

typedef struct FramePlan {
    FrameShape shape;
    FrameStep steps[FRAME_MAX_STEPS];
    u32 nsteps;
    u8 n_gpr, n_fpr;
    bool home_args, dynamic;
    u32 save_size;        /* register save + home area, 16-aligned */
    u32 frame_size;       /* total bytes the prologue allocates */
    u32 locals_sp_offset; /* sp-relative offset of the locals area after the prologue */
    u32 locals_size;
    u32 home_sp_offset;   /* sp-relative offset of homed x0 (if home_args) */
    u32 fp_sp_offset;     /* x29 - sp after the prologue */
} FramePlan;

void frame_plan(const FrameDesc *d, FramePlan *p);

/* Instruction counts of the generated sequences (epilogue includes ret). */
u32 frame_prologue_len(const FramePlan *p);
u32 frame_epilogue_len(const FramePlan *p);

/* Generates the sequences into `out` (capacity FRAME_MAX_STEPS + 1).
 * `chkstk_sym` is the module symbol id for __chkstk (used only if needed). */
u32 frame_prologue(const FramePlan *p, MInst *out, u32 chkstk_sym);
u32 frame_epilogue(const FramePlan *p, MInst *out);

/* ---- unwind records ----------------------------------------------------- */

typedef struct UnwindRecord {
    bool none;        /* frameless: no .pdata entry */
    bool packed;      /* .pdata second word is `packed_word` */
    u32 packed_word;
    Buf xdata;        /* otherwise: the .xdata record */
} UnwindRecord;

/* Builds the unwind record for a function of `func_bytes` bytes whose
 * epilogues start at the given byte offsets. */
void unwind_build(const FramePlan *p, u32 func_bytes, const u32 *epilogs, u32 nepilogs,
                  UnwindRecord *out);
void unwind_record_free(UnwindRecord *r);

/* Verifies the record against the encoded function by emulating every
 * prefix of the prologue and every epilogue and unwinding from each
 * instruction boundary the way the Windows unwinder does. Returns false and
 * fills `err` on the first discrepancy. */
bool unwind_verify(const FramePlan *p, const u8 *code, u32 func_bytes, const u32 *epilogs,
                   u32 nepilogs, const UnwindRecord *r, char *err, size_t errsz);

/* Decodes unwind-code bytes into a human-readable listing (for -S and tests). */
void unwind_format_codes(const u8 *codes, u32 len, Buf *out);

#endif
