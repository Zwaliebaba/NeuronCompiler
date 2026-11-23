/* os.c — host layer. Uses only ISO C file I/O plus rename semantics, which
 * differ between POSIX (rename replaces) and Windows (it does not). */
#include "os.h"

#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
/* Declared here instead of including windows.h (Design/13 §5). */
NC_DLLIMPORT int NC_WINAPI MoveFileExA(const char* _existingFileName, const char* _newFileName, unsigned _flags);
enum
{
  NC_MOVEFILE_REPLACE_EXISTING = 1,
  NC_MOVEFILE_WRITE_THROUGH = 8
};
#endif

bool OsReadFile(const char* _path, u8** _data, size_t* _size)
{
  FILE* f = fopen(_path, "rb");
  if (!f)
    return false;
  if (fseek(f, 0, SEEK_END) != 0)
  {
    fclose(f);
    return false;
  }
  long long n = ftell(f);
  if (n < 0 || fseek(f, 0, SEEK_SET) != 0)
  {
    fclose(f);
    return false;
  }
  u8* p = NcXrealloc(nullptr, (size_t)n + 1);
  size_t got = fread(p, 1, (size_t)n, f);
  fclose(f);
  if (got != (size_t)n)
  {
    NcXfree(p);
    return false;
  }
  p[n] = 0;
  *_data = p;
  *_size = (size_t)n;
  return true;
}

bool OsWriteFileAtomic(const char* _path, const void* _data, size_t _size)
{
  size_t plen = strlen(_path);
  char* tmp = NcXrealloc(nullptr, plen + 16);
  memcpy(tmp, _path, plen);
  memcpy(tmp + plen, ".ncc-tmp", 9);
  FILE* f = fopen(tmp, "wb");
  if (!f)
  {
    NcXfree(tmp);
    return false;
  }
  bool ok = fwrite(_data, 1, _size, f) == _size;
  ok = (fclose(f) == 0) && ok;
  if (ok)
  {
#if defined(_WIN32)
    ok = MoveFileExA(tmp, _path, NC_MOVEFILE_REPLACE_EXISTING | NC_MOVEFILE_WRITE_THROUGH) != 0;
#else
    ok = rename(tmp, _path) == 0;
#endif
  }
  if (!ok)
    remove(tmp);
  NcXfree(tmp);
  return ok;
}
