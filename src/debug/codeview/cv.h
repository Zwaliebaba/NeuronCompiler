/* cv.h — CodeView C13 debug information (Design/10-codeview.md).
 *
 * Phase 0 emits the minimum the linker and debugger need to show functions
 * and step by line: S_OBJNAME, S_COMPILE3, per-function S_GPROC32_ID /
 * S_FRAMEPROC / S_PROC_ID_END, line tables, file checksums and the string
 * table in .debug$S, and LF_ARGLIST / LF_PROCEDURE / LF_FUNC_ID in .debug$T.
 * Types and locals come with the front end. */
#ifndef NC_CV_H
#define NC_CV_H

#include "../../obj/coff/coff.h"

typedef struct CvLine {
    u32 offset; /* code offset within the function */
    u32 line;
} CvLine;

typedef struct CvFunc {
    const char *name;
    bool is_static;
    CoffSec text;
    CoffSym sym;
    u32 length;
    u32 prolog_end;   /* first instruction after the prologue */
    u32 epilog_start; /* first instruction of the first epilogue (or length) */
    u32 frame_size;
    u32 saved_regs_size;
    const CvLine *lines;
    u32 nlines;
} CvFunc;

typedef struct CvModule {
    Arena *arena;
    Buf types;       /* .debug$T records (after the signature) */
    u32 next_type;   /* next type index, starting at 0x1000 */
    Buf strings;     /* DEBUG_S_STRINGTABLE contents */
    Buf checksums;   /* DEBUG_S_FILECHKSMS contents */
    u32 file_id;     /* checksum-table offset of the (single) source file */
    u32 proc_type;   /* LF_PROCEDURE used for every function in phase 0 */
    const char *obj_name;
} CvModule;

/* CodeView constants used by the emitter. */
enum {
    CV_SIGNATURE_C13 = 4,
    DEBUG_S_SYMBOLS = 0xF1,
    DEBUG_S_LINES = 0xF2,
    DEBUG_S_STRINGTABLE = 0xF3,
    DEBUG_S_FILECHKSMS = 0xF4,
    S_OBJNAME = 0x1101,
    S_FRAMEPROC = 0x1012,
    S_COMPILE3 = 0x113C,
    S_LPROC32_ID = 0x1146,
    S_GPROC32_ID = 0x1147,
    S_PROC_ID_END = 0x114F,
    LF_PROCEDURE = 0x1008,
    LF_ARGLIST = 0x1201,
    LF_FUNC_ID = 0x1601,
    CV_CFL_C = 0x00,
    CV_CFL_ARM64 = 0xF6,
    T_INT4 = 0x0074,
};

void cv_init(CvModule *cv, Arena *arena, const char *obj_name, const char *source_name);
void cv_emit_function(CvModule *cv, CoffObj *obj, const CvFunc *f);
void cv_finish(CvModule *cv, CoffObj *obj);
void cv_free(CvModule *cv);

#endif
