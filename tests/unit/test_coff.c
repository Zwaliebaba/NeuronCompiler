/* test_coff.c — COFF writer: structure, long names, bigobj, relocation
 * overflow, determinism; cross-checked with llvm-readobj when available. */
#include "test.h"

#include "../../src/core/os.h"
#include "../../src/obj/coff/coff.h"

#include <stdlib.h>

/* Runs llvm-readobj with `_args` on `_obj`; returns the number of output lines
 * containing `_needle` (or -1 if llvm-readobj failed). The output goes through
 * a scratch file rather than popen(), which ISO C does not have. */
static int ReadobjCount(const char* _args, const char* _obj, const char* _needle)
{
  char out[512], cmd[1600];
  snprintf(out, sizeof out, "%s/ncc-test-readobj.txt", TestTmpdir());
  snprintf(cmd, sizeof cmd, "llvm-readobj %s %s > %s 2>&1", _args, _obj, out);
  int rc = system(cmd);
  FILE* p = fopen(out, "r");
  if (!p)
    return -1;
  char line[1024];
  int n = 0;
  bool err = false;
  while (fgets(line, sizeof line, p))
  {
    if (strstr(line, "error") || strstr(line, "warning"))
      err = true;
    if (strstr(line, _needle))
      n++;
  }
  fclose(p);
  return (rc != 0 || err) ? -1 : n;
}

static void WriteObj(CoffObj* _o, const char* _name, char* _path, size_t _pathsz, Buf* _out)
{
  CoffSerialize(_o, _out);
  snprintf(_path, _pathsz, "%s/%s", TestTmpdir(), _name);
  CHECK(OsWriteFileAtomic(_path, _out->data, _out->len));
}

static void SmallObject(Arena* _a, Buf* _out)
{
  CoffObj o;
  CoffInit(&o, _a);
  CoffSec t = CoffAddSection(&o, ".text$averyveryverylongname", IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ, 4);
  CoffSetComdat(&o, t, IMAGE_COMDAT_SELECT_NODUPLICATES, 0);
  Buf* b = &CoffSectionGet(&o, t)->data;
  BufU32(b, 0x94000000); /* bl g */
  BufU32(b, 0xD65F03C0); /* ret */
  CoffSym f = CoffSymbolNamed(&o, "a_function_with_a_long_name");
  CoffDefine(&o, f, t, 0, IMAGE_SYM_DTYPE_FUNCTION, IMAGE_SYM_CLASS_EXTERNAL);
  CoffSym g = CoffSymbolNamed(&o, "g");
  CoffAddReloc(&o, t, 0, g, IMAGE_REL_ARM64_BRANCH26);
  CoffSec bss = CoffAddSection(&o, ".bss", IMAGE_SCN_CNT_UNINITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE, 16);
  CoffSectionGet(&o, bss)->bssSize = 4096;
  CoffAddDirective(&o, "/DEFAULTLIB:kernel32");
  CoffSerialize(&o, _out);
  CoffFree(&o);
}

void TestCoff(void)
{
  Arena a;
  ArenaInit(&a);
  bool have = TestHaveTool("llvm-readobj");
  if (!have)
    fprintf(stderr, "coff: llvm-readobj not found, structural cross-checks SKIPPED\n");
  char path[512];

  /* Small object: header fields and determinism. */
  Buf o1 = {}, o2 = {};
  SmallObject(&a, &o1);
  SmallObject(&a, &o2);
  CHECK(o1.len == o2.len && memcmp(o1.data, o2.data, o1.len) == 0);
  CHECK(BufReadU16(o1.data) == IMAGE_FILE_MACHINE_ARM64);
  CHECK(BufReadU16(o1.data + 2) == 3);          /* .text$..., .bss, .drectve */
  CHECK(BufReadU32(o1.data + 4) == 0);          /* TimeDateStamp */
  CHECK(BufReadU32(o1.data + 12) == 3 * 2 + 3); /* 3 section syms + aux, @feat.00, f, g */
  snprintf(path, sizeof path, "%s/ncc-test-small.obj", TestTmpdir());
  CHECK(OsWriteFileAtomic(path, o1.data, o1.len));
  if (have)
  {
    CHECK(ReadobjCount("--sections --symbols --relocations", path, ".text$averyveryverylongname") >= 1);
    CHECK(ReadobjCount("--symbols", path, "a_function_with_a_long_name") == 1);
    CHECK(ReadobjCount("--relocations", path, "IMAGE_REL_ARM64_BRANCH26 g") == 1);
    CHECK(ReadobjCount("--symbols", path, "Selection: NoDuplicates") == 1);
    /* .bss: size recorded, but no file data (PointerToRawData 0), as MSVC does. */
    CHECK(ReadobjCount("--sections", path, "RawDataSize: 4096") == 1);
  }
  BufFree(&o1);
  BufFree(&o2);

  /* More sections than a regular COFF header can count: bigobj. */
  {
    CoffObj o;
    CoffInit(&o, &a);
    u32 n = 70000; /* > 65535 so the associative HighNumber field is exercised */
    CoffSec first = 0;
    for (u32 i = 0; i < n; i++)
    {
      CoffSec s = CoffAddSection(&o, ".text$mn", IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ, 4);
      if (!first)
        first = s;
      CoffSetComdat(&o, s, IMAGE_COMDAT_SELECT_ANY, 0);
      BufU32(&CoffSectionGet(&o, s)->data, 0xD65F03C0);
    }
    /* An associative section beyond 65535 exercises HighNumber. */
    CoffSec last = n;
    CoffSec assoc = CoffAddSection(&o, ".xdata", IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ, 4);
    CoffSetComdat(&o, assoc, IMAGE_COMDAT_SELECT_ASSOCIATIVE, last);
    BufU32(&CoffSectionGet(&o, assoc)->data, 0);
    Buf out = {};
    WriteObj(&o, "ncc-test-bigobj.obj", path, sizeof path, &out);
    CHECK(BufReadU16(out.data) == 0 && BufReadU16(out.data + 2) == 0xFFFF && BufReadU16(out.data + 4) == 2);
    CHECK(BufReadU16(out.data + 6) == IMAGE_FILE_MACHINE_ARM64);
    CHECK(BufReadU32(out.data + 44) == n + 1);
    if (have)
    {
      CHECK(ReadobjCount("--file-headers", path, "SectionCount: 70001") == 1);
      char needle[64];
      snprintf(needle, sizeof needle, "AssocSection: .text$mn (%u)", last);
      CHECK(ReadobjCount("--symbols", path, needle) == 1);
    }
    BufFree(&out);
    CoffFree(&o);
    (void)first;
  }

  /* More than 65535 relocations in one section: IMAGE_SCN_LNK_NRELOC_OVFL. */
  {
    CoffObj o;
    CoffInit(&o, &a);
    CoffSec d = CoffAddSection(&o, ".data", IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE, 8);
    CoffSym target = CoffSymbolNamed(&o, "target");
    u32 n = 70000;
    BufZeros(&CoffSectionGet(&o, d)->data, 8 * n);
    for (u32 i = 0; i < n; i++)
      CoffAddReloc(&o, d, 8 * i, target, IMAGE_REL_ARM64_ADDR64);
    Buf out = {};
    WriteObj(&o, "ncc-test-relocs.obj", path, sizeof path, &out);
    if (have)
      CHECK(ReadobjCount("--relocations", path, "IMAGE_REL_ARM64_ADDR64 target") == (int)n);
    BufFree(&out);
    CoffFree(&o);
  }
  ArenaRelease(&a);
}
