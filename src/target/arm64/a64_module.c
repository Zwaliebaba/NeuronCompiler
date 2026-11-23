/* a64_module.c — function finalisation and object emission. */
#include "a64_module.h"

#include <stdarg.h>
#include <stdio.h>

static const char *sym_name_cb(void *ctx, u32 sym) {
    A64Module *m = ctx;
    return sym < m->sym_names.len ? m->sym_names.data[sym] : "<badsym>";
}

void a64m_init(A64Module *m, Arena *arena, const char *obj_name, const char *source_name,
               bool debug) {
    memset(m, 0, sizeof *m);
    m->arena = arena;
    m->debug = debug;
    m->verify_unwind = true;
    coff_init(&m->obj, arena);
    if (debug) cv_init(&m->cv, arena, obj_name, source_name);
    m->chkstk_sym = a64m_sym(m, "__chkstk");
}

void a64m_free(A64Module *m) {
    coff_free(&m->obj);
    if (m->debug) cv_free(&m->cv);
    vec_free(&m->sym_names);
    vec_free(&m->sym_coff);
    buf_free(&m->listing);
}

/* Linear lookup is adequate for phase-0 object sizes; the front end will use
 * the interned-symbol hash map (Design/02 §3) instead. */
u32 a64m_sym(A64Module *m, const char *name) {
    for (u32 i = 0; i < m->sym_names.len; i++)
        if (strcmp(m->sym_names.data[i], name) == 0) return i;
    vec_push(&m->sym_names, arena_strdup(m->arena, name));
    vec_push(&m->sym_coff, coff_symbol_named(&m->obj, name));
    return m->sym_names.len - 1;
}

void a64m_directive(A64Module *m, const char *d) { coff_add_directive(&m->obj, d); }

static void listing_printf(A64Module *m, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) buf_bytes(&m->listing, tmp, (u32)NC_MIN((size_t)n, sizeof tmp - 1));
}

/* ---- branch relaxation -------------------------------------------------- */

static u32 inst_size(const FramePlan *p, const MInst *mi) {
    switch (mi->op) {
    case A64_P_LABEL: return 0;
    case A64_P_PROLOGUE: return 4 * frame_prologue_len(p);
    case A64_P_EPILOGUE: return 4 * frame_epilogue_len(p);
    default: return 4;
    }
}

static void compute_layout(const FramePlan *p, const MFunc *f, u32 *offs, u32 *label_off) {
    u32 pos = 0;
    for (u32 i = 0; i < f->insts.len; i++) {
        const MInst *mi = &f->insts.data[i];
        offs[i] = pos;
        if (mi->op == A64_P_LABEL) label_off[mi->target] = pos;
        pos += inst_size(p, mi);
    }
    offs[f->insts.len] = pos;
}

static bool is_cond_branch(u16 op) {
    if (op >= A64_NUM_REAL) return false;
    A64Form fm = a64_insn_info[op].form;
    return fm == F_BCOND || fm == F_CB || fm == F_TB;
}

static u16 invert_branch(u16 op) {
    switch (op) {
    case A64_Bcc: return A64_Bcc;
    case A64_CBZX: return A64_CBNZX;
    case A64_CBNZX: return A64_CBZX;
    case A64_CBZW: return A64_CBNZW;
    case A64_CBNZW: return A64_CBZW;
    case A64_TBZ: return A64_TBNZ;
    case A64_TBNZ: return A64_TBZ;
    }
    NC_UNREACHABLE();
}

/* Replaces out-of-range conditional branches by an inverted short branch
 * around an unconditional b. Offsets only grow, so this terminates. */
static void relax_branches(const FramePlan *p, MFunc *f) {
    for (;;) {
        u32 *offs = nc_xrealloc(nullptr, sizeof(u32) * (f->insts.len + 1));
        u32 *lab = nc_xrealloc(nullptr, sizeof(u32) * (f->nlabels + 1));
        compute_layout(p, f, offs, lab);
        i32 fix = -1;
        for (u32 i = 0; i < f->insts.len && fix < 0; i++) {
            MInst *mi = &f->insts.data[i];
            if (mi->tkind != TK_LABEL) continue;
            i64 d = (i64)lab[mi->target] - (i64)offs[i];
            i64 range = a64_branch_range((A64Op)mi->op);
            if (d >= -range && d < range) continue;
            if (!is_cond_branch(mi->op))
                nc_fatal("branch from %s+%u is out of range", f->name, offs[i]);
            fix = (i32)i;
        }
        nc_xfree(offs);
        nc_xfree(lab);
        if (fix < 0) return;

        MInst orig = f->insts.data[fix];
        u32 skip = f->nlabels++;
        MInst inv = orig;
        inv.op = invert_branch(orig.op);
        if (orig.op == A64_Bcc) inv.shift = a64_cond_invert((A64Cond)orig.shift);
        inv.target = skip;
        MInst far = {.op = A64_B, .tkind = TK_LABEL, .target = orig.target, .line = orig.line};
        MInst lbl = {.op = A64_P_LABEL, .target = skip, .line = orig.line};
        vec_reserve(&f->insts, f->insts.len + 2);
        memmove(&f->insts.data[fix + 3], &f->insts.data[fix + 1],
                sizeof(MInst) * (f->insts.len - (u32)fix - 1));
        f->insts.data[fix] = inv;
        f->insts.data[fix + 1] = far;
        f->insts.data[fix + 2] = lbl;
        f->insts.len += 2;
    }
}

/* ---- function emission -------------------------------------------------- */

void a64m_emit_function(A64Module *m, MFunc *f) {
    FramePlan plan;
    frame_plan(&f->frame, &plan);
    relax_branches(&plan, f);

    /* Expand pseudos. */
    MInstVec fin = {};
    VEC(u32) epilogs = {};
    u32 pos = 0;
    MInst seq[FRAME_MAX_STEPS + 1];
    for (u32 i = 0; i < f->insts.len; i++) {
        MInst mi = f->insts.data[i];
        if (mi.op == A64_P_PROLOGUE || mi.op == A64_P_EPILOGUE) {
            NC_ASSERT(mi.op != A64_P_PROLOGUE || pos == 0);
            if (mi.op == A64_P_EPILOGUE) vec_push(&epilogs, pos);
            u32 n = mi.op == A64_P_PROLOGUE ? frame_prologue(&plan, seq, m->chkstk_sym)
                                            : frame_epilogue(&plan, seq);
            for (u32 k = 0; k < n; k++) {
                seq[k].line = mi.line;
                vec_push(&fin, seq[k]);
            }
            pos += 4 * n;
        } else {
            vec_push(&fin, mi);
            if (mi.op != A64_P_LABEL) pos += 4;
        }
    }
    u32 func_bytes = pos;

    /* Label offsets. */
    u32 *lab = nc_xrealloc(nullptr, sizeof(u32) * (f->nlabels + 1));
    for (u32 i = 0, p = 0; i < fin.len; i++) {
        if (fin.data[i].op == A64_P_LABEL) lab[fin.data[i].target] = p;
        else p += 4;
    }

    /* Encode. */
    CoffSec text = coff_add_section(&m->obj, ".text$mn",
                                    IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ,
                                    4);
    coff_set_comdat(&m->obj, text, IMAGE_COMDAT_SELECT_NODUPLICATES, 0);
    u32 fsym = a64m_sym(m, f->name);
    CoffSym fcoff = m->sym_coff.data[fsym];
    coff_define(&m->obj, fcoff, text, 0, IMAGE_SYM_DTYPE_FUNCTION,
                f->is_static ? IMAGE_SYM_CLASS_STATIC : IMAGE_SYM_CLASS_EXTERNAL);
    Buf *code = &coff_section(&m->obj, text)->data;

    VEC(CvLine) lines = {};
    listing_printf(m, "\n%s:%s\n", f->name, f->is_static ? "  // static" : "");
    char text_buf[256];
    for (u32 i = 0, p = 0; i < fin.len; i++) {
        MInst *mi = &fin.data[i];
        if (mi->op == A64_P_LABEL) {
            listing_printf(m, ".L%u:\n", mi->target);
            continue;
        }
        i64 pcrel = 0;
        if (mi->tkind == TK_LABEL) pcrel = (i64)lab[mi->target] - (i64)p;
        else if (mi->tkind == TK_OFFSET) pcrel = mi->imm;
        u16 rtype;
        u32 w = a64_encode(mi, pcrel, &rtype);
        if (mi->tkind == TK_SYM) {
            NC_ASSERT(rtype != 0);
            coff_add_reloc(&m->obj, text, p, m->sym_coff.data[mi->target], rtype);
        }
        buf_u32(code, w);
        if (mi->line && (lines.len == 0 || vec_last(&lines)->line != mi->line))
            vec_push(&lines, ((CvLine){p, mi->line}));
        a64_format(text_buf, sizeof text_buf, mi, pcrel, sym_name_cb, m);
        listing_printf(m, "  %06x  %08x   %s\n", p, w, text_buf);
        p += 4;
    }
    NC_ASSERT(code->len == func_bytes);

    /* Unwind data. */
    UnwindRecord rec;
    unwind_build(&plan, func_bytes, epilogs.data, epilogs.len, &rec);
    if (m->verify_unwind) {
        char err[256];
        if (!unwind_verify(&plan, code->data, func_bytes, epilogs.data, epilogs.len, &rec, err,
                           sizeof err))
            nc_fatal("internal error: unwind verification failed for %s: %s", f->name, err);
    }
    m->nfuncs++;
    if (!rec.none) {
        /* Note: adding a section may reallocate the section table, so section
         * buffers are only looked up after all sections exist. */
        CoffSec pdata = coff_add_section(&m->obj, ".pdata",
                                         IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ, 4);
        coff_set_comdat(&m->obj, pdata, IMAGE_COMDAT_SELECT_ASSOCIATIVE, text);
        coff_add_reloc(&m->obj, pdata, 0, fcoff, IMAGE_REL_ARM64_ADDR32NB);
        u32 second = 0;
        if (rec.packed) {
            second = rec.packed_word;
            m->npacked++;
            listing_printf(m, "  // unwind: packed %08x\n", rec.packed_word);
        } else {
            CoffSec xdata = coff_add_section(
                &m->obj, ".xdata", IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ, 4);
            coff_set_comdat(&m->obj, xdata, IMAGE_COMDAT_SELECT_ASSOCIATIVE, text);
            buf_bytes(&coff_section(&m->obj, xdata)->data, rec.xdata.data, rec.xdata.len);
            char *uname = arena_printf(m->arena, "$unwind$%s", f->name);
            CoffSym usym =
                coff_add_symbol(&m->obj, uname, (i32)xdata, 0, 0, IMAGE_SYM_CLASS_STATIC);
            coff_add_reloc(&m->obj, pdata, 4, usym, IMAGE_REL_ARM64_ADDR32NB);
            m->nxdata++;
            listing_printf(m, "  // unwind: xdata, %u bytes, %u epilogue(s)\n", rec.xdata.len,
                           epilogs.len);
        }
        Buf *pd = &coff_section(&m->obj, pdata)->data;
        buf_u32(pd, 0);
        buf_u32(pd, second);
    } else {
        listing_printf(m, "  // unwind: none (frameless leaf)\n");
    }

    if (m->debug) {
        CvFunc cf = {
            .name = f->name, .is_static = f->is_static, .text = text, .sym = fcoff,
            .length = func_bytes, .prolog_end = 4 * frame_prologue_len(&plan),
            .epilog_start = epilogs.len ? epilogs.data[0] : func_bytes,
            .frame_size = plan.frame_size, .saved_regs_size = plan.save_size,
            .lines = lines.data, .nlines = lines.len,
        };
        cv_emit_function(&m->cv, &m->obj, &cf);
    }

    unwind_record_free(&rec);
    vec_free(&lines);
    vec_free(&epilogs);
    vec_free(&fin);
    nc_xfree(lab);
}

/* ---- data --------------------------------------------------------------- */

void a64m_emit_data(A64Module *m, const char *name, bool is_static, A64DataKind kind,
                    const void *init, u32 size, u32 align, const A64DataReloc *relocs,
                    u32 nrelocs) {
    static const char *const names[] = {".data", ".rdata", ".bss", ".tls$"};
    u32 ch = IMAGE_SCN_MEM_READ;
    switch (kind) {
    case DATA_RW: ch |= IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_WRITE; break;
    case DATA_RO: ch |= IMAGE_SCN_CNT_INITIALIZED_DATA; break;
    case DATA_BSS: ch |= IMAGE_SCN_CNT_UNINITIALIZED_DATA | IMAGE_SCN_MEM_WRITE; break;
    case DATA_TLS: ch |= IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_WRITE; break;
    }
    CoffSec s = coff_add_section(&m->obj, names[kind], ch, align);
    /* One section per object (/Gw, Q-18) so /OPT:REF can drop unused data. */
    coff_set_comdat(&m->obj, s, IMAGE_COMDAT_SELECT_NODUPLICATES, 0);
    CoffSection *sec = coff_section(&m->obj, s);
    if (kind == DATA_BSS) {
        NC_ASSERT(!init && !nrelocs);
        sec->bss_size = size;
    } else if (init) {
        buf_bytes(&sec->data, init, size);
    } else {
        buf_zeros(&sec->data, size);
    }
    u32 sym = a64m_sym(m, name);
    coff_define(&m->obj, m->sym_coff.data[sym], s, 0, 0,
                is_static ? IMAGE_SYM_CLASS_STATIC : IMAGE_SYM_CLASS_EXTERNAL);
    for (u32 i = 0; i < nrelocs; i++) {
        NC_ASSERT(relocs[i].offset + 8 <= size);
        coff_add_reloc(&m->obj, s, relocs[i].offset, m->sym_coff.data[relocs[i].sym],
                       IMAGE_REL_ARM64_ADDR64);
    }
    listing_printf(m, "\n%s:  // %s, %u bytes, align %u\n", name, names[kind], size, align);
}

void a64m_finish(A64Module *m, Buf *out) {
    if (m->debug) cv_finish(&m->cv, &m->obj);
    coff_serialize(&m->obj, out);
}

/* ---- builder helpers ---------------------------------------------------- */

void mf_init(MFunc *f, const char *name, bool is_static, FrameDesc frame) {
    memset(f, 0, sizeof *f);
    f->name = name;
    f->is_static = is_static;
    f->frame = frame;
}

void mf_free(MFunc *f) { vec_free(&f->insts); }

u32 mf_new_label(MFunc *f) { return f->nlabels++; }

void mf_emit(MFunc *f, MInst mi) {
    if (!mi.line) mi.line = f->cur_line;
    vec_push(&f->insts, mi);
}

void mf_label(MFunc *f, u32 label) {
    mf_emit(f, (MInst){.op = A64_P_LABEL, .target = label});
}

void mf_line(MFunc *f, u32 line) { f->cur_line = line; }

void mf_mov_imm(MFunc *f, u32 rd, u64 value, bool w) {
    u32 width = w ? 32 : 64, chunks = width / 16, zeros = 0, ones = 0;
    if (w) value &= 0xFFFFFFFFu;
    for (u32 i = 0; i < chunks; i++) {
        u32 c = (u32)(value >> (16 * i)) & 0xFFFF;
        zeros += c == 0;
        ones += c == 0xFFFF;
    }
    bool inverted = ones > zeros;
    u32 fill = inverted ? 0xFFFF : 0;
    u32 needed = chunks - (inverted ? ones : zeros); /* chunks that need an instruction */
    u32 n, immr, imms;
    if (needed > 1 && a64_encode_logical_imm(value, width, &n, &immr, &imms)) {
        mf_emit(f, (MInst){.op = w ? A64_ORRWri : A64_ORRXri, .rd = (u8)rd, .rn = A64_ZR,
                           .imm = (i64)value});
        return;
    }
    bool first = true;
    for (u32 i = 0; i < chunks; i++) {
        u32 c = (u32)(value >> (16 * i)) & 0xFFFF;
        bool last_chance = first && i == chunks - 1; /* every chunk equals the fill */
        if (c == fill && !last_chance) continue;
        if (first) {
            A64Op op = inverted ? (w ? A64_MOVNW : A64_MOVNX) : (w ? A64_MOVZW : A64_MOVZX);
            u32 imm16 = inverted ? (~c & 0xFFFF) : c;
            u32 sh = last_chance && c == fill ? 0 : 16 * i;
            mf_emit(f, (MInst){.op = op, .rd = (u8)rd, .imm = imm16, .amount = (u8)sh});
            first = false;
        } else {
            mf_emit(f, (MInst){.op = w ? A64_MOVKW : A64_MOVKX, .rd = (u8)rd, .imm = c,
                               .amount = (u8)(16 * i)});
        }
    }
}
