#include "test.h"

#include "../../src/core/os.h"
#include "../../src/core/source.h"

#include <stdio.h>

static bool PositionIs(SourceManager* _m, SourceLoc _loc, u32 _line, u32 _column)
{
  SourcePosition p = SourceGetPosition(_m, _loc);
  if (p.line == _line && p.column == _column)
    return true;
  fprintf(stderr, "  location %u is (%u,%u), expected (%u,%u)\n", _loc.offset, p.line, p.column, _line, _column);
  return false;
}

/* Writes `_bytes` to a scratch file and loads it. */
static SourceLoadStatus LoadBytes(SourceManager* _m, SymTable* _syms, const char* _name, const void* _bytes, size_t _sizeBytes,
                                  SourceLoc* _start)
{
  char path[512];
  snprintf(path, sizeof path, "%s/ncc-test-%s", TestTmpdir(), _name);
  if (!OsWriteFileAtomic(path, _bytes, _sizeBytes))
  {
    fprintf(stderr, "  cannot write %s\n", path);
    return SourceLoadStatusCannotOpen;
  }
  SourceLoadStatus s = SourceLoadFile(_m, SymInternCstr(_syms, _name), path, SOURCE_LOC_NONE, false, _start);
  remove(path);
  return s;
}

static void TestPositions(SourceManager* _m, SymTable* _syms)
{
  /* "é" is two bytes and one column; "\r\n" ends a line like "\n". */
  static const char TEXT[] = "a\nbc\n\xC3\xA9x\r\ny";
  SourceLoc f = SOURCE_LOC_NONE;
  CHECK(SourceAddBuffer(_m, SymInternCstr(_syms, "pos.c"), (const u8*)TEXT, sizeof TEXT - 1, SOURCE_LOC_NONE, false, &f) ==
        SourceLoadStatusOk);
  CHECK(!SourceLocIsNone(f));
  CHECK(PositionIs(_m, f, 1, 1));
  CHECK(PositionIs(_m, SourceLocAdvance(f, 1), 1, 2)); /* the newline itself */
  CHECK(PositionIs(_m, SourceLocAdvance(f, 2), 2, 1));
  CHECK(PositionIs(_m, SourceLocAdvance(f, 3), 2, 2));
  CHECK(PositionIs(_m, SourceLocAdvance(f, 5), 3, 1));
  CHECK(PositionIs(_m, SourceLocAdvance(f, 7), 3, 2)); /* 'x', after the two-byte é */
  CHECK(PositionIs(_m, SourceLocAdvance(f, 8), 3, 3)); /* '\r' */
  CHECK(PositionIs(_m, SourceLocAdvance(f, 10), 4, 1));
  CHECK(PositionIs(_m, SourceLocAdvance(f, 11), 4, 2)); /* end of file has a location */
  CHECK(*SourceText(_m, SourceLocAdvance(f, 7)) == 'x');
  CHECK(*SourceText(_m, SourceLocAdvance(f, 11)) == 0);
  SourcePosition p = SourceGetPosition(_m, f);
  CHECK(strcmp(SymText(_syms, p.fileName), "pos.c") == 0 && !p.isSystem && SourceLocIsNone(p.includedFrom));

  /* A second file takes the next range, and remembers who included it. */
  SourceLoc g = SOURCE_LOC_NONE;
  CHECK(SourceAddBuffer(_m, SymInternCstr(_syms, "sys.h"), (const u8*)"z", 1, SourceLocAdvance(f, 2), true, &g) == SourceLoadStatusOk);
  CHECK(g.offset == f.offset + sizeof TEXT); /* (size - 1) bytes + the end position */
  p = SourceGetPosition(_m, g);
  CHECK(p.isSystem && p.includedFrom.offset == f.offset + 2 && p.line == 1 && p.column == 1);
  CHECK(PositionIs(_m, SourceLocAdvance(f, 3), 2, 2)); /* the first file again, after the cache moved */

  /* An empty file still has its end position. */
  SourceLoc e = SOURCE_LOC_NONE;
  CHECK(SourceAddBuffer(_m, SymInternCstr(_syms, "empty.c"), (const u8*)"", 0, SOURCE_LOC_NONE, false, &e) == SourceLoadStatusOk);
  CHECK(PositionIs(_m, e, 1, 1) && *SourceText(_m, e) == 0);
}

static void TestExpansions(SourceManager* _m, SymTable* _syms)
{
  static const char TEXT[] = "#define M foo\n  M\n";
  SourceLoc f = SOURCE_LOC_NONE;
  CHECK(SourceAddBuffer(_m, SymInternCstr(_syms, "exp.c"), (const u8*)TEXT, sizeof TEXT - 1, SOURCE_LOC_NONE, false, &f) ==
        SourceLoadStatusOk);
  SourceLoc body = SourceLocAdvance(f, 10); /* "foo" */
  SourceLoc use = SourceLocAdvance(f, 16);  /* "M" on line 2 */
  SourceLoc x = SourceAddExpansion(_m, body, 3, use, use);
  CHECK(!SourceLocIsNone(x));
  SourceLoc o = SourceLocAdvance(x, 1);
  CHECK(SourceSpellingLoc(_m, o).offset == body.offset + 1);
  CHECK(SourceExpansionLoc(_m, o).offset == use.offset);
  CHECK(*SourceText(_m, o) == 'o');
  CHECK(PositionIs(_m, o, 2, 3)); /* reported where the macro was used */

  /* An expansion inside an expansion: spelling goes through both, and the
   * expansion site is the outermost one. */
  SourceLoc inner = SourceAddExpansion(_m, x, 3, SourceLocAdvance(x, 0), SourceLocAdvance(x, 2));
  CHECK(SourceSpellingLoc(_m, SourceLocAdvance(inner, 2)).offset == body.offset + 2);
  CHECK(SourceExpansionLoc(_m, inner).offset == use.offset);
}

static void TestFiles(SourceManager* _m, SymTable* _syms)
{
  SourceLoc s = SOURCE_LOC_NONE;
  static const u8 BOM8[] = {0xEF, 0xBB, 0xBF, 'a', 'b'};
  CHECK(LoadBytes(_m, _syms, "bom8.c", BOM8, sizeof BOM8, &s) == SourceLoadStatusOk);
  CHECK(memcmp(SourceText(_m, s), "ab", 3) == 0 && PositionIs(_m, SourceLocAdvance(s, 1), 1, 2));

  static const u8 LE[] = {0xFF, 0xFE, 'a', 0, '\n', 0, 0xE9, 0, 0x3D, 0xD8, 0x00, 0xDE}; /* a \n é U+1F600 */
  CHECK(LoadBytes(_m, _syms, "le16.c", LE, sizeof LE, &s) == SourceLoadStatusOk);
  CHECK(memcmp(SourceText(_m, s), "a\n\xC3\xA9\xF0\x9F\x98\x80", 9) == 0);
  CHECK(PositionIs(_m, SourceLocAdvance(s, 4), 2, 2)); /* the emoji, after é */

  static const u8 BE[] = {0xFE, 0xFF, 0, 'h', 0, 'i'};
  CHECK(LoadBytes(_m, _syms, "be16.c", BE, sizeof BE, &s) == SourceLoadStatusOk);
  CHECK(memcmp(SourceText(_m, s), "hi", 3) == 0);

  static const u8 ODD[] = {0xFF, 0xFE, 'a'};
  static const u8 LONE_LOW[] = {0xFF, 0xFE, 0x00, 0xDC};
  static const u8 HIGH_AT_END[] = {0xFF, 0xFE, 'a', 0, 0x3D, 0xD8};
  static const u8 HIGH_THEN_CHAR[] = {0xFF, 0xFE, 0x3D, 0xD8, 'a', 0};
  CHECK(LoadBytes(_m, _syms, "odd.c", ODD, sizeof ODD, &s) == SourceLoadStatusInvalidUtf16);
  CHECK(LoadBytes(_m, _syms, "low.c", LONE_LOW, sizeof LONE_LOW, &s) == SourceLoadStatusInvalidUtf16);
  CHECK(LoadBytes(_m, _syms, "end.c", HIGH_AT_END, sizeof HIGH_AT_END, &s) == SourceLoadStatusInvalidUtf16);
  CHECK(LoadBytes(_m, _syms, "high.c", HIGH_THEN_CHAR, sizeof HIGH_THEN_CHAR, &s) == SourceLoadStatusInvalidUtf16);

  /* No BOM: the bytes are taken as they are, a UTF-16 file included. */
  CHECK(LoadBytes(_m, _syms, "plain.c", "x\0y", 3, &s) == SourceLoadStatusOk && SourceText(_m, s)[1] == 0);

  CHECK(SourceLoadFile(_m, SymInternCstr(_syms, "missing.c"), "/nonexistent/ncc/missing.c", SOURCE_LOC_NONE, false, &s) ==
        SourceLoadStatusCannotOpen);
}

static void TestExhaustion(SymTable* _syms)
{
  Arena arena;
  ArenaInit(&arena);
  SourceManager* m = SourceManagerCreate(&arena);
  SourceManagerSetLimit(m, 1 + 8);
  SourceLoc s = SOURCE_LOC_NONE;
  CHECK(SourceAddBuffer(m, SymInternCstr(_syms, "a.c"), (const u8*)"1234567", 7, SOURCE_LOC_NONE, false, &s) == SourceLoadStatusOk);
  CHECK(SourceAddBuffer(m, SymInternCstr(_syms, "b.c"), (const u8*)"", 0, SOURCE_LOC_NONE, false, &s) == SourceLoadStatusTooLarge);
  CHECK(SourceLocIsNone(SourceAddExpansion(m, SourceLocAdvance(s, 0), 0, s, s)));
  SourceManagerDestroy(m);
  ArenaRelease(&arena);
}

void TestSource(void)
{
  Arena arena;
  ArenaInit(&arena);
  SymTable* syms = SymTableCreate(&arena);
  SourceManager* m = SourceManagerCreate(&arena);
  TestPositions(m, syms);
  TestExpansions(m, syms);
  TestFiles(m, syms);
  TestExhaustion(syms);
  SourceManagerDestroy(m);
  SymTableDestroy(syms);
  ArenaRelease(&arena);
}
