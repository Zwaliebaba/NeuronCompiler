#define _POSIX_C_SOURCE 200809L /* popen: the tests run host tools */
#if defined(_WIN32)
#define popen _popen
#define pclose _pclose
#endif
/* test_coff.c — COFF writer: structure, long names, bigobj, relocation
 * overflow, determinism; cross-checked with llvm-readobj when available. */
#include "test.h"

#include "../../src/core/os.h"
#include "../../src/obj/coff/coff.h"

#include <stdlib.h>

/* Runs llvm-readobj with `args` on `obj`; returns the number of output lines
 * containing `needle` (or -1 if llvm-readobj failed). */
static int readobj_count(const char *args, const char *obj, const char *needle) {
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "llvm-readobj %s %s 2>&1", args, obj);
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    char line[1024];
    int n = 0;
    bool err = false;
    while (fgets(line, sizeof line, p)) {
        if (strstr(line, "error") || strstr(line, "warning")) err = true;
        if (strstr(line, needle)) n++;
    }
    int rc = pclose(p);
    return (rc != 0 || err) ? -1 : n;
}

static void write_obj(CoffObj *o, const char *name, char *path, size_t pathsz, Buf *out) {
    coff_serialize(o, out);
    snprintf(path, pathsz, "%s/%s", test_tmpdir(), name);
    CHECK(os_write_file_atomic(path, out->data, out->len));
}

static void small_object(Arena *a, Buf *out) {
    CoffObj o;
    coff_init(&o, a);
    CoffSec t = coff_add_section(&o, ".text$averyveryverylongname",
                                 IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ, 4);
    coff_set_comdat(&o, t, IMAGE_COMDAT_SELECT_NODUPLICATES, 0);
    Buf *b = &coff_section(&o, t)->data;
    buf_u32(b, 0x94000000); /* bl g */
    buf_u32(b, 0xD65F03C0); /* ret */
    CoffSym f = coff_symbol_named(&o, "a_function_with_a_long_name");
    coff_define(&o, f, t, 0, IMAGE_SYM_DTYPE_FUNCTION, IMAGE_SYM_CLASS_EXTERNAL);
    CoffSym g = coff_symbol_named(&o, "g");
    coff_add_reloc(&o, t, 0, g, IMAGE_REL_ARM64_BRANCH26);
    CoffSec bss = coff_add_section(&o, ".bss", IMAGE_SCN_CNT_UNINITIALIZED_DATA |
                                                   IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE, 16);
    coff_section(&o, bss)->bss_size = 4096;
    coff_add_directive(&o, "/DEFAULTLIB:kernel32");
    coff_serialize(&o, out);
    coff_free(&o);
}

void test_coff(void) {
    Arena a;
    arena_init(&a);
    bool have = test_have_tool("llvm-readobj");
    if (!have) fprintf(stderr, "coff: llvm-readobj not found, structural cross-checks SKIPPED\n");
    char path[512];

    /* Small object: header fields and determinism. */
    Buf o1 = {}, o2 = {};
    small_object(&a, &o1);
    small_object(&a, &o2);
    CHECK(o1.len == o2.len && memcmp(o1.data, o2.data, o1.len) == 0);
    CHECK(rd_u16(o1.data) == IMAGE_FILE_MACHINE_ARM64);
    CHECK(rd_u16(o1.data + 2) == 3);          /* .text$..., .bss, .drectve */
    CHECK(rd_u32(o1.data + 4) == 0);          /* TimeDateStamp */
    CHECK(rd_u32(o1.data + 12) == 3 * 2 + 3); /* 3 section syms + aux, @feat.00, f, g */
    snprintf(path, sizeof path, "%s/ncc-test-small.obj", test_tmpdir());
    CHECK(os_write_file_atomic(path, o1.data, o1.len));
    if (have) {
        CHECK(readobj_count("--sections --symbols --relocations", path,
                            ".text$averyveryverylongname") >= 1);
        CHECK(readobj_count("--symbols", path, "a_function_with_a_long_name") == 1);
        CHECK(readobj_count("--relocations", path, "IMAGE_REL_ARM64_BRANCH26 g") == 1);
        CHECK(readobj_count("--symbols", path, "Selection: NoDuplicates") == 1);
        /* .bss: size recorded, but no file data (PointerToRawData 0), as MSVC does. */
        CHECK(readobj_count("--sections", path, "RawDataSize: 4096") == 1);
    }
    buf_free(&o1);
    buf_free(&o2);

    /* More sections than a regular COFF header can count: bigobj. */
    {
        CoffObj o;
        coff_init(&o, &a);
        u32 n = 70000; /* > 65535 so the associative HighNumber field is exercised */
        CoffSec first = 0;
        for (u32 i = 0; i < n; i++) {
            CoffSec s = coff_add_section(&o, ".text$mn", IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE |
                                                             IMAGE_SCN_MEM_READ, 4);
            if (!first) first = s;
            coff_set_comdat(&o, s, IMAGE_COMDAT_SELECT_ANY, 0);
            buf_u32(&coff_section(&o, s)->data, 0xD65F03C0);
        }
        /* An associative section beyond 65535 exercises HighNumber. */
        CoffSec last = n;
        CoffSec assoc = coff_add_section(&o, ".xdata", IMAGE_SCN_CNT_INITIALIZED_DATA |
                                                           IMAGE_SCN_MEM_READ, 4);
        coff_set_comdat(&o, assoc, IMAGE_COMDAT_SELECT_ASSOCIATIVE, last);
        buf_u32(&coff_section(&o, assoc)->data, 0);
        Buf out = {};
        write_obj(&o, "ncc-test-bigobj.obj", path, sizeof path, &out);
        CHECK(rd_u16(out.data) == 0 && rd_u16(out.data + 2) == 0xFFFF && rd_u16(out.data + 4) == 2);
        CHECK(rd_u16(out.data + 6) == IMAGE_FILE_MACHINE_ARM64);
        CHECK(rd_u32(out.data + 44) == n + 1);
        if (have) {
            CHECK(readobj_count("--file-headers", path, "SectionCount: 70001") == 1);
            char needle[64];
            snprintf(needle, sizeof needle, "AssocSection: .text$mn (%u)", last);
            CHECK(readobj_count("--symbols", path, needle) == 1);
        }
        buf_free(&out);
        coff_free(&o);
        (void)first;
    }

    /* More than 65535 relocations in one section: IMAGE_SCN_LNK_NRELOC_OVFL. */
    {
        CoffObj o;
        coff_init(&o, &a);
        CoffSec d = coff_add_section(&o, ".data", IMAGE_SCN_CNT_INITIALIZED_DATA |
                                                      IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE, 8);
        CoffSym target = coff_symbol_named(&o, "target");
        u32 n = 70000;
        buf_zeros(&coff_section(&o, d)->data, 8 * n);
        for (u32 i = 0; i < n; i++) coff_add_reloc(&o, d, 8 * i, target, IMAGE_REL_ARM64_ADDR64);
        Buf out = {};
        write_obj(&o, "ncc-test-relocs.obj", path, sizeof path, &out);
        if (have) CHECK(readobj_count("--relocations", path, "IMAGE_REL_ARM64_ADDR64 target") ==
                        (int)n);
        buf_free(&out);
        coff_free(&o);
    }
    arena_release(&a);
}
