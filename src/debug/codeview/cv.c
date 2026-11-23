/* cv.c — CodeView emission. Layouts follow cvinfo.h (microsoft-pdb). */
#include "cv.h"

static const u32 debug_sec_flags =
    IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_DISCARDABLE;

/* ---- record helpers ----------------------------------------------------- */

static u32 sym_begin(Buf *b, u16 kind) {
    u32 at = b->len;
    buf_u16(b, 0); /* length, patched */
    buf_u16(b, kind);
    return at;
}

static void sym_end(Buf *b, u32 at) {
    buf_align(b, 4, 0);
    buf_put_u16(b, at, (u16)(b->len - at - 2));
}

static u32 sub_begin(Buf *b, u32 kind) {
    buf_u32(b, kind);
    u32 at = b->len;
    buf_u32(b, 0);
    return at;
}

static void sub_end(Buf *b, u32 at) {
    buf_put_u32(b, at, b->len - at - 4);
    buf_align(b, 4, 0);
}

static u32 type_begin(CvModule *cv, u16 leaf) {
    u32 at = cv->types.len;
    buf_u16(&cv->types, 0);
    buf_u16(&cv->types, leaf);
    return at;
}

static u32 type_end(CvModule *cv, u32 at) {
    Buf *b = &cv->types;
    /* Type records are padded with LF_PAD bytes (0xF0 | bytes remaining). */
    u32 pad = (4 - (b->len & 3)) & 3;
    for (u32 i = pad; i > 0; i--) buf_u8(b, (u8)(0xF0 | i));
    buf_put_u16(b, at, (u16)(b->len - at - 2));
    return cv->next_type++;
}

static u32 string_add(CvModule *cv, const char *s) {
    u32 off = cv->strings.len;
    buf_str0(&cv->strings, s);
    return off;
}

/* ---- module ------------------------------------------------------------- */

void cv_init(CvModule *cv, Arena *arena, const char *obj_name, const char *source_name) {
    memset(cv, 0, sizeof *cv);
    cv->arena = arena;
    cv->obj_name = arena_strdup(arena, obj_name);
    cv->next_type = 0x1000;
    buf_u8(&cv->strings, 0); /* the string table starts with an empty string */

    /* One source file without a checksum (CHKSUM_TYPE_NONE) in phase 0; the
     * front end will record SHA-256 of the file as read. */
    cv->file_id = cv->checksums.len;
    buf_u32(&cv->checksums, string_add(cv, source_name));
    buf_u8(&cv->checksums, 0); /* checksum size */
    buf_u8(&cv->checksums, 0); /* CHKSUM_TYPE_NONE */
    buf_align(&cv->checksums, 4, 0);

    /* int (void) */
    u32 at = type_begin(cv, LF_ARGLIST);
    buf_u32(&cv->types, 0);
    u32 arglist = type_end(cv, at);
    at = type_begin(cv, LF_PROCEDURE);
    buf_u32(&cv->types, T_INT4);
    buf_u8(&cv->types, 0); /* CV_CALL_NEAR_C */
    buf_u8(&cv->types, 0); /* function attributes */
    buf_u16(&cv->types, 0); /* parameter count */
    buf_u32(&cv->types, arglist);
    cv->proc_type = type_end(cv, at);
}

void cv_free(CvModule *cv) {
    buf_free(&cv->types);
    buf_free(&cv->strings);
    buf_free(&cv->checksums);
}

void cv_emit_function(CvModule *cv, CoffObj *obj, const CvFunc *f) {
    u32 at = type_begin(cv, LF_FUNC_ID);
    buf_u32(&cv->types, 0); /* parent scope */
    buf_u32(&cv->types, cv->proc_type);
    buf_str0(&cv->types, f->name);
    u32 func_id = type_end(cv, at);

    CoffSec ds = coff_add_section(obj, ".debug$S", debug_sec_flags, 1);
    coff_set_comdat(obj, ds, IMAGE_COMDAT_SELECT_ASSOCIATIVE, f->text);
    Buf *b = &coff_section(obj, ds)->data;
    buf_u32(b, CV_SIGNATURE_C13);

    u32 sub = sub_begin(b, DEBUG_S_SYMBOLS);
    u32 rec = sym_begin(b, f->is_static ? S_LPROC32_ID : S_GPROC32_ID);
    buf_u32(b, 0); /* pParent */
    buf_u32(b, 0); /* pEnd */
    buf_u32(b, 0); /* pNext */
    buf_u32(b, f->length);
    buf_u32(b, f->prolog_end);
    buf_u32(b, f->epilog_start);
    buf_u32(b, func_id);
    coff_add_reloc(obj, ds, b->len, f->sym, IMAGE_REL_ARM64_SECREL);
    buf_u32(b, 0);
    coff_add_reloc(obj, ds, b->len, f->sym, IMAGE_REL_ARM64_SECTION);
    buf_u16(b, 0);
    buf_u8(b, 0); /* flags */
    buf_str0(b, f->name);
    sym_end(b, rec);

    rec = sym_begin(b, S_FRAMEPROC);
    buf_u32(b, f->frame_size);      /* cbFrame */
    buf_u32(b, 0);                  /* cbPad */
    buf_u32(b, 0);                  /* offPad */
    buf_u32(b, f->saved_regs_size); /* cbSaveRegs */
    buf_u32(b, 0);                  /* offExHdlr */
    buf_u16(b, 0);                  /* sectExHdlr */
    /* encodedLocalBasePointer and encodedParamBasePointer (bits 14-17):
     * 2 = frame pointer (x29). */
    buf_u32(b, 2u << 14 | 2u << 16);
    sym_end(b, rec);

    rec = sym_begin(b, S_PROC_ID_END);
    sym_end(b, rec);
    sub_end(b, sub);

    if (f->nlines) {
        sub = sub_begin(b, DEBUG_S_LINES);
        coff_add_reloc(obj, ds, b->len, f->sym, IMAGE_REL_ARM64_SECREL);
        buf_u32(b, 0); /* offCon */
        coff_add_reloc(obj, ds, b->len, f->sym, IMAGE_REL_ARM64_SECTION);
        buf_u16(b, 0); /* segCon */
        buf_u16(b, 0); /* flags: no columns */
        buf_u32(b, f->length);
        buf_u32(b, cv->file_id);
        buf_u32(b, f->nlines);
        buf_u32(b, 12 + 8 * f->nlines);
        for (u32 i = 0; i < f->nlines; i++) {
            buf_u32(b, f->lines[i].offset);
            buf_u32(b, (f->lines[i].line & 0xFFFFFF) | 0x80000000u); /* is_statement */
        }
        sub_end(b, sub);
    }
}

void cv_finish(CvModule *cv, CoffObj *obj) {
    CoffSec ds = coff_add_section(obj, ".debug$S", debug_sec_flags, 1);
    Buf *b = &coff_section(obj, ds)->data;
    buf_u32(b, CV_SIGNATURE_C13);

    u32 sub = sub_begin(b, DEBUG_S_SYMBOLS);
    u32 rec = sym_begin(b, S_OBJNAME);
    buf_u32(b, 0);
    buf_str0(b, cv->obj_name);
    sym_end(b, rec);

    rec = sym_begin(b, S_COMPILE3);
    buf_u32(b, CV_CFL_C);
    buf_u16(b, CV_CFL_ARM64);
    for (int i = 0; i < 4; i++) buf_u16(b, i == 1 ? 1 : 0); /* front-end version 0.1.0.0 */
    for (int i = 0; i < 4; i++) buf_u16(b, i == 1 ? 1 : 0); /* back-end version */
    buf_str0(b, "NeuronCompiler 0.1");
    sym_end(b, rec);
    sub_end(b, sub);

    sub = sub_begin(b, DEBUG_S_FILECHKSMS);
    buf_bytes(b, cv->checksums.data, cv->checksums.len);
    sub_end(b, sub);

    sub = sub_begin(b, DEBUG_S_STRINGTABLE);
    buf_bytes(b, cv->strings.data, cv->strings.len);
    sub_end(b, sub);

    CoffSec ts = coff_add_section(obj, ".debug$T", debug_sec_flags, 1);
    Buf *t = &coff_section(obj, ts)->data;
    buf_u32(t, CV_SIGNATURE_C13);
    buf_bytes(t, cv->types.data, cv->types.len);
}
