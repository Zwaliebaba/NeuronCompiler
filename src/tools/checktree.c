/* checktree.c — the repository checker behind `make check-tree` (AGENTS.md §6).
 *
 *   ncc-check-tree FILE...
 *
 * Run from the repository root with every file under src/ and tests/ as
 * arguments. It checks what neither clang-format nor clang-tidy can see:
 *
 *   R2   banned type affixes: CFoo, SFoo, EFoo, IFoo, FooBase, FooImpl, foo_t
 *   R7   lowercase file and directory names, and the allowed extensions
 *   R11  one spelling per family in identifiers (initializer, not initialiser)
 *   §1   enumerator shape: a named enum's enumerators are PascalCase and led by
 *        the type's name; an anonymous enum's are UPPER_CASE constants
 *   §2   the registries: every source directory is in the Makefile and in
 *        build.cmd, every tool is built by both, and every test entry point is
 *        declared in tests/unit/test.h and listed in tests/unit/test_main.c
 *
 * Every finding is printed as `file(line): check-tree: message` and the exit
 * status is the number of findings, capped at 255. */
#include "../core/os.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

static u32 g_findings; /* the run's result; a tool's state, hence global */

static void Report(const char* _file, u32 _line, const char* _fmt, const char* _arg)
{
  printf("%s(%u): check-tree: ", _file, _line);
  printf(_fmt, _arg);
  printf("\n");
  g_findings++;
}

static void ReportPlain(const char* _file, u32 _line, const char* _msg)
{
  Report(_file, _line, "%s", _msg);
}

/* ---- source text without comments, strings and #include lines ---------- */

/* Copies `_src` with comments, string and character literals and #include
 * lines blanked out, newlines kept, so that line numbers still match. */
static char* StripCode(const char* _src, size_t _size)
{
  char* out = NcXrealloc(nullptr, _size + 1);
  size_t i = 0;
  bool lineStart = true;
  while (i < _size)
  {
    char c = _src[i];
    if (lineStart)
    {
      size_t j = i;
      while (j < _size && (_src[j] == ' ' || _src[j] == '\t'))
        j++;
      if (j < _size && _src[j] == '#')
      {
        size_t k = j + 1;
        while (k < _size && (_src[k] == ' ' || _src[k] == '\t'))
          k++;
        if (_size - k >= 7 && strncmp(_src + k, "include", 7) == 0)
        {
          while (i < _size && _src[i] != '\n')
            out[i++] = ' ';
          continue;
        }
      }
    }
    lineStart = c == '\n';
    if (c == '/' && i + 1 < _size && _src[i + 1] == '/')
    {
      while (i < _size && _src[i] != '\n')
        out[i++] = ' ';
    }
    else if (c == '/' && i + 1 < _size && _src[i + 1] == '*')
    {
      out[i++] = ' ';
      out[i++] = ' ';
      while (i < _size && !(_src[i] == '*' && i + 1 < _size && _src[i + 1] == '/'))
      {
        out[i] = _src[i] == '\n' ? '\n' : ' ';
        i++;
      }
      if (i < _size)
      {
        out[i++] = ' ';
        out[i++] = ' ';
      }
    }
    else if (c == '"' || c == '\'')
    {
      out[i++] = ' ';
      while (i < _size && _src[i] != c && _src[i] != '\n')
      {
        if (_src[i] == '\\' && i + 1 < _size)
          out[i++] = ' ';
        out[i++] = ' ';
      }
      if (i < _size && _src[i] == c)
        out[i++] = ' ';
    }
    else
    {
      out[i] = c;
      i++;
    }
  }
  out[_size] = 0;
  return out;
}

static bool IsIdentStart(char _c)
{
  return isalpha((unsigned char)_c) || _c == '_';
}

static bool IsIdentChar(char _c)
{
  return isalnum((unsigned char)_c) || _c == '_';
}

static u32 LineAt(const char* _text, size_t _pos)
{
  u32 line = 1;
  for (size_t i = 0; i < _pos; i++)
    line += _text[i] == '\n';
  return line;
}

/* Reads the identifier at `*_pos` into `_buf`, advancing past it. */
static bool NextIdent(const char* _text, size_t* _pos, char* _buf, size_t _bufBytes)
{
  size_t i = *_pos;
  while (_text[i] && !IsIdentStart(_text[i]))
  {
    if (isdigit((unsigned char)_text[i]))
      while (IsIdentChar(_text[i]))
        i++; /* a number, with any suffix */
    else
      i++;
  }
  if (!_text[i])
    return false;
  size_t n = 0;
  while (IsIdentChar(_text[i]))
  {
    if (n + 1 < _bufBytes)
      _buf[n++] = _text[i];
    i++;
  }
  _buf[n] = 0;
  *_pos = i;
  return true;
}

static size_t SkipSpace(const char* _text, size_t _pos)
{
  while (_text[_pos] && isspace((unsigned char)_text[_pos]))
    _pos++;
  return _pos;
}

/* ---- R11, R2 and enumerator shape on one C file ------------------------- */

static const char* const BANNED_SPELLINGS[][2] = {
  {"initialis", "initializ"}, {"serialis", "serializ"}, {"normalis", "normaliz"}, {"optimis", "optimiz"},
  {"behaviour", "behavior"},  {"colour", "color"},      {"centre", "center"},     {"cancelled", "canceled"},
};

static void CheckSpelling(const char* _file, const char* _code)
{
  size_t pos = 0;
  char ident[256];
  while (NextIdent(_code, &pos, ident, sizeof ident))
  {
    char lower[256] = {};
    for (size_t i = 0; ident[i] && i + 1 < sizeof lower; i++)
      lower[i] = (char)tolower((unsigned char)ident[i]);
    for (size_t b = 0; b < NC_ARRAY_LEN(BANNED_SPELLINGS); b++)
      if (strstr(lower, BANNED_SPELLINGS[b][0]))
      {
        char msg[300];
        snprintf(msg, sizeof msg, "'%s' (R11: spell it '%s')", ident, BANNED_SPELLINGS[b][1]);
        Report(_file, LineAt(_code, pos), "identifier %s", msg);
      }
  }
}

/* core.h's u8…i64: the one sanctioned exception to the type-name rules. */
static bool IsFixedWidthShorthand(const char* _name)
{
  static const char* const SHORTHANDS[] = {"u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64"};
  for (size_t i = 0; i < NC_ARRAY_LEN(SHORTHANDS); i++)
    if (strcmp(_name, SHORTHANDS[i]) == 0)
      return true;
  return false;
}

static void CheckTypeName(const char* _file, u32 _line, const char* _name)
{
  /* `typedef struct Foo {...} Foo;` names the type twice on purpose: report it once per file */
  static char g_lastFile[512];
  static char g_lastName[256];
  if (strcmp(_file, g_lastFile) == 0 && strcmp(_name, g_lastName) == 0)
    return;
  snprintf(g_lastFile, sizeof g_lastFile, "%s", _file);
  snprintf(g_lastName, sizeof g_lastName, "%s", _name);
  size_t n = strlen(_name);
  bool prefixed = n >= 3 && strchr("CSEI", _name[0]) && isupper((unsigned char)_name[1]) && islower((unsigned char)_name[2]);
  bool suffixed = (n > 4 && strcmp(_name + n - 4, "Base") == 0) || (n > 4 && strcmp(_name + n - 4, "Impl") == 0) ||
                  (n > 2 && strcmp(_name + n - 2, "_t") == 0);
  if (prefixed || suffixed)
    Report(_file, _line, "type name '%s' carries a banned affix (R2)", _name);
}

static bool IsPascalLedBy(const char* _name, const char* _type)
{
  size_t t = strlen(_type);
  if (strncmp(_name, _type, t) != 0 || !isupper((unsigned char)_name[0]))
    return false;
  for (const char* p = _name; *p; p++)
    if (!isalnum((unsigned char)*p))
      return false;
  return true;
}

static bool IsUpperConstant(const char* _name)
{
  if (!isupper((unsigned char)_name[0]))
    return false;
  for (const char* p = _name; *p; p++)
    if (!(isupper((unsigned char)*p) || isdigit((unsigned char)*p) || *p == '_'))
      return false;
  return true;
}

/* Checks the enumerators of the enum body starting at `_open` ('{'). */
static void CheckEnumBody(const char* _file, const char* _code, size_t _open, const char* _type)
{
  size_t close = _open + 1;
  while (_code[close] && _code[close] != '}')
  {
    if (_code[close] == '#')
      return; /* an X-macro expansion: the generated names come from a .def */
    close++;
  }
  size_t pos = _open + 1;
  bool expectName = true;
  int depth = 0;
  while (pos < close)
  {
    pos = SkipSpace(_code, pos);
    if (pos >= close)
      break;
    char c = _code[pos];
    if (c == '(')
      depth++;
    else if (c == ')')
      depth--;
    if (depth == 0 && c == ',')
    {
      expectName = true;
      pos++;
      continue;
    }
    if (expectName && IsIdentStart(c))
    {
      char name[256];
      size_t start = pos;
      NextIdent(_code, &pos, name, sizeof name);
      u32 line = LineAt(_code, start);
      if (_type[0] && !IsPascalLedBy(name, _type))
        Report(_file, line, "enumerator '%s' of a named enum must be PascalCase and led by its type (AGENTS.md §1)", name);
      else if (!_type[0] && !IsUpperConstant(name))
        Report(_file, line, "enumerator '%s' of an anonymous enum is a constant and must be UPPER_CASE (R3)", name);
      expectName = false;
      continue;
    }
    pos++;
  }
}

static void CheckDeclarations(const char* _file, const char* _code)
{
  size_t pos = 0;
  char ident[256];
  while (NextIdent(_code, &pos, ident, sizeof ident))
  {
    bool isEnum = strcmp(ident, "enum") == 0;
    if (!isEnum && strcmp(ident, "struct") != 0 && strcmp(ident, "union") != 0)
    {
      if (strcmp(ident, "typedef") == 0)
      {
        /* typedef ... NAME; : the last identifier before the ';' at depth 0 */
        size_t p = pos;
        int depth = 0;
        size_t lastStart = 0;
        char last[256] = "";
        while (_code[p] && !(depth == 0 && _code[p] == ';'))
        {
          if (_code[p] == '{' || _code[p] == '(')
            depth++;
          else if (_code[p] == '}' || _code[p] == ')')
            depth--;
          if (depth == 0 && IsIdentStart(_code[p]) && (p == 0 || !IsIdentChar(_code[p - 1])))
          {
            lastStart = p;
            size_t q = p;
            NextIdent(_code, &q, last, sizeof last);
            p = q;
            continue;
          }
          p++;
        }
        if (last[0] && !IsFixedWidthShorthand(last))
          CheckTypeName(_file, LineAt(_code, lastStart), last);
      }
      continue;
    }
    size_t after = SkipSpace(_code, pos);
    char tag[256] = "";
    if (IsIdentStart(_code[after]))
    {
      size_t q = after;
      NextIdent(_code, &q, tag, sizeof tag);
      CheckTypeName(_file, LineAt(_code, after), tag);
      after = SkipSpace(_code, q);
    }
    if (!isEnum)
      continue;
    if (_code[after] == ':')
    {
      /* fixed underlying type */
      after++;
      while (_code[after] && _code[after] != '{' && _code[after] != ';')
        after++;
    }
    if (_code[after] == '{')
      CheckEnumBody(_file, _code, after, tag);
  }
}

/* ---- R7: names and extensions -------------------------------------------- */

static bool HasSuffix(const char* _s, const char* _suffix)
{
  size_t n = strlen(_s), m = strlen(_suffix);
  return n >= m && strcmp(_s + n - m, _suffix) == 0;
}

static void CheckPath(const char* _path)
{
  /* every component lowercase: [a-z0-9_] for directories, plus '.' and '-' in file names */
  const char* comp = _path;
  for (const char* p = _path;; p++)
  {
    if (*p == '/' || *p == 0)
    {
      size_t n = (size_t)(p - comp);
      bool isFile = *p == 0;
      for (size_t i = 0; i < n; i++)
      {
        char c = comp[i];
        bool ok = islower((unsigned char)c) || isdigit((unsigned char)c) || c == '_' || (isFile && (c == '.' || c == '-'));
        if (!ok)
        {
          ReportPlain(_path, 1, "file and directory names are lowercase snake_case (R7)");
          return;
        }
      }
      if (!isFile && n == 5 && strncmp(comp, "build", 5) == 0)
        ReportPlain(_path, 1, "no source directory may be named build (AGENTS.md §2)");
      if (isFile)
        break;
      comp = p + 1;
    }
  }
  bool inSrc = strncmp(_path, "src/", 4) == 0;
  bool allowed = HasSuffix(_path, ".c") || HasSuffix(_path, ".h") || HasSuffix(_path, ".def") || HasSuffix(_path, ".inc") ||
                 (!inSrc && (HasSuffix(_path, ".sh") || HasSuffix(_path, ".cmd")));
  if (!allowed)
    ReportPlain(_path, 1, "not an allowed source extension (R7: .c .h .def .inc; tests may also hold .sh and .cmd)");
}

/* ---- §2: the registries -------------------------------------------------- */

static char* ReadText(const char* _path)
{
  u8* data;
  size_t size;
  if (!OsReadFile(_path, &data, &size))
    return nullptr;
  return (char*)data;
}

static void CheckBuildRegistry(const char* const* _paths, int _count)
{
  char* makefile = ReadText("Makefile");
  char* buildCmd = ReadText("build.cmd");
  if (!makefile || !buildCmd)
  {
    ReportPlain("Makefile", 1, "Makefile and build.cmd must both be readable from the repository root");
    NcXfree(makefile);
    NcXfree(buildCmd);
    return;
  }
  for (int i = 0; i < _count; i++)
  {
    const char* path = _paths[i];
    if (!HasSuffix(path, ".c") || strncmp(path, "tests/link/", 11) == 0)
      continue; /* tests/link holds fixtures compiled by the link test, not by make or build.cmd */
    const char* slash = strrchr(path, '/');
    size_t dirLen = (size_t)(slash - path);
    char dir[512], winDir[512], needle[600];
    snprintf(dir, sizeof dir, "%.*s", (int)dirLen, path);
    snprintf(winDir, sizeof winDir, "%s", dir);
    for (char* p = winDir; *p; p++)
      if (*p == '/')
        *p = '\\';
    if (strcmp(dir, "src/tools") == 0)
    {
      /* each tool is built explicitly by both files */
      char base[256];
      snprintf(base, sizeof base, "%.*s", (int)(strlen(slash + 1) - 2), slash + 1);
      snprintf(needle, sizeof needle, "src/tools/%s.o", base);
      if (!strstr(makefile, needle))
        Report(path, 1, "tool is not built by the Makefile (%s missing)", needle);
      snprintf(needle, sizeof needle, "src\\tools\\%s.c", base);
      if (!strstr(buildCmd, needle))
        Report(path, 1, "tool is not built by build.cmd (%s missing)", needle);
      continue;
    }
    snprintf(needle, sizeof needle, "$(wildcard %s/*.c)", dir);
    if (!strstr(makefile, needle))
      Report(path, 1, "source directory is missing from the Makefile (%s)", needle);
    snprintf(needle, sizeof needle, "%s\\*.c", winDir);
    if (!strstr(buildCmd, needle))
      Report(path, 1, "source directory is missing from build.cmd (%s)", needle);
  }
  NcXfree(makefile);
  NcXfree(buildCmd);
}

/* Collects the names of `void TestXxx(void)` declarations or definitions. */
static u32 CollectTestEntries(const char* _code, char _names[][64], u32 _max)
{
  u32 n = 0;
  for (const char* p = _code; (p = strstr(p, "void Test")) != nullptr; p++)
  {
    const char* name = p + 5;
    size_t len = 0;
    while (IsIdentChar(name[len]))
      len++;
    const char* rest = name + len;
    while (*rest == ' ')
      rest++;
    /* an entry point has external linkage; a static TestXxx is a helper */
    bool isStatic = p - _code >= 7 && strncmp(p - 7, "static ", 7) == 0;
    if (isStatic || strncmp(rest, "(void)", 6) != 0 || n >= _max || len >= 64)
      continue;
    snprintf(_names[n++], 64, "%.*s", (int)len, name);
  }
  return n;
}

static void CheckTestRegistry(const char* const* _paths, int _count)
{
  char* header = ReadText("tests/unit/test.h");
  char* mainSrc = ReadText("tests/unit/test_main.c");
  if (!header || !mainSrc)
  {
    NcXfree(header);
    NcXfree(mainSrc);
    return;
  }
  static char g_declared[128][64];
  char* headerCode = StripCode(header, strlen(header));
  u32 nDeclared = CollectTestEntries(headerCode, g_declared, 128);
  for (u32 i = 0; i < nDeclared; i++)
  {
    char needle[96];
    snprintf(needle, sizeof needle, ", %.63s}", g_declared[i]); /* names are < 64 bytes */
    if (!strstr(mainSrc, needle))
      Report("tests/unit/test_main.c", 1, "test entry point %s is declared in test.h but not listed in TESTS[]", g_declared[i]);
  }
  for (int i = 0; i < _count; i++)
  {
    const char* path = _paths[i];
    if (strncmp(path, "tests/unit/", 11) != 0 || !HasSuffix(path, ".c") || HasSuffix(path, "test_main.c"))
      continue;
    char* src = ReadText(path);
    if (!src)
      continue;
    char* code = StripCode(src, strlen(src));
    static char g_defined[128][64];
    u32 nDefined = CollectTestEntries(code, g_defined, 128);
    for (u32 d = 0; d < nDefined; d++)
    {
      bool found = false;
      for (u32 k = 0; k < nDeclared; k++)
        found |= strcmp(g_defined[d], g_declared[k]) == 0;
      if (!found)
        Report(path, 1, "test entry point %s is not declared in tests/unit/test.h", g_defined[d]);
    }
    NcXfree(code);
    NcXfree(src);
  }
  NcXfree(headerCode);
  NcXfree(header);
  NcXfree(mainSrc);
}

int main(int argc, char** argv)
{
  if (argc < 2)
  {
    fprintf(stderr, "usage: ncc-check-tree FILE...  (run from the repository root)\n");
    return 2;
  }
  const char* const* paths = (const char* const*)(argv + 1);
  int count = argc - 1;
  for (int i = 0; i < count; i++)
  {
    const char* path = paths[i];
    CheckPath(path);
    if (!HasSuffix(path, ".c") && !HasSuffix(path, ".h"))
      continue;
    u8* data;
    size_t size;
    if (!OsReadFile(path, &data, &size))
    {
      ReportPlain(path, 1, "cannot read");
      continue;
    }
    char* code = StripCode((const char*)data, size);
    CheckSpelling(path, code);
    CheckDeclarations(path, code);
    NcXfree(code);
    NcXfree(data);
  }
  CheckBuildRegistry(paths, count);
  CheckTestRegistry(paths, count);
  printf("ncc-check-tree: %d files, %u findings\n", count, g_findings);
  return g_findings > 255 ? 255 : (int)g_findings;
}
