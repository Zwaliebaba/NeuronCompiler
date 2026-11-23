/* coff.h — COFF relocatable object writer for ARM64
 * (Design/08-coff-object-writer.md). */
#ifndef NC_COFF_H
#define NC_COFF_H

#include "../../core/arena.h"
#include "../../core/buf.h"
#include "../../core/vec.h"

// clang-format off: PE/COFF specification values, aligned by hand (AGENTS.md §4)
enum {
    IMAGE_FILE_MACHINE_ARM64 = 0xAA64,

    /* Section characteristics */
    IMAGE_SCN_CNT_CODE               = 0x00000020,
    IMAGE_SCN_CNT_INITIALIZED_DATA   = 0x00000040,
    IMAGE_SCN_CNT_UNINITIALIZED_DATA = 0x00000080,
    IMAGE_SCN_LNK_INFO               = 0x00000200,
    IMAGE_SCN_LNK_REMOVE             = 0x00000800,
    IMAGE_SCN_LNK_COMDAT             = 0x00001000,
    IMAGE_SCN_LNK_NRELOC_OVFL        = 0x01000000,
    IMAGE_SCN_MEM_DISCARDABLE        = 0x02000000,
    IMAGE_SCN_MEM_EXECUTE            = 0x20000000,
    IMAGE_SCN_MEM_READ               = 0x40000000,
    IMAGE_SCN_MEM_WRITE              = 0x80000000,

    /* Symbol storage classes */
    IMAGE_SYM_CLASS_EXTERNAL = 2,
    IMAGE_SYM_CLASS_STATIC   = 3,
    IMAGE_SYM_CLASS_LABEL    = 6,
    IMAGE_SYM_CLASS_FILE     = 103,
    IMAGE_SYM_DTYPE_FUNCTION = 0x20,

    /* COMDAT selection */
    IMAGE_COMDAT_SELECT_NODUPLICATES = 1,
    IMAGE_COMDAT_SELECT_ANY          = 2,
    IMAGE_COMDAT_SELECT_SAME_SIZE    = 3,
    IMAGE_COMDAT_SELECT_EXACT_MATCH  = 4,
    IMAGE_COMDAT_SELECT_ASSOCIATIVE  = 5,
    IMAGE_COMDAT_SELECT_LARGEST      = 6,

    /* ARM64 relocation types */
    IMAGE_REL_ARM64_ABSOLUTE       = 0x00,
    IMAGE_REL_ARM64_ADDR32         = 0x01,
    IMAGE_REL_ARM64_ADDR32NB       = 0x02,
    IMAGE_REL_ARM64_BRANCH26       = 0x03,
    IMAGE_REL_ARM64_PAGEBASE_REL21 = 0x04,
    IMAGE_REL_ARM64_REL21          = 0x05,
    IMAGE_REL_ARM64_PAGEOFFSET_12A = 0x06,
    IMAGE_REL_ARM64_PAGEOFFSET_12L = 0x07,
    IMAGE_REL_ARM64_SECREL         = 0x08,
    IMAGE_REL_ARM64_SECREL_LOW12A  = 0x09,
    IMAGE_REL_ARM64_SECREL_HIGH12A = 0x0A,
    IMAGE_REL_ARM64_SECREL_LOW12L  = 0x0B,
    IMAGE_REL_ARM64_TOKEN          = 0x0C,
    IMAGE_REL_ARM64_SECTION        = 0x0D,
    IMAGE_REL_ARM64_ADDR64         = 0x0E,
    IMAGE_REL_ARM64_BRANCH19       = 0x0F,
    IMAGE_REL_ARM64_BRANCH14       = 0x10,
    IMAGE_REL_ARM64_REL32          = 0x11,
};
// clang-format on

/* Above this many sections the writer switches to the bigobj format. */
enum
{
  COFF_MAX_REGULAR_SECTIONS = 65279
};

/* Section alignment as characteristics bits (align must be a power of 2, <= 8192). */
u32 CoffAlignFlags(u32 _align);

typedef u32 CoffSec; /* 1-based section number */
typedef u32 CoffSym; /* writer-internal symbol handle (not the file index) */

typedef struct CoffReloc
{
  u32 offset;
  CoffSym sym;
  u16 type;
} CoffReloc;

typedef struct CoffSection
{
  const char* name;
  u32 characteristics;
  Buf data;
  u32 bssSize; /* for uninitialized-data sections */
  VEC(CoffReloc) relocs;
  u8 comdatSel;   /* 0 = not COMDAT */
  CoffSec assoc;  /* for IMAGE_COMDAT_SELECT_ASSOCIATIVE */
  CoffSym secSym; /* the section's own symbol */
} CoffSection;

typedef struct CoffSymbol
{
  const char* name;
  i32 section; /* 0 undefined, -1 absolute, else CoffSec */
  u32 value;
  u16 type;
  u8 storageClass;
  bool isSectionSym;
} CoffSymbol;

typedef struct CoffObj
{
  Arena* arena;
  VEC(CoffSection) sections;
  VEC(CoffSymbol) symbols;
  Buf directives;
  bool forceBigobj;
} CoffObj;

void CoffInit(CoffObj* _o, Arena* _arena);
void CoffFree(CoffObj* _o);

/* Adds a section. `align` is in bytes. A section symbol is created for it. */
CoffSec CoffAddSection(CoffObj* _o, const char* _name, u32 _characteristics, u32 _align);
CoffSection* CoffSectionGet(CoffObj* _o, CoffSec _s);
void CoffSetComdat(CoffObj* _o, CoffSec _s, u8 _selection, CoffSec _assoc);

/* Symbols. CoffSymbolNamed returns the existing symbol with that name or
 * creates an undefined external one; CoffDefine then gives it a location. */
CoffSym CoffSymbolNamed(CoffObj* _o, const char* _name);
CoffSym CoffAddSymbol(CoffObj* _o, const char* _name, i32 _section, u32 _value, u16 _type, u8 _storageClass);
void CoffDefine(CoffObj* _o, CoffSym _sym, CoffSec _sec, u32 _value, u16 _type, u8 _storageClass);
CoffSym CoffSectionSymbol(CoffObj* _o, CoffSec _s);

void CoffAddReloc(CoffObj* _o, CoffSec _s, u32 _offset, CoffSym _sym, u16 _type);
void CoffAddDirective(CoffObj* _o, const char* _directive);

/* Serialises the object (symbol order: section symbols, then all other
 * symbols in creation order) into `out`. */
void CoffSerialize(CoffObj* _o, Buf* _out);

#endif
