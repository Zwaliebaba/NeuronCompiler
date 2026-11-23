/* diag.h — diagnostics: stable ids, MSVC-format rendering, warning controls
 * (Design/02 §3, Design/03 §5.8, Design/12 §6, R18).
 *
 * A diagnostic is something the user got wrong. Something the compiler got
 * wrong is NC_ASSERT, never a diagnostic. */
#ifndef NC_DIAG_H
#define NC_DIAG_H

#include "source.h"

typedef enum DiagId : u16
{
#define DIAG(name, number, severity, level, message) DiagId##name,
#include "diag.def"
#undef DIAG
  DiagIdCount
} DiagId;

typedef enum DiagSeverity : u8
{
  DiagSeverityNote,
  DiagSeverityWarning,
  DiagSeverityError,
  DiagSeverityFatal /* compilation of the translation unit stops */
} DiagSeverity;

typedef enum DiagArgKind : u8
{
  DiagArgKindText,
  DiagArgKindInt,
  DiagArgKindUint,
  DiagArgKindSym
} DiagArgKind;

/* An argument for a %N in a message. Typed rather than printf-style, so a
 * message and its arguments cannot disagree about a format (Q-31). */
typedef struct DiagArg
{
  DiagArgKind kind;
  union
  {
    const char* text;
    i64 intValue;
    u64 uintValue;
    Sym sym;
  };
} DiagArg;

static inline DiagArg DiagArgText(const char* _text)
{
  return (DiagArg){.kind = DiagArgKindText, .text = _text};
}
static inline DiagArg DiagArgInt(i64 _value)
{
  return (DiagArg){.kind = DiagArgKindInt, .intValue = _value};
}
static inline DiagArg DiagArgUint(u64 _value)
{
  return (DiagArg){.kind = DiagArgKindUint, .uintValue = _value};
}
static inline DiagArg DiagArgSym(Sym _sym)
{
  return (DiagArg){.kind = DiagArgKindSym, .sym = _sym};
}

/* Receives each rendered diagnostic, one line without its newline. */
typedef void (*DiagSinkFn)(void* _context, const char* _line);

typedef struct DiagEngine DiagEngine; /* opaque: defined in diag.c */

/* Renders to stderr until DiagSetSink says otherwise. Warning level 1, as
 * for cl. */
[[nodiscard]] DiagEngine* DiagEngineCreate(Arena* _arena, SourceManager* _sources, const SymTable* _syms);
void DiagEngineDestroy(DiagEngine* _engine);
void DiagSetSink(DiagEngine* _engine, DiagSinkFn _sink, void* _context);

/* Reports a diagnostic at `_loc`, which may be SOURCE_LOC_NONE for one with
 * no position (rendered as "ncc: ..."). Every %N in the message must be
 * below `_argCount`; one that is not is an internal error. */
void DiagReport(DiagEngine* _engine, SourceLoc _loc, DiagId _id, const DiagArg* _args, u32 _argCount);

/* The controls behind /W0-/W4, /WX, /wd, /we and /w1-/w4. Each per-warning
 * control takes a warning's NC number and returns false, changing nothing,
 * if no warning has that number. */
void DiagSetWarningLevel(DiagEngine* _engine, u32 _level);
void DiagSetWarningsAsErrors(DiagEngine* _engine, bool _on);
bool DiagDisableWarning(DiagEngine* _engine, u32 _number);
bool DiagWarningAsError(DiagEngine* _engine, u32 _number);
bool DiagSetLevelOf(DiagEngine* _engine, u32 _number, u32 _level);

/* Counts of what was reported, after the controls: a warning turned into an
 * error counts as an error, a suppressed one not at all. Notes are not
 * counted. */
u32 DiagErrorCount(const DiagEngine* _engine);
u32 DiagWarningCount(const DiagEngine* _engine);
bool DiagHadFatal(const DiagEngine* _engine);

/* The table, for the driver, #pragma warning and tests. */
u32 DiagNumber(DiagId _id);
DiagSeverity DiagSeverityOf(DiagId _id);
bool DiagFindNumber(u32 _number, DiagId* _id);

#endif
