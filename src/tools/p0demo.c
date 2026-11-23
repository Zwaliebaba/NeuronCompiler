/* p0demo.c — phase-0 end-to-end driver (Design/15-roadmap.md, phase 0).
 *
 * Builds a small program directly as machine IR, without a front end, and
 * writes it as an ARM64 COFF object plus a listing and a pseudo-source file
 * that the CodeView line tables point at (so stepping in a debugger shows
 * meaningful lines).
 *
 *   p0demo nocrt out.obj   no CRT: entry point p0_start, kernel32 only,
 *                          provides its own __chkstk. Links with
 *                          link.exe /ENTRY:p0_start /NODEFAULTLIB kernel32.lib
 *   p0demo crt out.obj     main() for the MSVC CRT: printf (variadic),
 *                          thread-local storage, the CRT's __chkstk.
 *
 * Exercised: packed and full unwind records, frameless leaves, frames with
 * saved registers, a variadic function with homed arguments, an 8 KiB frame
 * with __chkstk, a function with no epilogue, dllimport calls via __imp_,
 * direct calls to import thunks, ADRP/ADD/LDR relocations, ADDR64 data
 * relocations, TLS (SECREL_HIGH12A/LOW12A), branches and loops. */
#include "../core/arena.h"
#include "../core/os.h"
#include "../target/arm64/a64_module.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static A64Module M;
static Buf src;       /* pseudo-source text */
static u32 src_line;  /* lines emitted so far */

/* Appends a pseudo-source line and stamps following instructions with it. */
static void SRC(MFunc *f, const char *fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    buf_bytes(&src, tmp, (u32)n);
    buf_u8(&src, '\n');
    src_line++;
    if (f) mf_line(f, src_line);
}

/* ---- tiny assembler vocabulary ------------------------------------------ */

static void ins(MFunc *f, MInst mi) { mf_emit(f, mi); }

static MInst rr(A64Op op, u32 d, u32 n, u32 m) {
    return (MInst){.op = op, .rd = (u8)d, .rn = (u8)n, .rm = (u8)m};
}
static MInst ri(A64Op op, u32 d, u32 n, i64 imm) {
    return (MInst){.op = op, .rd = (u8)d, .rn = (u8)n, .imm = imm};
}
static MInst bcond(A64Cond c, u32 label) {
    return (MInst){.op = A64_Bcc, .shift = c, .tkind = TK_LABEL, .target = label};
}
static void call(MFunc *f, const char *name) {
    ins(f, (MInst){.op = A64_BL, .tkind = TK_SYM, .target = a64m_sym(&M, name)});
}
/* Call through the import address table, as for a dllimport declaration. */
static void call_import(MFunc *f, const char *name) {
    char imp[128];
    snprintf(imp, sizeof imp, "__imp_%s", name);
    u32 s = a64m_sym(&M, imp);
    ins(f, (MInst){.op = A64_ADRP, .rd = A64_IP0, .tkind = TK_SYM, .target = s});
    ins(f, (MInst){.op = A64_LDRXui, .rd = A64_IP0, .rn = A64_IP0, .tkind = TK_SYM, .target = s});
    ins(f, (MInst){.op = A64_BLR, .rn = A64_IP0});
}
static void addr_of(MFunc *f, u32 rd, const char *name) {
    u32 s = a64m_sym(&M, name);
    ins(f, (MInst){.op = A64_ADRP, .rd = (u8)rd, .tkind = TK_SYM, .target = s});
    ins(f, (MInst){.op = A64_ADDXri, .rd = (u8)rd, .rn = (u8)rd, .tkind = TK_SYM, .target = s});
}
static void mov(MFunc *f, u32 d, u32 m) { ins(f, rr(A64_MOVXr, d, 0, m)); }

static void emit_string(const char *name, const char *s) {
    a64m_emit_data(&M, name, true, DATA_RO, s, (u32)strlen(s) + 1, 1, nullptr, 0);
}

/* ---- functions ---------------------------------------------------------- */

/* sum_squares(n): frameless leaf. */
static void build_sum_squares(void) {
    MFunc f;
    mf_init(&f, "sum_squares", true, (FrameDesc){.frameless = true});
    SRC(&f, "static int sum_squares(int n) {   // frameless leaf: no unwind data");
    ins(&f, (MInst){.op = A64_P_PROLOGUE});
    u32 loop = mf_new_label(&f), done = mf_new_label(&f);
    SRC(&f, "    int acc = 0;");
    ins(&f, rr(A64_MOVWr, 1, 0, A64_ZR));
    SRC(&f, "    for (; n != 0; n--) acc += n * n;");
    ins(&f, (MInst){.op = A64_CBZW, .rd = 0, .tkind = TK_LABEL, .target = done});
    mf_label(&f, loop);
    ins(&f, (MInst){.op = A64_MADDW, .rd = 1, .rn = 0, .rm = 0, .ra = 1});
    ins(&f, ri(A64_SUBSWri, 0, 0, 1));
    ins(&f, bcond(CC_NE, loop));
    mf_label(&f, done);
    SRC(&f, "    return acc;");
    ins(&f, rr(A64_MOVWr, 0, 0, 1));
    ins(&f, (MInst){.op = A64_P_EPILOGUE});
    SRC(&f, "}");
    a64m_emit_function(&M, &f);
    mf_free(&f);
}

/* big_frame(): 8 KiB of locals, so the prologue calls __chkstk. */
static void build_big_frame(void) {
    MFunc f;
    mf_init(&f, "big_frame", true, (FrameDesc){.locals = 8192});
    SRC(&f, "static int big_frame(void) {       // 8 KiB frame: __chkstk probe");
    ins(&f, (MInst){.op = A64_P_PROLOGUE});
    SRC(&f, "    unsigned char buf[8192]; int sum = 0;");
    ins(&f, (MInst){.op = A64_MOVXsp, .rd = 8, .rn = A64_SP});
    ins(&f, (MInst){.op = A64_MOVZX, .rd = 10});
    ins(&f, (MInst){.op = A64_MOVZW, .rd = 12});
    u32 l1 = mf_new_label(&f), l2 = mf_new_label(&f);
    SRC(&f, "    for (size_t i = 0; i < 8192; i++) buf[i] = (unsigned char)i;");
    mf_label(&f, l1);
    ins(&f, (MInst){.op = A64_STRBBro, .rd = 10, .rn = 8, .rm = 10, .shift = EX_UXTX});
    ins(&f, ri(A64_ADDXri, 10, 10, 1));
    ins(&f, (MInst){.op = A64_CMPXri, .rn = 10, .imm = 8192});
    ins(&f, bcond(CC_LO, l1));
    SRC(&f, "    for (size_t i = 0; i < 8192; i++) sum += buf[i];");
    ins(&f, (MInst){.op = A64_MOVZX, .rd = 10});
    mf_label(&f, l2);
    ins(&f, (MInst){.op = A64_LDRBBro, .rd = 11, .rn = 8, .rm = 10, .shift = EX_UXTX});
    ins(&f, rr(A64_ADDWrs, 12, 12, 11));
    ins(&f, ri(A64_ADDXri, 10, 10, 1));
    ins(&f, (MInst){.op = A64_CMPXri, .rn = 10, .imm = 8192});
    ins(&f, bcond(CC_LO, l2));
    SRC(&f, "    return sum;                       // 1044480");
    ins(&f, rr(A64_MOVWr, 0, 0, 12));
    ins(&f, (MInst){.op = A64_P_EPILOGUE});
    SRC(&f, "}");
    a64m_emit_function(&M, &f);
    mf_free(&f);
}

/* sum_va(n, ...): variadic, homes x0-x7 next to the stack arguments. */
static void build_sum_va(void) {
    MFunc f;
    mf_init(&f, "sum_va", true, (FrameDesc){.home_args = true});
    FramePlan plan;
    frame_plan(&f.frame, &plan);
    SRC(&f, "static long long sum_va(int n, ...) {   // variadic: x0-x7 homed");
    ins(&f, (MInst){.op = A64_P_PROLOGUE});
    SRC(&f, "    va_list ap; va_start(ap, n); long long s = 0;");
    ins(&f, ri(A64_ADDXri, 9, A64_SP, plan.home_sp_offset + 8));
    ins(&f, rr(A64_MOVWr, 10, 0, 0));
    ins(&f, rr(A64_MOVXr, 0, 0, A64_ZR));
    u32 loop = mf_new_label(&f), done = mf_new_label(&f);
    SRC(&f, "    while (n--) s += va_arg(ap, long long);");
    ins(&f, (MInst){.op = A64_CBZW, .rd = 10, .tkind = TK_LABEL, .target = done});
    mf_label(&f, loop);
    ins(&f, ri(A64_LDRXui, 11, 9, 0));
    ins(&f, ri(A64_ADDXri, 9, 9, 8));
    ins(&f, rr(A64_ADDXrs, 0, 0, 11));
    ins(&f, ri(A64_SUBSWri, 10, 10, 1));
    ins(&f, bcond(CC_NE, loop));
    mf_label(&f, done);
    SRC(&f, "    return s;");
    ins(&f, (MInst){.op = A64_P_EPILOGUE});
    SRC(&f, "}");
    a64m_emit_function(&M, &f);
    mf_free(&f);
}

/* write_all(h, buf, len): WriteFile via the import address table. */
static void build_write_all(void) {
    MFunc f;
    mf_init(&f, "write_all", true, (FrameDesc){.locals = 16});
    FramePlan plan;
    frame_plan(&f.frame, &plan);
    SRC(&f, "static void write_all(HANDLE h, const char *p, DWORD n) {");
    ins(&f, (MInst){.op = A64_P_PROLOGUE});
    SRC(&f, "    DWORD written; WriteFile(h, p, n, &written, NULL);   // __imp_WriteFile");
    ins(&f, ri(A64_ADDXri, 3, A64_SP, plan.locals_sp_offset));
    mov(&f, 4, A64_ZR);
    call_import(&f, "WriteFile");
    ins(&f, (MInst){.op = A64_P_EPILOGUE});
    SRC(&f, "}");
    a64m_emit_function(&M, &f);
    mf_free(&f);
}

/* print_uint(h, v): decimal conversion into a local buffer. */
static void build_print_uint(void) {
    MFunc f;
    mf_init(&f, "print_uint", true, (FrameDesc){.n_gpr = 1, .locals = 32});
    FramePlan plan;
    frame_plan(&f.frame, &plan);
    u32 buf_end = plan.locals_sp_offset + 32;
    SRC(&f, "static void print_uint(HANDLE h, unsigned v) {");
    ins(&f, (MInst){.op = A64_P_PROLOGUE});
    SRC(&f, "    char buf[32], *p = buf + 32; *--p = '\\n';");
    mov(&f, 19, 0);
    ins(&f, ri(A64_ADDXri, 9, A64_SP, buf_end));
    ins(&f, ri(A64_SUBXri, 9, 9, 1));
    ins(&f, (MInst){.op = A64_MOVZW, .rd = 10, .imm = '\n'});
    ins(&f, ri(A64_STRBBui, 10, 9, 0));
    ins(&f, (MInst){.op = A64_MOVZW, .rd = 11, .imm = 10});
    u32 loop = mf_new_label(&f);
    SRC(&f, "    do { *--p = '0' + v % 10; v /= 10; } while (v);");
    mf_label(&f, loop);
    ins(&f, rr(A64_UDIVW, 12, 1, 11));
    ins(&f, (MInst){.op = A64_MSUBW, .rd = 13, .rn = 12, .rm = 11, .ra = 1});
    ins(&f, ri(A64_ADDWri, 13, 13, '0'));
    ins(&f, ri(A64_SUBXri, 9, 9, 1));
    ins(&f, ri(A64_STRBBui, 13, 9, 0));
    ins(&f, rr(A64_MOVWr, 1, 0, 12));
    ins(&f, (MInst){.op = A64_CBNZW, .rd = 1, .tkind = TK_LABEL, .target = loop});
    SRC(&f, "    write_all(h, p, (DWORD)(buf + 32 - p));");
    ins(&f, ri(A64_ADDXri, 10, A64_SP, buf_end));
    ins(&f, rr(A64_SUBXrs, 2, 10, 9));
    mov(&f, 1, 9);
    mov(&f, 0, 19);
    call(&f, "write_all");
    ins(&f, (MInst){.op = A64_P_EPILOGUE});
    SRC(&f, "}");
    a64m_emit_function(&M, &f);
    mf_free(&f);
}

/* Prints a label string followed by the value in w1 (x19 holds the handle). */
static void say(MFunc *f, const char *sym, const char *text) {
    emit_string(sym, text);
    mov(f, 0, 19);
    addr_of(f, 1, sym);
    mf_mov_imm(f, 2, strlen(text), true);
    call(f, "write_all");
}

static void build_p0_main(void) {
    MFunc f;
    mf_init(&f, "p0_main", false, (FrameDesc){.n_gpr = 2});
    SRC(&f, "int p0_main(void) {");
    ins(&f, (MInst){.op = A64_P_PROLOGUE});
    SRC(&f, "    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);        // __imp_GetStdHandle");
    mf_mov_imm(&f, 0, (u32)-11, true);
    call_import(&f, "GetStdHandle");
    mov(&f, 19, 0);

    SRC(&f, "    puts(\"NeuronCompiler phase 0 ...\");");
    say(&f, "msg_hello", "NeuronCompiler phase 0: hand-built ARM64, no CRT\n");

    SRC(&f, "    print(\"sum of squares 1..10 = \", sum_squares(10));     // 385");
    say(&f, "msg_squares", "sum of squares 1..10 = ");
    mf_mov_imm(&f, 0, 10, true);
    call(&f, "sum_squares");
    ins(&f, rr(A64_MOVWr, 1, 0, 0));
    mov(&f, 0, 19);
    call(&f, "print_uint");

    SRC(&f, "    print(\"8 KiB frame checksum = \", big_frame());         // 1044480");
    say(&f, "msg_big", "8 KiB frame checksum = ");
    call(&f, "big_frame");
    ins(&f, rr(A64_MOVWr, 1, 0, 0));
    mov(&f, 0, 19);
    call(&f, "print_uint");

    SRC(&f, "    print(\"variadic sum(10, 20, 30) = \", sum_va(3, 10, 20, 30));   // 60");
    say(&f, "msg_va", "variadic sum(10, 20, 30) = ");
    mf_mov_imm(&f, 0, 3, false);
    mf_mov_imm(&f, 1, 10, false);
    mf_mov_imm(&f, 2, 20, false);
    mf_mov_imm(&f, 3, 30, false);
    call(&f, "sum_va");
    ins(&f, rr(A64_MOVWr, 1, 0, 0));
    mov(&f, 0, 19);
    call(&f, "print_uint");

    SRC(&f, "    write_all(out, table[1], ...);          // pointer from an ADDR64 relocation");
    static const char ok[] = "data relocation ok\n";
    emit_string("msg_ok", ok);
    emit_string("msg_unused", "unused\n");
    u8 zeros[16] = {};
    A64DataReloc rel[2] = {{0, a64m_sym(&M, "msg_unused")}, {8, a64m_sym(&M, "msg_ok")}};
    a64m_emit_data(&M, "table", true, DATA_RW, zeros, 16, 8, rel, 2);
    addr_of(&f, 8, "table");
    ins(&f, ri(A64_LDRXui, 1, 8, 8));
    mf_mov_imm(&f, 2, sizeof ok - 1, true);
    mov(&f, 0, 19);
    call(&f, "write_all");

    SRC(&f, "    return 0;");
    ins(&f, (MInst){.op = A64_MOVZW, .rd = 0});
    ins(&f, (MInst){.op = A64_P_EPILOGUE});
    SRC(&f, "}");
    a64m_emit_function(&M, &f);
    mf_free(&f);
}

/* Entry point without a CRT: never returns, so it has no epilogue. */
static void build_p0_start(void) {
    MFunc f;
    mf_init(&f, "p0_start", false, (FrameDesc){});
    SRC(&f, "void p0_start(void) {                    // /ENTRY: no epilogue");
    ins(&f, (MInst){.op = A64_P_PROLOGUE});
    SRC(&f, "    ExitProcess(p0_main());                // ExitProcess via import thunk");
    call(&f, "p0_main");
    call(&f, "ExitProcess");
    ins(&f, (MInst){.op = A64_BRK, .imm = 0xF000});
    SRC(&f, "}");
    a64m_emit_function(&M, &f);
    mf_free(&f);
}

/* Our own stack probe for the no-CRT build: touches every page of the
 * requested allocation (x15 = size / 16) below sp. Preserves x15. */
static void build_chkstk(void) {
    MFunc f;
    mf_init(&f, "__chkstk", false, (FrameDesc){.frameless = true});
    SRC(&f, "void __chkstk(void) {                   // x15 = size/16; probes each page");
    ins(&f, (MInst){.op = A64_P_PROLOGUE});
    ins(&f, (MInst){.op = A64_LSLXi, .rd = 16, .rn = 15, .amount = 4});
    ins(&f, (MInst){.op = A64_MOVXsp, .rd = 17, .rn = A64_SP});
    u32 loop = mf_new_label(&f);
    mf_label(&f, loop);
    ins(&f, ri(A64_SUBXri, 17, 17, 4096));
    ins(&f, ri(A64_LDRXui, A64_ZR, 17, 0));
    ins(&f, ri(A64_SUBSXri, 16, 16, 4096));
    ins(&f, bcond(CC_GT, loop));
    ins(&f, (MInst){.op = A64_P_EPILOGUE});
    SRC(&f, "}");
    a64m_emit_function(&M, &f);
    mf_free(&f);
}

/* main() for the MSVC CRT: printf, TLS, the CRT's __chkstk. */
static void build_crt_main(void) {
    a64m_emit_data(&M, "tls_counter", false, DATA_TLS, (const u8[]){100, 0, 0, 0}, 4, 4,
                   nullptr, 0);
    emit_string("fmt1", "%s from ncc phase 0: %d, thread-local counter = %d\n");
    emit_string("str_hello", "hello");
    emit_string("fmt2", "8 KiB frame checksum = %d, variadic sum = %lld\n");

    MFunc f;
    mf_init(&f, "main", false, (FrameDesc){.n_gpr = 2});
    SRC(&f, "int main(void) {");
    ins(&f, (MInst){.op = A64_P_PROLOGUE});
    SRC(&f, "    int t = ++tls_counter;                // implicit TLS: x18 + 0x58, _tls_index");
    ins(&f, ri(A64_LDRXui, 8, A64_X18, 0x58));
    u32 ti = a64m_sym(&M, "_tls_index");
    ins(&f, (MInst){.op = A64_ADRP, .rd = 9, .tkind = TK_SYM, .target = ti});
    ins(&f, (MInst){.op = A64_LDRWui, .rd = 9, .rn = 9, .tkind = TK_SYM, .target = ti});
    ins(&f, (MInst){.op = A64_LDRXro, .rd = 8, .rn = 8, .rm = 9, .shift = EX_UXTX, .amount = 1});
    u32 tc = a64m_sym(&M, "tls_counter");
    ins(&f, (MInst){.op = A64_ADDXri, .rd = 8, .rn = 8, .tkind = TK_SYM, .target = tc,
                    .smod = SM_SECREL_HI12});
    ins(&f, (MInst){.op = A64_ADDXri, .rd = 8, .rn = 8, .tkind = TK_SYM, .target = tc,
                    .smod = SM_SECREL_LO12});
    ins(&f, ri(A64_LDRWui, 3, 8, 0));
    ins(&f, ri(A64_ADDWri, 3, 3, 1));
    ins(&f, ri(A64_STRWui, 3, 8, 0));
    SRC(&f, "    printf(fmt1, \"hello\", 42, t);       // variadic call: all args in x0-x3");
    addr_of(&f, 0, "fmt1");
    addr_of(&f, 1, "str_hello");
    mf_mov_imm(&f, 2, 42, true);
    call(&f, "printf");
    SRC(&f, "    int c = big_frame();");
    call(&f, "big_frame");
    mov(&f, 19, 0);
    SRC(&f, "    long long s = sum_va(3, 10, 20, 30);");
    mf_mov_imm(&f, 0, 3, false);
    mf_mov_imm(&f, 1, 10, false);
    mf_mov_imm(&f, 2, 20, false);
    mf_mov_imm(&f, 3, 30, false);
    call(&f, "sum_va");
    mov(&f, 2, 0);
    SRC(&f, "    printf(fmt2, c, s);");
    ins(&f, rr(A64_MOVWr, 1, 0, 19));
    addr_of(&f, 0, "fmt2");
    call(&f, "printf");
    SRC(&f, "    return 0;");
    ins(&f, (MInst){.op = A64_MOVZW, .rd = 0});
    ins(&f, (MInst){.op = A64_P_EPILOGUE});
    SRC(&f, "}");
    a64m_emit_function(&M, &f);
    mf_free(&f);
}

/* ---- main --------------------------------------------------------------- */

static bool write_buf(const char *path, const Buf *b) {
    if (!os_write_file_atomic(path, b->data, b->len)) {
        fprintf(stderr, "p0demo: cannot write %s\n", path);
        return false;
    }
    return true;
}

int main(int argc, char **argv) {
    if (argc != 3 || (strcmp(argv[1], "nocrt") != 0 && strcmp(argv[1], "crt") != 0)) {
        fprintf(stderr, "usage: p0demo nocrt|crt out.obj\n");
        return 2;
    }
    bool crt = strcmp(argv[1], "crt") == 0;
    const char *out = argv[2];

    Arena arena;
    arena_init(&arena);
    char *src_path = arena_printf(&arena, "%s.pseudo.c", out);
    char *lst_path = arena_printf(&arena, "%s.lst", out);
    a64m_init(&M, &arena, out, src_path, true);

    SRC(nullptr, "/* Pseudo-source for %s, generated by p0demo (%s mode). The machine code was", out,
        argv[1]);
    SRC(nullptr, " * built by hand; these lines exist so line tables have something to show. */");

    build_sum_squares();
    build_big_frame();
    build_sum_va();
    if (crt) {
        build_crt_main();
        a64m_directive(&M, "/DEFAULTLIB:msvcrt");
        a64m_directive(&M, "/DEFAULTLIB:oldnames");
        a64m_directive(&M, "/DEFAULTLIB:legacy_stdio_definitions");
    } else {
        build_write_all();
        build_print_uint();
        build_p0_main();
        build_p0_start();
        build_chkstk();
        a64m_directive(&M, "/DEFAULTLIB:kernel32");
    }

    Buf obj = {};
    a64m_finish(&M, &obj);
    bool ok = write_buf(out, &obj) && write_buf(src_path, &src) && write_buf(lst_path, &M.listing);
    if (ok)
        printf("p0demo: wrote %s (%u bytes): %u functions, %u packed unwind, %u xdata, "
               "all unwind data verified\n",
               out, obj.len, M.nfuncs, M.npacked, M.nxdata);
    buf_free(&obj);
    buf_free(&src);
    a64m_free(&M);
    arena_release(&arena);
    return ok ? 0 : 1;
}
