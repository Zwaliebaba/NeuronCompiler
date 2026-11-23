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

static A64Module g_module;
static Buf g_src;     /* pseudo-source text */
static u32 g_srcLine; /* lines emitted so far */

/* Appends a pseudo-source line and stamps following instructions with it. */
static void SRC(MirFunc* _f, const char* _fmt, ...)
{
  char tmp[256];
  va_list ap;
  va_start(ap, _fmt);
  int n = vsnprintf(tmp, sizeof tmp, _fmt, ap);
  va_end(ap);
  BufBytes(&g_src, tmp, (u32)n);
  BufU8(&g_src, '\n');
  g_srcLine++;
  if (_f)
    MirFuncLine(_f, g_srcLine);
}

/* ---- tiny assembler vocabulary ------------------------------------------ */

static void Ins(MirFunc* _f, MirInst _mi)
{
  MirFuncEmit(_f, _mi);
}

static MirInst Rr(A64Op _op, u32 _d, u32 _n, u32 _m)
{
  return (MirInst){.op = _op, .rd = (u8)_d, .rn = (u8)_n, .rm = (u8)_m};
}
static MirInst Ri(A64Op _op, u32 _d, u32 _n, i64 _imm)
{
  return (MirInst){.op = _op, .rd = (u8)_d, .rn = (u8)_n, .imm = _imm};
}
static MirInst Bcond(A64Cond _c, u32 _label)
{
  return (MirInst){.op = A64OpBcc, .shift = _c, .tkind = A64TargetKindLabel, .target = _label};
}
static void Call(MirFunc* _f, const char* _name)
{
  Ins(_f, (MirInst){.op = A64OpBL, .tkind = A64TargetKindSym, .target = A64ModuleSym(&g_module, _name)});
}
/* Call through the import address table, as for a dllimport declaration. */
static void CallImport(MirFunc* _f, const char* _name)
{
  char imp[128];
  snprintf(imp, sizeof imp, "__imp_%s", _name);
  u32 s = A64ModuleSym(&g_module, imp);
  Ins(_f, (MirInst){.op = A64OpADRP, .rd = A64_IP0, .tkind = A64TargetKindSym, .target = s});
  Ins(_f, (MirInst){.op = A64OpLDRXui, .rd = A64_IP0, .rn = A64_IP0, .tkind = A64TargetKindSym, .target = s});
  Ins(_f, (MirInst){.op = A64OpBLR, .rn = A64_IP0});
}
static void AddrOf(MirFunc* _f, u32 _rd, const char* _name)
{
  u32 s = A64ModuleSym(&g_module, _name);
  Ins(_f, (MirInst){.op = A64OpADRP, .rd = (u8)_rd, .tkind = A64TargetKindSym, .target = s});
  Ins(_f, (MirInst){.op = A64OpADDXri, .rd = (u8)_rd, .rn = (u8)_rd, .tkind = A64TargetKindSym, .target = s});
}
static void Mov(MirFunc* _f, u32 _d, u32 _m)
{
  Ins(_f, Rr(A64OpMOVXr, _d, 0, _m));
}

static void EmitString(const char* _name, const char* _s)
{
  A64ModuleEmitData(&g_module, _name, true, A64DataKindRo, _s, (u32)strlen(_s) + 1, 1, nullptr, 0);
}

/* ---- functions ---------------------------------------------------------- */

/* sum_squares(n): frameless leaf. */
static void BuildSumSquares(void)
{
  MirFunc f;
  MirFuncInit(&f, "sum_squares", true, (FrameDesc){.frameless = true});
  SRC(&f, "static int sum_squares(int n) {   // frameless leaf: no unwind data");
  Ins(&f, (MirInst){.op = A64OpPseudoPrologue});
  u32 loop = MirFuncNewLabel(&f), done = MirFuncNewLabel(&f);
  SRC(&f, "    int acc = 0;");
  Ins(&f, Rr(A64OpMOVWr, 1, 0, A64_ZR));
  SRC(&f, "    for (; n != 0; n--) acc += n * n;");
  Ins(&f, (MirInst){.op = A64OpCBZW, .rd = 0, .tkind = A64TargetKindLabel, .target = done});
  MirFuncLabel(&f, loop);
  Ins(&f, (MirInst){.op = A64OpMADDW, .rd = 1, .rn = 0, .rm = 0, .ra = 1});
  Ins(&f, Ri(A64OpSUBSWri, 0, 0, 1));
  Ins(&f, Bcond(A64CondNe, loop));
  MirFuncLabel(&f, done);
  SRC(&f, "    return acc;");
  Ins(&f, Rr(A64OpMOVWr, 0, 0, 1));
  Ins(&f, (MirInst){.op = A64OpPseudoEpilogue});
  SRC(&f, "}");
  A64ModuleEmitFunction(&g_module, &f);
  MirFuncFree(&f);
}

/* big_frame(): 8 KiB of locals, so the prologue calls __chkstk. */
static void BuildBigFrame(void)
{
  MirFunc f;
  MirFuncInit(&f, "big_frame", true, (FrameDesc){.locals = 8192});
  SRC(&f, "static int big_frame(void) {       // 8 KiB frame: __chkstk probe");
  Ins(&f, (MirInst){.op = A64OpPseudoPrologue});
  SRC(&f, "    unsigned char buf[8192]; int sum = 0;");
  Ins(&f, (MirInst){.op = A64OpMOVXsp, .rd = 8, .rn = A64_SP});
  Ins(&f, (MirInst){.op = A64OpMOVZX, .rd = 10});
  Ins(&f, (MirInst){.op = A64OpMOVZW, .rd = 12});
  u32 l1 = MirFuncNewLabel(&f), l2 = MirFuncNewLabel(&f);
  SRC(&f, "    for (size_t i = 0; i < 8192; i++) buf[i] = (unsigned char)i;");
  MirFuncLabel(&f, l1);
  Ins(&f, (MirInst){.op = A64OpSTRBBro, .rd = 10, .rn = 8, .rm = 10, .shift = A64ExtendUxtx});
  Ins(&f, Ri(A64OpADDXri, 10, 10, 1));
  Ins(&f, (MirInst){.op = A64OpCMPXri, .rn = 10, .imm = 8192});
  Ins(&f, Bcond(A64CondLo, l1));
  SRC(&f, "    for (size_t i = 0; i < 8192; i++) sum += buf[i];");
  Ins(&f, (MirInst){.op = A64OpMOVZX, .rd = 10});
  MirFuncLabel(&f, l2);
  Ins(&f, (MirInst){.op = A64OpLDRBBro, .rd = 11, .rn = 8, .rm = 10, .shift = A64ExtendUxtx});
  Ins(&f, Rr(A64OpADDWrs, 12, 12, 11));
  Ins(&f, Ri(A64OpADDXri, 10, 10, 1));
  Ins(&f, (MirInst){.op = A64OpCMPXri, .rn = 10, .imm = 8192});
  Ins(&f, Bcond(A64CondLo, l2));
  SRC(&f, "    return sum;                       // 1044480");
  Ins(&f, Rr(A64OpMOVWr, 0, 0, 12));
  Ins(&f, (MirInst){.op = A64OpPseudoEpilogue});
  SRC(&f, "}");
  A64ModuleEmitFunction(&g_module, &f);
  MirFuncFree(&f);
}

/* sum_va(n, ...): variadic, homes x0-x7 next to the stack arguments. */
static void BuildSumVa(void)
{
  MirFunc f;
  MirFuncInit(&f, "sum_va", true, (FrameDesc){.homeArgs = true});
  FramePlan plan;
  FramePlanBuild(&f.frame, &plan);
  SRC(&f, "static long long sum_va(int n, ...) {   // variadic: x0-x7 homed");
  Ins(&f, (MirInst){.op = A64OpPseudoPrologue});
  SRC(&f, "    va_list ap; va_start(ap, n); long long s = 0;");
  Ins(&f, Ri(A64OpADDXri, 9, A64_SP, plan.homeSpOffset + 8));
  Ins(&f, Rr(A64OpMOVWr, 10, 0, 0));
  Ins(&f, Rr(A64OpMOVXr, 0, 0, A64_ZR));
  u32 loop = MirFuncNewLabel(&f), done = MirFuncNewLabel(&f);
  SRC(&f, "    while (n--) s += va_arg(ap, long long);");
  Ins(&f, (MirInst){.op = A64OpCBZW, .rd = 10, .tkind = A64TargetKindLabel, .target = done});
  MirFuncLabel(&f, loop);
  Ins(&f, Ri(A64OpLDRXui, 11, 9, 0));
  Ins(&f, Ri(A64OpADDXri, 9, 9, 8));
  Ins(&f, Rr(A64OpADDXrs, 0, 0, 11));
  Ins(&f, Ri(A64OpSUBSWri, 10, 10, 1));
  Ins(&f, Bcond(A64CondNe, loop));
  MirFuncLabel(&f, done);
  SRC(&f, "    return s;");
  Ins(&f, (MirInst){.op = A64OpPseudoEpilogue});
  SRC(&f, "}");
  A64ModuleEmitFunction(&g_module, &f);
  MirFuncFree(&f);
}

/* write_all(h, buf, len): WriteFile via the import address table. */
static void BuildWriteAll(void)
{
  MirFunc f;
  MirFuncInit(&f, "write_all", true, (FrameDesc){.locals = 16});
  FramePlan plan;
  FramePlanBuild(&f.frame, &plan);
  SRC(&f, "static void write_all(HANDLE h, const char *p, DWORD n) {");
  Ins(&f, (MirInst){.op = A64OpPseudoPrologue});
  SRC(&f, "    DWORD written; WriteFile(h, p, n, &written, NULL);   // __imp_WriteFile");
  Ins(&f, Ri(A64OpADDXri, 3, A64_SP, plan.localsSpOffset));
  Mov(&f, 4, A64_ZR);
  CallImport(&f, "WriteFile");
  Ins(&f, (MirInst){.op = A64OpPseudoEpilogue});
  SRC(&f, "}");
  A64ModuleEmitFunction(&g_module, &f);
  MirFuncFree(&f);
}

/* print_uint(h, v): decimal conversion into a local buffer. */
static void BuildPrintUint(void)
{
  MirFunc f;
  MirFuncInit(&f, "print_uint", true, (FrameDesc){.nGpr = 1, .locals = 32});
  FramePlan plan;
  FramePlanBuild(&f.frame, &plan);
  u32 bufEnd = plan.localsSpOffset + 32;
  SRC(&f, "static void print_uint(HANDLE h, unsigned v) {");
  Ins(&f, (MirInst){.op = A64OpPseudoPrologue});
  SRC(&f, "    char buf[32], *p = buf + 32; *--p = '\\n';");
  Mov(&f, 19, 0);
  Ins(&f, Ri(A64OpADDXri, 9, A64_SP, bufEnd));
  Ins(&f, Ri(A64OpSUBXri, 9, 9, 1));
  Ins(&f, (MirInst){.op = A64OpMOVZW, .rd = 10, .imm = '\n'});
  Ins(&f, Ri(A64OpSTRBBui, 10, 9, 0));
  Ins(&f, (MirInst){.op = A64OpMOVZW, .rd = 11, .imm = 10});
  u32 loop = MirFuncNewLabel(&f);
  SRC(&f, "    do { *--p = '0' + v % 10; v /= 10; } while (v);");
  MirFuncLabel(&f, loop);
  Ins(&f, Rr(A64OpUDIVW, 12, 1, 11));
  Ins(&f, (MirInst){.op = A64OpMSUBW, .rd = 13, .rn = 12, .rm = 11, .ra = 1});
  Ins(&f, Ri(A64OpADDWri, 13, 13, '0'));
  Ins(&f, Ri(A64OpSUBXri, 9, 9, 1));
  Ins(&f, Ri(A64OpSTRBBui, 13, 9, 0));
  Ins(&f, Rr(A64OpMOVWr, 1, 0, 12));
  Ins(&f, (MirInst){.op = A64OpCBNZW, .rd = 1, .tkind = A64TargetKindLabel, .target = loop});
  SRC(&f, "    write_all(h, p, (DWORD)(buf + 32 - p));");
  Ins(&f, Ri(A64OpADDXri, 10, A64_SP, bufEnd));
  Ins(&f, Rr(A64OpSUBXrs, 2, 10, 9));
  Mov(&f, 1, 9);
  Mov(&f, 0, 19);
  Call(&f, "write_all");
  Ins(&f, (MirInst){.op = A64OpPseudoEpilogue});
  SRC(&f, "}");
  A64ModuleEmitFunction(&g_module, &f);
  MirFuncFree(&f);
}

/* Prints a label string followed by the value in w1 (x19 holds the handle). */
static void Say(MirFunc* _f, const char* _sym, const char* _text)
{
  EmitString(_sym, _text);
  Mov(_f, 0, 19);
  AddrOf(_f, 1, _sym);
  MirFuncMovImm(_f, 2, strlen(_text), true);
  Call(_f, "write_all");
}

static void BuildP0Main(void)
{
  MirFunc f;
  MirFuncInit(&f, "p0_main", false, (FrameDesc){.nGpr = 2});
  SRC(&f, "int p0_main(void) {");
  Ins(&f, (MirInst){.op = A64OpPseudoPrologue});
  SRC(&f, "    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);        // __imp_GetStdHandle");
  MirFuncMovImm(&f, 0, (u32)-11, true);
  CallImport(&f, "GetStdHandle");
  Mov(&f, 19, 0);

  SRC(&f, "    puts(\"NeuronCompiler phase 0 ...\");");
  Say(&f, "msg_hello", "NeuronCompiler phase 0: hand-built ARM64, no CRT\n");

  SRC(&f, "    print(\"sum of squares 1..10 = \", sum_squares(10));     // 385");
  Say(&f, "msg_squares", "sum of squares 1..10 = ");
  MirFuncMovImm(&f, 0, 10, true);
  Call(&f, "sum_squares");
  Ins(&f, Rr(A64OpMOVWr, 1, 0, 0));
  Mov(&f, 0, 19);
  Call(&f, "print_uint");

  SRC(&f, "    print(\"8 KiB frame checksum = \", big_frame());         // 1044480");
  Say(&f, "msg_big", "8 KiB frame checksum = ");
  Call(&f, "big_frame");
  Ins(&f, Rr(A64OpMOVWr, 1, 0, 0));
  Mov(&f, 0, 19);
  Call(&f, "print_uint");

  SRC(&f, "    print(\"variadic sum(10, 20, 30) = \", sum_va(3, 10, 20, 30));   // 60");
  Say(&f, "msg_va", "variadic sum(10, 20, 30) = ");
  MirFuncMovImm(&f, 0, 3, false);
  MirFuncMovImm(&f, 1, 10, false);
  MirFuncMovImm(&f, 2, 20, false);
  MirFuncMovImm(&f, 3, 30, false);
  Call(&f, "sum_va");
  Ins(&f, Rr(A64OpMOVWr, 1, 0, 0));
  Mov(&f, 0, 19);
  Call(&f, "print_uint");

  SRC(&f, "    write_all(out, table[1], ...);          // pointer from an ADDR64 relocation");
  static const char OK[] = "data relocation ok\n";
  EmitString("msg_ok", OK);
  EmitString("msg_unused", "unused\n");
  u8 zeros[16] = {};
  A64DataReloc rel[2] = {{0, A64ModuleSym(&g_module, "msg_unused")}, {8, A64ModuleSym(&g_module, "msg_ok")}};
  A64ModuleEmitData(&g_module, "table", true, A64DataKindRw, zeros, 16, 8, rel, 2);
  AddrOf(&f, 8, "table");
  Ins(&f, Ri(A64OpLDRXui, 1, 8, 8));
  MirFuncMovImm(&f, 2, sizeof OK - 1, true);
  Mov(&f, 0, 19);
  Call(&f, "write_all");

  SRC(&f, "    return 0;");
  Ins(&f, (MirInst){.op = A64OpMOVZW, .rd = 0});
  Ins(&f, (MirInst){.op = A64OpPseudoEpilogue});
  SRC(&f, "}");
  A64ModuleEmitFunction(&g_module, &f);
  MirFuncFree(&f);
}

/* Entry point without a CRT: never returns, so it has no epilogue. */
static void BuildP0Start(void)
{
  MirFunc f;
  MirFuncInit(&f, "p0_start", false, (FrameDesc){});
  SRC(&f, "void p0_start(void) {                    // /ENTRY: no epilogue");
  Ins(&f, (MirInst){.op = A64OpPseudoPrologue});
  SRC(&f, "    ExitProcess(p0_main());                // ExitProcess via import thunk");
  Call(&f, "p0_main");
  Call(&f, "ExitProcess");
  Ins(&f, (MirInst){.op = A64OpBRK, .imm = 0xF000});
  SRC(&f, "}");
  A64ModuleEmitFunction(&g_module, &f);
  MirFuncFree(&f);
}

/* Our own stack probe for the no-CRT build: touches every page of the
 * requested allocation (x15 = size / 16) below sp. Preserves x15. */
static void BuildChkstk(void)
{
  MirFunc f;
  MirFuncInit(&f, "__chkstk", false, (FrameDesc){.frameless = true});
  SRC(&f, "void __chkstk(void) {                   // x15 = size/16; probes each page");
  Ins(&f, (MirInst){.op = A64OpPseudoPrologue});
  Ins(&f, (MirInst){.op = A64OpLSLXi, .rd = 16, .rn = 15, .amount = 4});
  Ins(&f, (MirInst){.op = A64OpMOVXsp, .rd = 17, .rn = A64_SP});
  u32 loop = MirFuncNewLabel(&f);
  MirFuncLabel(&f, loop);
  Ins(&f, Ri(A64OpSUBXri, 17, 17, 4096));
  Ins(&f, Ri(A64OpLDRXui, A64_ZR, 17, 0));
  Ins(&f, Ri(A64OpSUBSXri, 16, 16, 4096));
  Ins(&f, Bcond(A64CondGt, loop));
  Ins(&f, (MirInst){.op = A64OpPseudoEpilogue});
  SRC(&f, "}");
  A64ModuleEmitFunction(&g_module, &f);
  MirFuncFree(&f);
}

/* main() for the MSVC CRT: printf, TLS, the CRT's __chkstk. */
static void BuildCrtMain(void)
{
  A64ModuleEmitData(&g_module, "tls_counter", false, A64DataKindTls, (const u8[]){100, 0, 0, 0}, 4, 4, nullptr, 0);
  EmitString("fmt1", "%s from ncc phase 0: %d, thread-local counter = %d\n");
  EmitString("str_hello", "hello");
  EmitString("fmt2", "8 KiB frame checksum = %d, variadic sum = %lld\n");

  MirFunc f;
  MirFuncInit(&f, "main", false, (FrameDesc){.nGpr = 2});
  SRC(&f, "int main(void) {");
  Ins(&f, (MirInst){.op = A64OpPseudoPrologue});
  SRC(&f, "    int t = ++tls_counter;                // implicit TLS: x18 + 0x58, _tls_index");
  Ins(&f, Ri(A64OpLDRXui, 8, A64_X18, 0x58));
  u32 ti = A64ModuleSym(&g_module, "_tls_index");
  Ins(&f, (MirInst){.op = A64OpADRP, .rd = 9, .tkind = A64TargetKindSym, .target = ti});
  Ins(&f, (MirInst){.op = A64OpLDRWui, .rd = 9, .rn = 9, .tkind = A64TargetKindSym, .target = ti});
  Ins(&f, (MirInst){.op = A64OpLDRXro, .rd = 8, .rn = 8, .rm = 9, .shift = A64ExtendUxtx, .amount = 1});
  u32 tc = A64ModuleSym(&g_module, "tls_counter");
  Ins(&f, (MirInst){.op = A64OpADDXri, .rd = 8, .rn = 8, .tkind = A64TargetKindSym, .target = tc, .smod = A64SymModSecrelHi12});
  Ins(&f, (MirInst){.op = A64OpADDXri, .rd = 8, .rn = 8, .tkind = A64TargetKindSym, .target = tc, .smod = A64SymModSecrelLo12});
  Ins(&f, Ri(A64OpLDRWui, 3, 8, 0));
  Ins(&f, Ri(A64OpADDWri, 3, 3, 1));
  Ins(&f, Ri(A64OpSTRWui, 3, 8, 0));
  SRC(&f, "    printf(fmt1, \"hello\", 42, t);       // variadic call: all args in x0-x3");
  AddrOf(&f, 0, "fmt1");
  AddrOf(&f, 1, "str_hello");
  MirFuncMovImm(&f, 2, 42, true);
  Call(&f, "printf");
  SRC(&f, "    int c = big_frame();");
  Call(&f, "big_frame");
  Mov(&f, 19, 0);
  SRC(&f, "    long long s = sum_va(3, 10, 20, 30);");
  MirFuncMovImm(&f, 0, 3, false);
  MirFuncMovImm(&f, 1, 10, false);
  MirFuncMovImm(&f, 2, 20, false);
  MirFuncMovImm(&f, 3, 30, false);
  Call(&f, "sum_va");
  Mov(&f, 2, 0);
  SRC(&f, "    printf(fmt2, c, s);");
  Ins(&f, Rr(A64OpMOVWr, 1, 0, 19));
  AddrOf(&f, 0, "fmt2");
  Call(&f, "printf");
  SRC(&f, "    return 0;");
  Ins(&f, (MirInst){.op = A64OpMOVZW, .rd = 0});
  Ins(&f, (MirInst){.op = A64OpPseudoEpilogue});
  SRC(&f, "}");
  A64ModuleEmitFunction(&g_module, &f);
  MirFuncFree(&f);
}

/* ---- main --------------------------------------------------------------- */

static bool WriteBuf(const char* _path, const Buf* _b)
{
  if (!OsWriteFileAtomic(_path, _b->data, _b->len))
  {
    fprintf(stderr, "p0demo: cannot write %s\n", _path);
    return false;
  }
  return true;
}

int main(int argc, char** argv)
{
  if (argc != 3 || (strcmp(argv[1], "nocrt") != 0 && strcmp(argv[1], "crt") != 0))
  {
    fprintf(stderr, "usage: p0demo nocrt|crt out.obj\n");
    return 2;
  }
  bool crt = strcmp(argv[1], "crt") == 0;
  const char* out = argv[2];

  Arena arena;
  ArenaInit(&arena);
  char* srcPath = ArenaPrintf(&arena, "%s.pseudo.c", out);
  char* lstPath = ArenaPrintf(&arena, "%s.lst", out);
  A64ModuleInit(&g_module, &arena, out, srcPath, true);

  SRC(nullptr, "/* Pseudo-source for %s, generated by p0demo (%s mode). The machine code was", out, argv[1]);
  SRC(nullptr, " * built by hand; these lines exist so line tables have something to show. */");

  BuildSumSquares();
  BuildBigFrame();
  BuildSumVa();
  if (crt)
  {
    BuildCrtMain();
    A64ModuleDirective(&g_module, "/DEFAULTLIB:msvcrt");
    A64ModuleDirective(&g_module, "/DEFAULTLIB:oldnames");
    A64ModuleDirective(&g_module, "/DEFAULTLIB:legacy_stdio_definitions");
  }
  else
  {
    BuildWriteAll();
    BuildPrintUint();
    BuildP0Main();
    BuildP0Start();
    BuildChkstk();
    A64ModuleDirective(&g_module, "/DEFAULTLIB:kernel32");
  }

  Buf obj = {};
  A64ModuleFinish(&g_module, &obj);
  bool ok = WriteBuf(out, &obj) && WriteBuf(srcPath, &g_src) && WriteBuf(lstPath, &g_module.listing);
  if (ok)
    printf("p0demo: wrote %s (%u bytes): %u functions, %u packed unwind, %u xdata, "
           "all unwind data verified\n",
           out, obj.len, g_module.nfuncs, g_module.npacked, g_module.nxdata);
  BufFree(&obj);
  BufFree(&g_src);
  A64ModuleFree(&g_module);
  ArenaRelease(&arena);
  return ok ? 0 : 1;
}
