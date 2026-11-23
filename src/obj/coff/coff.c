/* coff.c — COFF object serialisation, regular and bigobj formats. */
#include "coff.h"

#include "../../core/crc32.h"

static const u8 bigobj_class_id[16] = {0xC7, 0xA1, 0xBA, 0xD1, 0xEE, 0xBA, 0xA9, 0x4B,
                                       0xAF, 0x20, 0xFA, 0xF6, 0x6A, 0xA4, 0xDC, 0xB8};

u32 coff_align_flags(u32 align) {
    NC_ASSERT(NC_IS_POW2(align) && align <= 8192);
    u32 log = nc_ctz64(align);
    return (log + 1) << 20;
}

void coff_init(CoffObj *o, Arena *arena) {
    memset(o, 0, sizeof *o);
    o->arena = arena;
    /* MSVC objects always carry @feat.00; we start with no feature bits. */
    coff_add_symbol(o, "@feat.00", -1, 0, 0, IMAGE_SYM_CLASS_STATIC);
}

void coff_free(CoffObj *o) {
    for (u32 i = 0; i < o->sections.len; i++) {
        buf_free(&o->sections.data[i].data);
        vec_free(&o->sections.data[i].relocs);
    }
    vec_free(&o->sections);
    vec_free(&o->symbols);
    buf_free(&o->directives);
}

CoffSection *coff_section(CoffObj *o, CoffSec s) {
    NC_ASSERT(s >= 1 && s <= o->sections.len);
    return &o->sections.data[s - 1];
}

CoffSec coff_add_section(CoffObj *o, const char *name, u32 characteristics, u32 align) {
    CoffSection *sec = vec_push_zero(&o->sections);
    sec->name = arena_strdup(o->arena, name);
    sec->characteristics = characteristics | coff_align_flags(align);
    CoffSec id = o->sections.len;
    CoffSym sym = coff_add_symbol(o, name, (i32)id, 0, 0, IMAGE_SYM_CLASS_STATIC);
    o->symbols.data[sym].is_section_sym = true;
    coff_section(o, id)->sec_sym = sym;
    return id;
}

void coff_set_comdat(CoffObj *o, CoffSec s, u8 selection, CoffSec assoc) {
    CoffSection *sec = coff_section(o, s);
    NC_ASSERT(selection >= IMAGE_COMDAT_SELECT_NODUPLICATES &&
              selection <= IMAGE_COMDAT_SELECT_LARGEST);
    NC_ASSERT((selection == IMAGE_COMDAT_SELECT_ASSOCIATIVE) == (assoc != 0));
    if (assoc) NC_ASSERT(coff_section(o, assoc)->comdat_sel != 0);
    sec->characteristics |= IMAGE_SCN_LNK_COMDAT;
    sec->comdat_sel = selection;
    sec->assoc = assoc;
}

CoffSym coff_add_symbol(CoffObj *o, const char *name, i32 section, u32 value, u16 type,
                        u8 storage_class) {
    CoffSymbol *s = vec_push_zero(&o->symbols);
    s->name = arena_strdup(o->arena, name);
    s->section = section;
    s->value = value;
    s->type = type;
    s->storage_class = storage_class;
    return o->symbols.len - 1;
}

CoffSym coff_symbol_named(CoffObj *o, const char *name) {
    /* Linear search is fine for phase-0 object sizes; the code generator keeps
     * its own symbol map and calls this once per symbol. */
    for (u32 i = 0; i < o->symbols.len; i++) {
        CoffSymbol *s = &o->symbols.data[i];
        if (!s->is_section_sym && strcmp(s->name, name) == 0) return i;
    }
    return coff_add_symbol(o, name, 0, 0, 0, IMAGE_SYM_CLASS_EXTERNAL);
}

void coff_define(CoffObj *o, CoffSym sym, CoffSec sec, u32 value, u16 type, u8 storage_class) {
    CoffSymbol *s = &o->symbols.data[sym];
    NC_ASSERT(s->section == 0 && !s->is_section_sym);
    s->section = (i32)sec;
    s->value = value;
    s->type = type;
    s->storage_class = storage_class;
}

CoffSym coff_section_symbol(CoffObj *o, CoffSec s) { return coff_section(o, s)->sec_sym; }

void coff_add_reloc(CoffObj *o, CoffSec s, u32 offset, CoffSym sym, u16 type) {
    CoffSection *sec = coff_section(o, s);
    NC_ASSERT(sym < o->symbols.len);
    vec_push(&sec->relocs, ((CoffReloc){offset, sym, type}));
}

void coff_add_directive(CoffObj *o, const char *directive) {
    buf_u8(&o->directives, ' ');
    buf_bytes(&o->directives, directive, (u32)strlen(directive));
}

/* ---- serialisation ------------------------------------------------------ */

typedef struct StrTab {
    Buf b;
} StrTab;

static u32 strtab_add(StrTab *t, const char *s) {
    if (t->b.len == 0) buf_u32(&t->b, 0); /* size, patched at the end */
    u32 off = t->b.len;
    buf_str0(&t->b, s);
    return off;
}

static void put_name8(Buf *out, StrTab *st, const char *name, bool section_header) {
    size_t n = strlen(name);
    u8 field[8] = {};
    if (n <= 8) {
        memcpy(field, name, n);
    } else if (section_header) {
        u32 off = strtab_add(st, name);
        char tmp[16];
        int len = 0;
        tmp[len++] = '/';
        char digits[12];
        int nd = 0;
        do { digits[nd++] = (char)('0' + off % 10); off /= 10; } while (off);
        if (nd > 7) nc_fatal("COFF string table too large for section names");
        while (nd) tmp[len++] = digits[--nd];
        memcpy(field, tmp, (size_t)len);
    } else {
        u32 off = strtab_add(st, name);
        field[4] = (u8)off; field[5] = (u8)(off >> 8); field[6] = (u8)(off >> 16);
        field[7] = (u8)(off >> 24);
    }
    buf_bytes(out, field, 8);
}

void coff_serialize(CoffObj *o, Buf *out) {
    /* The .drectve section is materialised last so directives can be added
     * at any time. */
    if (o->directives.len) {
        CoffSec d = coff_add_section(o, ".drectve", IMAGE_SCN_LNK_INFO | IMAGE_SCN_LNK_REMOVE, 1);
        buf_bytes(&coff_section(o, d)->data, o->directives.data, o->directives.len);
        buf_u8(&coff_section(o, d)->data, ' ');
    }

    u32 nsec = o->sections.len;
    bool big = o->force_bigobj || nsec > COFF_MAX_REGULAR_SECTIONS;
    u32 sym_size = big ? 20 : 18;

    /* Symbol file indices: section symbols first (1 aux each), then the rest. */
    u32 *file_index = nc_xrealloc(nullptr, sizeof(u32) * (o->symbols.len + 1));
    u32 next = 0;
    for (u32 s = 1; s <= nsec; s++) {
        file_index[coff_section(o, s)->sec_sym] = next;
        next += 2;
    }
    for (u32 i = 0; i < o->symbols.len; i++)
        if (!o->symbols.data[i].is_section_sym) file_index[i] = next++;
    u32 nsyms_file = next;

    u32 header_size = big ? 56 : 20;
    u32 pos = header_size + 40 * nsec;
    u32 *raw_ptr = nc_xrealloc(nullptr, sizeof(u32) * (nsec + 1));
    u32 *rel_ptr = nc_xrealloc(nullptr, sizeof(u32) * (nsec + 1));
    for (u32 s = 1; s <= nsec; s++) {
        CoffSection *sec = coff_section(o, s);
        bool bss = sec->characteristics & IMAGE_SCN_CNT_UNINITIALIZED_DATA;
        raw_ptr[s] = (!bss && sec->data.len) ? pos : 0;
        if (!bss) pos += sec->data.len;
        u32 nrel = sec->relocs.len + (sec->relocs.len > 0xFFFF ? 1 : 0);
        rel_ptr[s] = nrel ? pos : 0;
        pos += 10 * nrel;
    }
    u32 symtab_ptr = pos;

    StrTab st = {};
    buf_u32(&st.b, 0);
    out->len = 0;

    /* File header. */
    if (big) {
        buf_u16(out, 0);
        buf_u16(out, 0xFFFF);
        buf_u16(out, 2);
        buf_u16(out, IMAGE_FILE_MACHINE_ARM64);
        buf_u32(out, 0); /* TimeDateStamp: 0 for reproducibility */
        buf_bytes(out, bigobj_class_id, 16);
        buf_u32(out, 0); buf_u32(out, 0); buf_u32(out, 0); buf_u32(out, 0);
        buf_u32(out, nsec);
        buf_u32(out, symtab_ptr);
        buf_u32(out, nsyms_file);
    } else {
        buf_u16(out, IMAGE_FILE_MACHINE_ARM64);
        buf_u16(out, (u16)nsec);
        buf_u32(out, 0);
        buf_u32(out, symtab_ptr);
        buf_u32(out, nsyms_file);
        buf_u16(out, 0);
        buf_u16(out, 0);
    }

    /* Section headers. */
    for (u32 s = 1; s <= nsec; s++) {
        CoffSection *sec = coff_section(o, s);
        bool bss = sec->characteristics & IMAGE_SCN_CNT_UNINITIALIZED_DATA;
        u32 nrel = sec->relocs.len;
        u32 ch = sec->characteristics;
        if (nrel > 0xFFFF) ch |= IMAGE_SCN_LNK_NRELOC_OVFL;
        put_name8(out, &st, sec->name, true);
        buf_u32(out, 0);                                /* VirtualSize */
        buf_u32(out, 0);                                /* VirtualAddress */
        buf_u32(out, bss ? sec->bss_size : sec->data.len);
        buf_u32(out, raw_ptr[s]);
        buf_u32(out, rel_ptr[s]);
        buf_u32(out, 0);                                /* PointerToLinenumbers */
        buf_u16(out, (u16)(nrel > 0xFFFF ? 0xFFFF : nrel));
        buf_u16(out, 0);
        buf_u32(out, ch);
    }

    /* Raw data and relocations. */
    for (u32 s = 1; s <= nsec; s++) {
        CoffSection *sec = coff_section(o, s);
        bool bss = sec->characteristics & IMAGE_SCN_CNT_UNINITIALIZED_DATA;
        if (!bss) {
            NC_ASSERT(raw_ptr[s] == 0 || out->len == raw_ptr[s]);
            buf_bytes(out, sec->data.data, sec->data.len);
        }
        if (sec->relocs.len) {
            NC_ASSERT(out->len == rel_ptr[s]);
            if (sec->relocs.len > 0xFFFF) {
                buf_u32(out, sec->relocs.len + 1);
                buf_u32(out, 0);
                buf_u16(out, 0);
            }
            for (u32 r = 0; r < sec->relocs.len; r++) {
                CoffReloc *rl = &sec->relocs.data[r];
                buf_u32(out, rl->offset);
                buf_u32(out, file_index[rl->sym]);
                buf_u16(out, rl->type);
            }
        }
    }

    /* Symbol table. */
    NC_ASSERT(out->len == symtab_ptr);
    for (u32 s = 1; s <= nsec; s++) {
        CoffSection *sec = coff_section(o, s);
        bool bss = sec->characteristics & IMAGE_SCN_CNT_UNINITIALIZED_DATA;
        put_name8(out, &st, sec->name, false);
        buf_u32(out, 0);
        if (big) buf_u32(out, s); else buf_u16(out, (u16)s);
        buf_u16(out, 0);
        buf_u8(out, IMAGE_SYM_CLASS_STATIC);
        buf_u8(out, 1);
        /* Section-definition auxiliary record. */
        u32 len = bss ? sec->bss_size : sec->data.len;
        u32 crc = (sec->comdat_sel && !bss) ? nc_jamcrc(sec->data.data, sec->data.len) : 0;
        buf_u32(out, len);
        buf_u16(out, (u16)(sec->relocs.len > 0xFFFF ? 0xFFFF : sec->relocs.len));
        buf_u16(out, 0);
        buf_u32(out, crc);
        buf_u16(out, (u16)sec->assoc);
        buf_u8(out, sec->comdat_sel);
        if (big) {
            buf_u8(out, 0);
            buf_u16(out, (u16)(sec->assoc >> 16));
            buf_u16(out, 0);
        } else {
            buf_zeros(out, 3);
        }
    }
    for (u32 i = 0; i < o->symbols.len; i++) {
        CoffSymbol *sym = &o->symbols.data[i];
        if (sym->is_section_sym) continue;
        put_name8(out, &st, sym->name, false);
        buf_u32(out, sym->value);
        if (big) buf_u32(out, (u32)sym->section); else buf_u16(out, (u16)(i16)sym->section);
        buf_u16(out, sym->type);
        buf_u8(out, sym->storage_class);
        buf_u8(out, 0);
    }
    NC_ASSERT(out->len == symtab_ptr + nsyms_file * sym_size);

    /* String table. */
    buf_put_u32(&st.b, 0, st.b.len);
    buf_bytes(out, st.b.data, st.b.len);

    buf_free(&st.b);
    nc_xfree(file_index);
    nc_xfree(raw_ptr);
    nc_xfree(rel_ptr);
}
