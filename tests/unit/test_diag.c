#include "test.h"

#include "../../src/core/diag.h"

#include <stdio.h>

typedef struct Capture
{
  char last[512];
  u32 lines;
} Capture;

static void CaptureSink(void* _context, const char* _line)
{
  Capture* c = _context;
  snprintf(c->last, sizeof c->last, "%s", _line);
  c->lines++;
}

/* Reports and returns what was rendered, or "" if nothing was. */
static const char* Render(DiagEngine* _e, Capture* _c, SourceLoc _loc, DiagId _id, const DiagArg* _args, u32 _argCount)
{
  u32 before = _c->lines;
  DiagReport(_e, _loc, _id, _args, _argCount);
  return _c->lines == before ? "" : _c->last;
}

#define EXPECT_RENDER(expected, loc, id, ...)                                                \
  do                                                                                         \
  {                                                                                          \
    const DiagArg args_[] = {DiagArgInt(0), __VA_ARGS__};                                    \
    const char* got_ = Render(e, &capture, (loc), (id), args_ + 1, NC_ARRAY_LEN(args_) - 1); \
    CHECKF(strcmp(got_, (expected)) == 0, "got \"%s\"", got_);                               \
  } while (0)

static void TestTable(void)
{
  for (u32 i = 0; i < DiagIdCount; i++)
  {
    u32 n = DiagNumber((DiagId)i);
    DiagSeverity s = DiagSeverityOf((DiagId)i);
    switch (s)
    {
    case DiagSeverityNote:
      CHECKF(n == 0, "note %u has number %u", i, n);
      break;
    case DiagSeverityFatal:
      CHECKF(n >= 1000 && n < 2000, "fatal NC%04u outside 1xxx", n);
      break;
    case DiagSeverityError:
      CHECKF(n >= 2000 && n < 4000, "error NC%04u outside 2xxx-3xxx", n);
      break;
    case DiagSeverityWarning:
      CHECKF(n >= 4000 && n < 5000, "warning NC%04u outside 4xxx", n);
      break;
    }
    DiagId found;
    if (n)
      CHECKF(DiagFindNumber(n, &found) && found == (DiagId)i, "NC%04u is not unique", n);
  }
  DiagId unused;
  CHECK(!DiagFindNumber(0, &unused) && !DiagFindNumber(9999, &unused));
}

void TestDiag(void)
{
  TestTable();

  Arena arena;
  ArenaInit(&arena);
  SymTable* syms = SymTableCreate(&arena);
  SourceManager* sources = SourceManagerCreate(&arena);
  DiagEngine* e = DiagEngineCreate(&arena, sources, syms);
  Capture capture = {};
  DiagSetSink(e, CaptureSink, &capture);

  SourceLoc f = SOURCE_LOC_NONE, sys = SOURCE_LOC_NONE;
  CHECK(SourceAddBuffer(sources, SymInternCstr(syms, "x.c"), (const u8*)"int\nab", 6, SOURCE_LOC_NONE, false, &f) == SourceLoadStatusOk);
  CHECK(SourceAddBuffer(sources, SymInternCstr(syms, "sys.h"), (const u8*)"q", 1, f, true, &sys) == SourceLoadStatusOk);
  SourceLoc b = SourceLocAdvance(f, 5); /* 'b', line 2 column 2 */

  /* Each severity, with and without a position. */
  EXPECT_RENDER("ncc: fatal error NC1001: cannot open source file 'a.c'", SOURCE_LOC_NONE, DiagIdCannotOpenSource, DiagArgText("a.c"));
  CHECK(DiagHadFatal(e) && DiagErrorCount(e) == 1);
  EXPECT_RENDER("x.c(2,2): warning NC4001: null character ignored", b, DiagIdNullCharacterIgnored, );
  CHECK(DiagWarningCount(e) == 1);
  EXPECT_RENDER("x.c(1,1): note: in file included from here", f, DiagIdNoteIncludedFrom, );
  CHECK(DiagWarningCount(e) == 1 && DiagErrorCount(e) == 1); /* notes are not counted */
  EXPECT_RENDER("x.c(1,4): fatal error NC1003: 'le16.c' starts with a UTF-16 byte-order mark but is not valid UTF-16",
                SourceLocAdvance(f, 3), DiagIdInvalidUtf16Source, DiagArgSym(SymInternCstr(syms, "le16.c")));

  /* Warning levels: NC4002 is a level-4 warning. */
  EXPECT_RENDER("", b, DiagIdNoNewlineAtEof, );
  DiagSetWarningLevel(e, 4);
  EXPECT_RENDER("x.c(2,2): warning NC4002: no newline at end of file", b, DiagIdNoNewlineAtEof, );
  DiagSetWarningLevel(e, 0);
  EXPECT_RENDER("", b, DiagIdNullCharacterIgnored, );
  DiagSetWarningLevel(e, 1);
  CHECK(DiagSetLevelOf(e, 4002, 1)); /* /w14002 */
  EXPECT_RENDER("x.c(2,2): warning NC4002: no newline at end of file", b, DiagIdNoNewlineAtEof, );

  /* /wd, /we, /WX. */
  u32 warnings = DiagWarningCount(e), errors = DiagErrorCount(e);
  CHECK(DiagDisableWarning(e, 4001));
  EXPECT_RENDER("", b, DiagIdNullCharacterIgnored, );
  CHECK(DiagWarningAsError(e, 4001));
  EXPECT_RENDER("x.c(2,2): error NC4001: null character ignored", b, DiagIdNullCharacterIgnored, );
  CHECK(DiagErrorCount(e) == errors + 1 && DiagWarningCount(e) == warnings);
  DiagSetWarningsAsErrors(e, true);
  EXPECT_RENDER("x.c(2,2): error NC4002: no newline at end of file", b, DiagIdNoNewlineAtEof, );
  DiagSetWarningsAsErrors(e, false);

  /* The per-warning controls reject numbers that are not warnings. */
  CHECK(!DiagDisableWarning(e, 1001) && !DiagWarningAsError(e, 9999) && !DiagSetLevelOf(e, 4001, 5) && !DiagSetLevelOf(e, 0, 1));

  /* Warnings in system headers are dropped; errors are not. */
  EXPECT_RENDER("", sys, DiagIdNoNewlineAtEof, );
  EXPECT_RENDER("sys.h(1,1): fatal error NC1002: 'big.c' does not fit in the location space", sys, DiagIdSourceTooLarge,
                DiagArgText("big.c"));

  DiagEngineDestroy(e);
  SourceManagerDestroy(sources);
  SymTableDestroy(syms);
  ArenaRelease(&arena);
}
