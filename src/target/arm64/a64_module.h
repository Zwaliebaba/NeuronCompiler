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

typedef VEC(MirInst) MirInstVec;

typedef struct MirFunc
{
  const char* name;
  bool isStatic;
  FrameDesc frame;
  MirInstVec insts; /* body, with P_PROLOGUE / P_EPILOGUE / P_LABEL pseudos */
  u32 nlabels;
  u32 curLine; /* line number stamped on instructions added by builders */
} MirFunc;

typedef enum A64DataKind : u8
{
  A64DataKindRw,
  A64DataKindRo,
  A64DataKindBss,
  A64DataKindTls
} A64DataKind;

typedef struct A64DataReloc
{
  u32 offset; /* byte offset within the object */
  u32 sym;    /* module symbol whose address is stored (ADDR64) */
} A64DataReloc;

typedef struct A64Module
{
  Arena* arena;
  CoffObj obj;
  CvModule cv;
  bool debug;
  bool verifyUnwind;
  VEC(const char*) symNames;
  VEC(CoffSym) symCoff;
  u32 chkstkSym;
  Buf listing; /* -S style listing of everything emitted */
  u32 nfuncs, npacked, nxdata;
} A64Module;

void A64ModuleInit(A64Module* _m, Arena* _arena, const char* _objName, const char* _sourceName, bool _debug);
void A64ModuleFree(A64Module* _m);

/* Returns the module symbol id for `name`, creating an undefined external. */
u32 A64ModuleSym(A64Module* _m, const char* _name);

void A64ModuleEmitFunction(A64Module* _m, MirFunc* _f);
void A64ModuleEmitData(A64Module* _m, const char* _name, bool _isStatic, A64DataKind _kind, const void* _init, u32 _size, u32 _align,
                       const A64DataReloc* _relocs, u32 _nrelocs);
void A64ModuleDirective(A64Module* _m, const char* _directive);
void A64ModuleFinish(A64Module* _m, Buf* _out);

/* ---- builder helpers ---------------------------------------------------- */

void MirFuncInit(MirFunc* _f, const char* _name, bool _isStatic, FrameDesc _frame);
void MirFuncFree(MirFunc* _f);
u32 MirFuncNewLabel(MirFunc* _f);
void MirFuncEmit(MirFunc* _f, MirInst _mi);
void MirFuncLabel(MirFunc* _f, u32 _label);
void MirFuncLine(MirFunc* _f, u32 _line);
/* Materialises a 64-bit (or 32-bit, if `w`) constant with the shortest
 * movz/movn/movk or orr-immediate sequence. */
void MirFuncMovImm(MirFunc* _f, u32 _rd, u64 _value, bool _w);

#endif
