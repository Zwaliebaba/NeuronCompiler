/* source.h — source manager and the location space (Design/03 §1).
 *
 * A SourceLoc is a 32-bit offset into one global, monotonically assigned
 * location space. Every file inclusion and every macro expansion takes a
 * contiguous range of it, so a single location identifies the file, line and
 * column and the whole include and expansion stack without storing any of it
 * per token. Offset 0 is SOURCE_LOC_NONE. */
#ifndef NC_SOURCE_H
#define NC_SOURCE_H

#include "arena.h"
#include "sym.h"

typedef struct SourceLoc
{
  u32 offset;
} SourceLoc;

#define SOURCE_LOC_NONE ((SourceLoc){0})

typedef struct SourceManager SourceManager; /* opaque: defined in source.c */

typedef enum SourceLoadStatus : u8
{
  SourceLoadStatusOk,
  SourceLoadStatusCannotOpen,
  SourceLoadStatusTooLarge,    /* the location space is exhausted */
  SourceLoadStatusInvalidUtf16 /* UTF-16 BOM, then an odd length or an unpaired surrogate */
} SourceLoadStatus;

/* Where a location is, for a diagnostic or a line record. `line` and
 * `column` are 1-based; the column counts UTF-8 code points from the start
 * of the line, and a tab counts as one (Q-31). */
typedef struct SourcePosition
{
  Sym fileName;
  u32 line;
  u32 column;
  bool isSystem;
  SourceLoc includedFrom; /* SOURCE_LOC_NONE for the main file */
} SourcePosition;

/* File contents live in `_arena`, which must outlive the manager. */
[[nodiscard]] SourceManager* SourceManagerCreate(Arena* _arena);
void SourceManagerDestroy(SourceManager* _manager);

/* Reads `_path` and gives this inclusion of it its own range. A UTF-8 BOM is
 * skipped; a UTF-16 BOM (either byte order) makes the file transcoded to
 * UTF-8 (Q-08). On success `*_start` is the location of the first byte. The
 * caller reports a failure as a diagnostic: the source manager sits below
 * the diagnostics engine and reports nothing itself. */
SourceLoadStatus SourceLoadFile(SourceManager* _manager, Sym _fileName, const char* _path, SourceLoc _includedFrom, bool _isSystem,
                                SourceLoc* _start);

/* As SourceLoadFile, for bytes already in memory (the predefines buffer, a
 * test). The bytes are copied; no BOM handling. */
SourceLoadStatus SourceAddBuffer(SourceManager* _manager, Sym _fileName, const u8* _text, u32 _sizeBytes, SourceLoc _includedFrom,
                                 bool _isSystem, SourceLoc* _start);

/* Gives a macro expansion a range of `_lengthBytes` locations, each mapping
 * to the corresponding byte from `_spelling` onward, and recording the
 * expansion site `_expansionStart`.._expansionEnd. Returns its first
 * location, or SOURCE_LOC_NONE if the location space is exhausted. */
SourceLoc SourceAddExpansion(SourceManager* _manager, SourceLoc _spelling, u32 _lengthBytes, SourceLoc _expansionStart,
                             SourceLoc _expansionEnd);

/* The location where the bytes were written: follows expansions to a file. */
SourceLoc SourceSpellingLoc(SourceManager* _manager, SourceLoc _loc);

/* The location a diagnostic points at: the outermost expansion site. */
SourceLoc SourceExpansionLoc(SourceManager* _manager, SourceLoc _loc);

/* The bytes at a file location, NUL-terminated at the end of the file. */
const u8* SourceText(SourceManager* _manager, SourceLoc _loc);

/* The position of a location, after SourceExpansionLoc. Builds the file's
 * line table the first time it is needed. */
SourcePosition SourceGetPosition(SourceManager* _manager, SourceLoc _loc);

/* Tests only: caps the location space so exhaustion can be tested. */
void SourceManagerSetLimit(SourceManager* _manager, u32 _limit);

static inline SourceLoc SourceLocAdvance(SourceLoc _loc, u32 _bytes)
{
  return (SourceLoc){_loc.offset + _bytes};
}

static inline bool SourceLocIsNone(SourceLoc _loc)
{
  return _loc.offset == 0;
}

#endif
