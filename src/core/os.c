/* os.c — host layer. Uses only ISO C file I/O plus rename semantics, which
 * differ between POSIX (rename replaces) and Windows (it does not). */
#include "os.h"

#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
/* Declared here instead of including windows.h (Design/13 §5). */
__declspec(dllimport) int __stdcall MoveFileExA(const char *from, const char *to, unsigned flags);
enum { NC_MOVEFILE_REPLACE_EXISTING = 1, NC_MOVEFILE_WRITE_THROUGH = 8 };
#endif

bool os_read_file(const char *path, u8 **data, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return false; }
    u8 *p = nc_xrealloc(nullptr, (size_t)n + 1);
    size_t got = fread(p, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { nc_xfree(p); return false; }
    p[n] = 0;
    *data = p;
    *size = (size_t)n;
    return true;
}

bool os_write_file_atomic(const char *path, const void *data, size_t size) {
    size_t plen = strlen(path);
    char *tmp = nc_xrealloc(nullptr, plen + 16);
    memcpy(tmp, path, plen);
    memcpy(tmp + plen, ".ncc-tmp", 9);
    FILE *f = fopen(tmp, "wb");
    if (!f) { nc_xfree(tmp); return false; }
    bool ok = fwrite(data, 1, size, f) == size;
    ok = (fclose(f) == 0) && ok;
    if (ok) {
#if defined(_WIN32)
        ok = MoveFileExA(tmp, path, NC_MOVEFILE_REPLACE_EXISTING | NC_MOVEFILE_WRITE_THROUGH) != 0;
#else
        ok = rename(tmp, path) == 0;
#endif
    }
    if (!ok) remove(tmp);
    nc_xfree(tmp);
    return ok;
}
