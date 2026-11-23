/* a64_module.h — turns machine functions and data into a COFF object:
 * pseudo expansion, branch relaxation, encoding, relocations, unwind data
 * (verified), CodeView, and an assembly listing. */
#ifndef NC_A64_MODULE_H
#define NC_A64_MODULE_H

#include "../../core/vec.h"
#include "../../debug/codeview/cv.h"
#include "../../obj/coff/coff.h"
#include "a64.h"
#include "unwind.h"

typedef VEC(MInst) MInstVec;

typedef struct MFunc {
    const char *name;
    bool is_static;
    FrameDesc frame;
    MInstVec insts; /* body, with P_PROLOGUE / P_EPILOGUE / P_LABEL pseudos */
    u32 nlabels;
    u32 cur_line;   /* line number stamped on instructions added by builders */
} MFunc;

typedef enum A64DataKind : u8 { DATA_RW, DATA_RO, DATA_BSS, DATA_TLS } A64DataKind;

typedef struct A64DataReloc {
    u32 offset; /* byte offset within the object */
    u32 sym;    /* module symbol whose address is stored (ADDR64) */
} A64DataReloc;

typedef struct A64Module {
    Arena *arena;
    CoffObj obj;
    CvModule cv;
    bool debug;
    bool verify_unwind;
    VEC(const char *) sym_names;
    VEC(CoffSym) sym_coff;
    u32 chkstk_sym;
    Buf listing; /* -S style listing of everything emitted */
    u32 nfuncs, npacked, nxdata;
} A64Module;

void a64m_init(A64Module *m, Arena *arena, const char *obj_name, const char *source_name,
               bool debug);
void a64m_free(A64Module *m);

/* Returns the module symbol id for `name`, creating an undefined external. */
u32 a64m_sym(A64Module *m, const char *name);

void a64m_emit_function(A64Module *m, MFunc *f);
void a64m_emit_data(A64Module *m, const char *name, bool is_static, A64DataKind kind,
                    const void *init, u32 size, u32 align, const A64DataReloc *relocs,
                    u32 nrelocs);
void a64m_directive(A64Module *m, const char *directive);
void a64m_finish(A64Module *m, Buf *out);

/* ---- builder helpers ---------------------------------------------------- */

void mf_init(MFunc *f, const char *name, bool is_static, FrameDesc frame);
void mf_free(MFunc *f);
u32  mf_new_label(MFunc *f);
void mf_emit(MFunc *f, MInst mi);
void mf_label(MFunc *f, u32 label);
void mf_line(MFunc *f, u32 line);
/* Materialises a 64-bit (or 32-bit, if `w`) constant with the shortest
 * movz/movn/movk or orr-immediate sequence. */
void mf_mov_imm(MFunc *f, u32 rd, u64 value, bool w);

#endif
