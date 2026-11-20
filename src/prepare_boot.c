// SPDX-License-Identifier: GPL-2.0-or-later
// On-console preparation: build the BBPROBE5 image from the dump, so installing the package and
// pointing it at E:\ is enough. Port of scripts/prepare.py and scripts/link_modules.py; the output
// is meant to match the Python pipeline byte for byte (tests compare hashes).
#include "prepare_boot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) && !defined(BB_PREPARE_STANDALONE)
// AppContainer-safe open: FileApiFromApp, the same wrapper the host uses for the external dump.
#include "runtime_compat.h"
#define bb_open(path, mode) uncharted_uwp_fopen(path, mode)
#else
#define bb_open(path, mode) fopen(path, mode)
#endif

#if defined(_WIN32)
#include <direct.h>
#define bb_mkdir(path) _mkdir(path)
#else
#include <sys/stat.h>
#define bb_mkdir(path) mkdir(path, 0777)
#endif

#define PROBE_LIMIT (512ull << 20)
#define NAME_LEN 128
#define MAX_SYMS 131072
#define MAX_RELOCS 262144
#define MAX_NAMES 16384
#define MAX_EXPORTS 32768
#define MAX_BINDINGS 16384
#define MAX_MODULES 8
#define MAX_SEGMENTS 128

typedef unsigned char u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef struct {
    u32 type, flags;
    u64 offset, vaddr, paddr, filesz, memsz, align;
} ph_t;

typedef struct {
    u8* data;
    size_t size;
    ph_t* ph;
    u32 nph;
    u64 entry;
} elf_t;

typedef struct {
    u64 tag, value;
} dyn_t;

typedef struct {
    char name[160];
    u8 type, binding;
    u16 section;
    u64 value, size;
    int has_identity;
    char nid[24];
    char lib[64];
    char mod[64];
    u16 lib_id, mod_id;
} sym_t;

typedef struct {
    u64 target, kind, sym;
    int64_t addend;
} rel_t;

typedef struct {
    elf_t elf;
    dyn_t dyn[512];
    u32 ndyn;
    sym_t* syms;
    u32 nsyms;
    rel_t* rels;
    u32 nrels;
} module_t;

typedef struct {
    char nid[96];
    char lib[64];
    char mod[64];
    u16 lib_id, mod_id;
    int raw; /* no library identity: the name is used verbatim */
} ident_t;

typedef struct {
    ident_t id;
    u64 value, kind;
} export_t;

typedef struct {
    u32 index;
    u64 value, kind;
} binding_t;

typedef struct {
    u64 vaddr, memsz, flags;
} seg_t;

static u16 rd16(const u8* p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 rd32(const u8* p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static u64 rd64(const u8* p) { return (u64)rd32(p) | ((u64)rd32(p + 4) << 32); }
static void wr64(u8* p, u64 v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = (u8)(v >> (8 * i));
    }
}
static int in_range(size_t total, u64 offset, u64 size) {
    return offset <= total && size <= total - offset;
}

static u8* read_file(const char* path, size_t* size, char* err, size_t err_len) {
    FILE* file = bb_open(path, "rb");
    if (!file) {
        snprintf(err, err_len, "cannot open %s", path);
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        snprintf(err, err_len, "cannot seek %s", path);
        return NULL;
    }
    const long length = ftell(file);
    if (length <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        snprintf(err, err_len, "cannot size %s", path);
        return NULL;
    }
    u8* data = (u8*)malloc((size_t)length);
    if (!data) {
        fclose(file);
        snprintf(err, err_len, "out of memory reading %s", path);
        return NULL;
    }
    if (fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        snprintf(err, err_len, "short read on %s", path);
        return NULL;
    }
    fclose(file);
    *size = (size_t)length;
    return data;
}

// Reads the ELF header that starts at `base` inside `data` and allocates the flat image the probe
// consumes (segment contents at their virtual offsets).
static int parse_elf(const u8* data, size_t total, size_t base, elf_t* out, char* err,
                     size_t err_len) {
    if (!in_range(total, base, 64)) {
        snprintf(err, err_len, "ELF header out of bounds");
        return -1;
    }
    const u8* h = data + base;
    if (memcmp(h, "\x7f" "ELF\x02\x01\x01", 7) != 0 || rd16(h + 18) != 62) {
        snprintf(err, err_len, "expected little-endian x86-64 ELF");
        return -1;
    }
    out->entry = rd64(h + 24);
    const u64 phoff = rd64(h + 32);
    const u16 phentsize = rd16(h + 54);
    const u16 phnum = rd16(h + 56);
    if (phentsize != 56 || phnum == 0 || phnum >= 256) {
        snprintf(err, err_len, "unsupported program headers");
        return -1;
    }
    if (!in_range(total, base + phoff, (u64)phnum * 56)) {
        snprintf(err, err_len, "program header table out of bounds");
        return -1;
    }
    out->ph = (ph_t*)calloc(phnum, sizeof(ph_t));
    if (!out->ph) {
        snprintf(err, err_len, "out of memory for program headers");
        return -1;
    }
    out->nph = phnum;
    u64 end = 0;
    for (u32 i = 0; i < phnum; ++i) {
        const u8* p = data + base + phoff + (u64)i * 56;
        ph_t* ph = &out->ph[i];
        ph->type = rd32(p);
        ph->flags = rd32(p + 4);
        ph->offset = rd64(p + 8);
        ph->vaddr = rd64(p + 16);
        ph->paddr = rd64(p + 24);
        ph->filesz = rd64(p + 32);
        ph->memsz = rd64(p + 40);
        ph->align = rd64(p + 48);
        if (ph->offset + ph->filesz > end) {
            end = ph->offset + ph->filesz;
        }
    }
    if (end > PROBE_LIMIT) {
        snprintf(err, err_len, "probe image exceeds the 512 MiB limit");
        return -1;
    }
    out->data = (u8*)calloc(1, (size_t)end);
    if (!out->data) {
        snprintf(err, err_len, "out of memory for the %llu-byte image",
                 (unsigned long long)end);
        return -1;
    }
    out->size = (size_t)end;
    const u64 header_end = phoff + (u64)phnum * 56;
    if (!in_range(total, base, header_end) || header_end > out->size) {
        snprintf(err, err_len, "ELF header region out of bounds");
        return -1;
    }
    memcpy(out->data, data + base, (size_t)header_end);
    return 0;
}

static int parse_self(const char* path, elf_t* out, char* err, size_t err_len) {
    size_t total = 0;
    u8* data = read_file(path, &total, err, err_len);
    if (!data) {
        return -1;
    }
    // Byte ranges that carry real content: the ELF header table, plus every SELF segment. A program
    // header counts as present when its whole [offset, offset+filesz) falls inside one of them.
    struct {
        u64 start, end;
    } covered[64];
    int ncovered = 0;
    int failed = 0;

    if (total >= 4 && memcmp(data, "ELF", 3) == 0) {
        if (parse_elf(data, total, 0, out, err, err_len) != 0) {
            free(data);
            return -1;
        }
        for (u32 i = 0; i < out->nph && !failed; ++i) {
            ph_t* ph = &out->ph[i];
            if (!ph->filesz) {
                continue;
            }
            if (!in_range(total, ph->offset, ph->filesz) ||
                !in_range(out->size, ph->offset, ph->filesz)) {
                const int required = ph->type == 1 || ph->type == 2 ||
                                     ph->type == 0x61000000 || ph->type == 0x61000010;
                if (required) {
                    snprintf(err, err_len,
                             "required segment %u is unavailable (type=0x%x offset=0x%llx)", i,
                             ph->type, (unsigned long long)ph->offset);
                    failed = 1;
                }
                continue;
            }
            memcpy(out->data + ph->offset, data + ph->offset, (size_t)ph->filesz);
        }
        (void)covered;
        (void)ncovered;
        free(data);
        return failed ? -1 : 0;
    }

    if (total < 32 || data[0] != 0x4F || data[1] != 0x15 || data[2] != 0x3D) {
        snprintf(err, err_len, "expected PS4 SELF or ELF64: %s", path);
        free(data);
        return -1;
    }
    const u16 count = rd16(data + 24);
    const size_t base = 32 + (size_t)count * 32;
    if (parse_elf(data, total, base, out, err, err_len) != 0) {
        free(data);
        return -1;
    }
    covered[ncovered].start = 0;
    covered[ncovered].end = rd64(data + base + 32) + (u64)out->nph * 56;
    ncovered++;

    for (u32 i = 0; i < count && !failed; ++i) {
        const u8* s = data + 32 + (size_t)i * 32;
        const u64 flags = rd64(s);
        const u64 offset = rd64(s + 8);
        const u64 size = rd64(s + 16);
        const u64 mem = rd64(s + 24);
        if (!(flags & 0x800)) {
            continue;
        }
        if (flags & 10) {
            snprintf(err, err_len, "encrypted/compressed SELF segment is unsupported");
            failed = 1;
            break;
        }
        const u32 index = (u32)((flags >> 20) & 4095);
        if (index >= out->nph) {
            snprintf(err, err_len, "invalid SELF segment index");
            failed = 1;
            break;
        }
        const ph_t* ph = &out->ph[index];
        if (size != ph->filesz || size != mem || !in_range(total, offset, size) ||
            !in_range(out->size, ph->offset, size)) {
            snprintf(err, err_len, "unsupported blocked segment layout");
            failed = 1;
            break;
        }
        memcpy(out->data + ph->offset, data + offset, (size_t)size);
        if (ncovered < 64) {
            covered[ncovered].start = ph->offset;
            covered[ncovered].end = ph->offset + size;
            ncovered++;
        }
    }
    for (u32 i = 0; i < out->nph && !failed; ++i) {
        const ph_t* ph = &out->ph[i];
        if (!ph->filesz) {
            continue;
        }
        int present = 0;
        for (int c = 0; c < ncovered && !present; ++c) {
            present = covered[c].start <= ph->offset && ph->offset + ph->filesz <= covered[c].end;
        }
        if (present) {
            continue;
        }
        const int required =
            ph->type == 1 || ph->type == 2 || ph->type == 0x61000000 || ph->type == 0x61000010;
        if (required) {
            snprintf(err, err_len,
                     "required segment %u is unavailable (type=0x%x offset=0x%llx filesz=%llu)", i,
                     ph->type, (unsigned long long)ph->offset,
                     (unsigned long long)ph->filesz);
            failed = 1;
        }
    }
    free(data);
    return failed ? -1 : 0;
}

static const ph_t* find_ph(const elf_t* elf, u32 type) {
    for (u32 i = 0; i < elf->nph; ++i) {
        if (elf->ph[i].type == type) {
            return &elf->ph[i];
        }
    }
    return NULL;
}

static int load_dynamic(module_t* m, char* err, size_t err_len) {
    const ph_t* dyn = find_ph(&m->elf, 2);
    if (!dyn) {
        snprintf(err, err_len, "no dynamic segment");
        return -1;
    }
    m->ndyn = 0;
    for (u64 pos = 0; pos + 16 <= dyn->filesz && m->ndyn < 512; pos += 16) {
        const u64 tag = rd64(m->elf.data + dyn->offset + pos);
        const u64 value = rd64(m->elf.data + dyn->offset + pos + 8);
        if (tag == 0) {
            break;
        }
        m->dyn[m->ndyn].tag = tag;
        m->dyn[m->ndyn].value = value;
        m->ndyn++;
    }
    return 0;
}

static u64 dyn_value(const module_t* m, u64 tag, u64 fallback) {
    for (u32 i = 0; i < m->ndyn; ++i) {
        if (m->dyn[i].tag == tag) {
            return m->dyn[i].value;
        }
    }
    return fallback;
}

static const char* blob_string(const u8* strings, u64 strings_size, u64 offset) {
    if (offset >= strings_size) {
        return "";
    }
    const u8* at = strings + offset;
    const u8* nul = (const u8*)memchr(at, 0, (size_t)(strings_size - offset));
    return nul ? (const char*)at : "";
}

static void copy_field(char* dst, size_t cap, const char* src) {
    size_t length = strlen(src);
    if (length >= cap) {
        length = cap - 1;
    }
    memcpy(dst, src, length);
    dst[length] = '\0';
}

static char id_alphabet(int value) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    return alphabet[value & 63];
}

// encode_id: base-64 over the 16-bit library/module code, most significant digit first.
static void encode_id(u64 value, char* out, size_t cap) {
    char tmp[16];
    int n = 0;
    tmp[n++] = id_alphabet((int)(value & 63));
    value >>= 6;
    while (value && n < (int)sizeof(tmp)) {
        tmp[n++] = id_alphabet((int)(value & 63));
        value >>= 6;
    }
    size_t at = 0;
    while (n > 0 && at + 1 < cap) {
        out[at++] = tmp[--n];
    }
    out[at] = '\0';
}

static int ident_from_name(const module_t* m, const char* name, ident_t* id) {
    const char* first = strchr(name, '#');
    if (!first) {
        return 0;
    }
    const char* second = strchr(first + 1, '#');
    if (!second) {
        return 0;
    }
    char lib_key[16], mod_key[16];
    const size_t n = (size_t)(second - first - 1);
    if (n == 0 || n >= sizeof(lib_key)) {
        return 0;
    }
    memcpy(lib_key, first + 1, n);
    lib_key[n] = '\0';
    copy_field(mod_key, sizeof(mod_key), second + 1);
    int found_lib = 0, found_mod = 0;
    const ph_t* lib_seg = find_ph(&m->elf, 0x61000000);
    const u64 blob = lib_seg ? lib_seg->offset : 0;
    for (u32 i = 0; i < m->ndyn; ++i) {
        const u64 tag = m->dyn[i].tag;
        if (tag != 0x61000013 && tag != 0x61000015 && tag != 0x6100000d && tag != 0x6100000f) {
            continue;
        }
        char key[16];
        encode_id(m->dyn[i].value >> 48, key, sizeof(key));
        const int is_lib = tag == 0x61000013 || tag == 0x61000015;
        if ((is_lib && strcmp(key, lib_key) != 0) || (!is_lib && strcmp(key, mod_key) != 0)) {
            continue;
        }
        const u64 name_offset = m->dyn[i].value & 0xffffffffull;
        const u64 strings = dyn_value(m, 0x61000035, 0);
        const u64 strings_len = dyn_value(m, 0x61000037, 0);
        const char* text =
            blob_string(m->elf.data + blob + strings, strings_len, name_offset);
        const u64 code = (m->dyn[i].value >> 32) & 65535;
        if (is_lib) {
            copy_field(id->lib, sizeof(id->lib), text);
            id->lib_id = (u16)code;
            found_lib = 1;
        } else {
            copy_field(id->mod, sizeof(id->mod), text);
            id->mod_id = (u16)code;
            found_mod = 1;
        }
    }
    if (!found_lib || !found_mod) {
        return 0;
    }
    copy_field(id->nid, sizeof(id->nid), name);
    char* hash = strchr(id->nid, '#');
    if (hash) {
        *hash = '\0';
    }
    return 1;
}

typedef struct {
    const char* lib;
    const char* lib_code;
    const char* mod_code;
} spelling_t;

static const spelling_t kSpelling[] = {
    {"libSceAjm", "N", "O"},
    {"libSceAppContent", "c", "d"},
    {"libSceAudioIn", "Y", "Z"},
    {"libSceAudioOut", "G", "H"},
    {"libSceAvPlayer", "m", "n"},
    {"libSceCommonDialog", "Z", "a"},
    {"libSceFios2", "F", "G"},
    {"libSceGameLiveStreaming", "e", "f"},
    {"libSceGnmDriver", "B", "C"},
    {"libSceHttp", "K", "L"},
    {"libSceImeDialog", "b", "c"},
    {"libSceMouse", "U", "V"},
    {"libSceMsgDialog", "j", "k"},
    {"libSceNet", "E", "F"},
    {"libSceNetCtl", "J", "K"},
    {"libSceNpAuth", "X", "Y"},
    {"libSceNpCommerce", "k", "l"},
    {"libSceNpCommon", "Q", "R"},
    {"libSceNpManager", "R", "S"},
    {"libSceNpMatching2", "P", "Q"},
    {"libSceNpProfileDialog", "h", "i"},
    {"libSceNpScore", "o", "p"},
    {"libSceNpSignaling", "T", "U"},
    {"libSceNpTrophy", "l", "m"},
    {"libSceNpUtility", "S", "T"},
    {"libSceNpWebApi", "W", "X"},
    {"libScePad", "A", "B"},
    {"libScePlayGo", "n", "o"},
    {"libScePosix", "I", "J"},
    {"libSceRtc", "H", "I"},
    {"libSceSaveData", "O", "P"},
    {"libSceSaveDataDialog", "d", "e"},
    {"libSceScreenShot", "g", "h"},
    {"libSceSsl", "L", "M"},
    {"libSceSysmodule", "M", "N"},
    {"libSceSystemService", "V", "W"},
    {"libSceUserService", "D", "E"},
    {"libSceVideoOut", "C", "D"},
    {"libSceVideoRecording", "f", "g"},
    {"libSceVoice", "i", "j"},
    {"libSceWebBrowserDialog", "a", "b"},
    {"libc", "q", "q"},
    {"libkernel", "p", "J"},
};

static const char* runtime_module_for(const char* lib) {
    if (strcmp(lib, "libSceAppContent") == 0) {
        return "libSceAppContentUtil";
    }
    if (strcmp(lib, "libScePosix") == 0) {
        return "libkernel";
    }
    return lib;
}

static void runtime_name(const ident_t* id, char* out, size_t cap) {
    if (id->raw) {
        copy_field(out, cap, id->nid);
        return;
    }
    const char* expected_module = runtime_module_for(id->lib);
    for (size_t i = 0; i < sizeof(kSpelling) / sizeof(kSpelling[0]); ++i) {
        if (strcmp(kSpelling[i].lib, id->lib) == 0 && strcmp(expected_module, id->mod) == 0) {
            snprintf(out, cap, "%s#%s#%s", id->nid, kSpelling[i].lib_code, kSpelling[i].mod_code);
            return;
        }
    }
    snprintf(out, cap, "%s#%s", id->nid, id->lib);
}

static int load_module(const char* path, module_t* m, char* err, size_t err_len) {
    memset(m, 0, sizeof(*m));
    if (parse_self(path, &m->elf, err, err_len) != 0) {
        return -1;
    }
    if (load_dynamic(m, err, err_len) != 0) {
        return -1;
    }
    const ph_t* lib = find_ph(&m->elf, 0x61000000);
    if (!lib) {
        snprintf(err, err_len, "%s: no SCE library segment", path);
        return -1;
    }
    const u64 blob = lib->offset;
    const u64 blob_size = lib->filesz;
    const u64 strings_at = dyn_value(m, 0x61000035, 0);
    const u64 strings_len = dyn_value(m, 0x61000037, 0);
    const u64 syms_at = dyn_value(m, 0x61000039, 0);
    const u64 syms_len = dyn_value(m, 0x6100003f, 0);
    if (!in_range(blob_size, strings_at, strings_len) ||
        !in_range(blob_size, syms_at, syms_len)) {
        snprintf(err, err_len, "%s: malformed symbol tables", path);
        return -1;
    }
    const u8* strings = m->elf.data + blob + strings_at;
    const u64 strings_size = strings_len;
    const u8* syms = m->elf.data + blob + syms_at;
    m->nsyms = (u32)(syms_len / 24);
    if (m->nsyms == 0 || m->nsyms > MAX_SYMS) {
        snprintf(err, err_len, "%s: unsupported symbol count", path);
        return -1;
    }
    m->syms = (sym_t*)calloc(m->nsyms, sizeof(sym_t));
    if (!m->syms) {
        snprintf(err, err_len, "out of memory for %s symbols", path);
        return -1;
    }
    for (u32 i = 0; i < m->nsyms; ++i) {
        const u8* s = syms + (size_t)i * 24;
        sym_t* sym = &m->syms[i];
        const char* name = blob_string(strings, strings_size, rd32(s));
        copy_field(sym->name, sizeof(sym->name), name);
        sym->type = (u8)(rd16(s + 4) & 15);
        sym->binding = (u8)(rd16(s + 4) >> 4);
        sym->section = rd16(s + 6);
        sym->value = rd64(s + 8);
        sym->size = rd64(s + 16);
        if (sym->name[0] != '\0') {
            ident_t id = {0};
            sym->has_identity = ident_from_name(m, sym->name, &id);
            if (sym->has_identity) {
                copy_field(sym->nid, sizeof(sym->nid), id.nid);
                copy_field(sym->lib, sizeof(sym->lib), id.lib);
                copy_field(sym->mod, sizeof(sym->mod), id.mod);
                sym->lib_id = id.lib_id;
                sym->mod_id = id.mod_id;
            }
        }
    }
    m->nrels = 0;
    const u64 pairs[2][2] = {{0x61000029, 0x6100002d}, {0x6100002f, 0x61000031}};
    for (int pair = 0; pair < 2; ++pair) {
        const u64 at = dyn_value(m, pairs[pair][0], 0);
        const u64 len = dyn_value(m, pairs[pair][1], 0);
        if (!in_range(blob_size, at, len)) {
            snprintf(err, err_len, "%s: malformed relocation table", path);
            return -1;
        }
        const u32 count = (u32)(len / 24);
        if (m->nrels + count > MAX_RELOCS) {
            snprintf(err, err_len, "%s: too many relocations", path);
            return -1;
        }
        m->rels = (rel_t*)realloc(m->rels, (m->nrels + count) * sizeof(rel_t));
        if (!m->rels) {
            snprintf(err, err_len, "out of memory for relocations");
            return -1;
        }
        for (u32 i = 0; i < count; ++i) {
            const u8* r = m->elf.data + blob + at + (size_t)i * 24;
            rel_t* rel = &m->rels[m->nrels++];
            rel->target = rd64(r);
            const u64 info = rd64(r + 8);
            rel->kind = info & 0xffffffffull;
            rel->sym = info >> 32;
            rel->addend = (int64_t)rd64(r + 16);
        }
    }
    return 0;
}

typedef struct {
    u8* p;
    size_t len, cap;
} buf_t;

static int buf_reserve(buf_t* b, size_t extra) {
    if (b->len + extra <= b->cap) {
        return 0;
    }
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->len + extra) {
        cap *= 2;
    }
    u8* p = (u8*)realloc(b->p, cap);
    if (!p) {
        return -1;
    }
    b->p = p;
    b->cap = cap;
    return 0;
}

static void put64(buf_t* b, u64 v) {
    if (buf_reserve(b, 8) != 0) {
        return;
    }
    wr64(b->p + b->len, v);
    b->len += 8;
}

static void put_bytes(buf_t* b, const void* p, size_t n) {
    if (buf_reserve(b, n) != 0) {
        return;
    }
    memcpy(b->p + b->len, p, n);
    b->len += n;
}

static int patch_fs_loads(u8* image, size_t image_size, const ph_t* ph, u32 nph, u64 base) {
    static const u8 pattern[9] = {0x64, 0x48, 0x8b, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00};
    int patched = 0;
    for (u32 i = 0; i < nph; ++i) {
        if (ph[i].type != 1 || !(ph[i].flags & 1)) {
            continue;
        }
        const u64 start = base + ph[i].vaddr;
        const u64 end = start + ph[i].filesz;
        if (!in_range(image_size, start, ph[i].filesz)) {
            continue;
        }
        for (u64 at = start; at + sizeof(pattern) <= end; ++at) {
            if (memcmp(image + at, pattern, sizeof(pattern)) == 0) {
                image[at] = 0x65;
                patched++;
                at += sizeof(pattern) - 1;
            }
        }
    }
    return patched;
}

static int write_file(const char* path, const void* data, size_t size, char* err,
                      size_t err_len) {
    FILE* file = bb_open(path, "wb");
    if (!file) {
        snprintf(err, err_len, "cannot create %s", path);
        return -1;
    }
    const int ok = fwrite(data, 1, size, file) == size;
    fclose(file);
    if (!ok) {
        snprintf(err, err_len, "short write on %s", path);
        return -1;
    }
    return 0;
}

static int sfo_read(const char* path, u32 params[4], char* title_id, size_t title_cap, char* err,
                    size_t err_len) {
    size_t total = 0;
    u8* data = read_file(path, &total, err, err_len);
    if (!data) {
        return -1;
    }
    params[0] = params[1] = params[2] = params[3] = 0;
    title_id[0] = '\0';
    if (total < 20 || memcmp(data, "\0PSF", 4) != 0) {
        free(data);
        snprintf(err, err_len, "bad SFO signature");
        return -1;
    }
    const u32 keys = rd32(data + 8);
    const u32 values = rd32(data + 12);
    const u32 count = rd32(data + 16);
    for (u32 i = 0; i < count; ++i) {
        const size_t at = 20 + (size_t)i * 16;
        if (!in_range(total, at, 16)) {
            break;
        }
        const u16 key = rd16(data + at);
        const u16 fmt = rd16(data + at + 2);
        const u32 size = rd32(data + at + 4);
        const u32 offset = rd32(data + at + 12);
        if (!in_range(total, (u64)keys + key, 1) || !in_range(total, (u64)values + offset, size)) {
            continue;
        }
        const char* name = (const char*)(data + keys + key);
        const u8* value = data + values + offset;
        if (strcmp(name, "TITLE_ID") == 0) {
            size_t n = size;
            while (n > 0 && value[n - 1] == 0) {
                --n;
            }
            if (n >= title_cap) {
                n = title_cap - 1;
            }
            memcpy(title_id, value, n);
            title_id[n] = '\0';
        }
        for (int p = 0; p < 4; ++p) {
            char want[32];
            snprintf(want, sizeof(want), "USER_DEFINED_PARAM_%d", p + 1);
            if (strcmp(name, want) == 0 && fmt == 0x404 && size >= 4) {
                params[p] = rd32(value);
            }
        }
    }
    free(data);
    return 0;
}

static int ident_equal(const ident_t* a, const ident_t* b) {
    if (a->raw || b->raw) {
        return a->raw == b->raw && strcmp(a->nid, b->nid) == 0;
    }
    return strcmp(a->nid, b->nid) == 0 && strcmp(a->lib, b->lib) == 0 &&
           strcmp(a->mod, b->mod) == 0;
}

static u32 import_index(const ident_t* idents, u32 count, const ident_t* want, int* found) {
    for (u32 i = 0; i < count; ++i) {
        if (ident_equal(&idents[i], want)) {
            *found = 1;
            return i;
        }
    }
    *found = 0;
    return count;
}

int bb_prepare_boot(const char* dump, const char* out_dir, char* err, size_t err_len) {
    char path[1024];
    bb_mkdir(out_dir);
    module_t mainm;
    snprintf(path, sizeof(path), "%s/eboot.bin", dump);
    if (load_module(path, &mainm, err, err_len) != 0) {
        return -1;
    }

    u32 nloads = 0;
    u64 size = 0;
    seg_t segs[MAX_SEGMENTS];
    u32 nsegs = 0;
    for (u32 i = 0; i < mainm.elf.nph; ++i) {
        const ph_t* p = &mainm.elf.ph[i];
        if (p->type != 1 && p->type != 0x61000010) {
            continue;
        }
        if (size < p->vaddr + p->memsz) {
            size = p->vaddr + p->memsz;
        }
        if (nsegs < MAX_SEGMENTS) {
            segs[nsegs++] = (seg_t){p->vaddr, p->memsz, p->flags};
        }
        nloads++;
    }
    if (!nloads || size > PROBE_LIMIT) {
        snprintf(err, err_len, "invalid memory image");
        return -1;
    }
    u8* image = (u8*)calloc(1, (size_t)size);
    if (!image) {
        snprintf(err, err_len, "out of memory for the %llu-byte image", (unsigned long long)size);
        return -1;
    }
    for (u32 i = 0; i < mainm.elf.nph; ++i) {
        const ph_t* p = &mainm.elf.ph[i];
        if (p->type != 1 && p->type != 0x61000010) {
            continue;
        }
        if (p->filesz > p->memsz || !in_range(mainm.elf.size, p->offset, p->filesz) ||
            !in_range(size, p->vaddr, p->filesz)) {
            snprintf(err, err_len, "invalid load segment %u", i);
            return -1;
        }
        memcpy(image + p->vaddr, mainm.elf.data + p->offset, (size_t)p->filesz);
    }

    rel_t* rels = NULL;
    u32 nrels = 0;
    char(*names)[NAME_LEN] = (char(*)[NAME_LEN])calloc(MAX_NAMES, NAME_LEN);
    ident_t* idents = (ident_t*)calloc(MAX_NAMES, sizeof(ident_t));
    u32 nnames = 0;
    if (!names || !idents) {
        snprintf(err, err_len, "out of memory for the import table");
        return -1;
    }

    for (u32 r = 0; r < mainm.nrels; ++r) {
        const rel_t* rel = &mainm.rels[r];
        if (rel->target + 8 > size) {
            snprintf(err, err_len, "relocation outside mapped segments: 0x%llx",
                     (unsigned long long)rel->target);
            return -1;
        }
        if (rel->kind == 8) {
            rels = (rel_t*)realloc(rels, (nrels + 1) * sizeof(rel_t));
            rels[nrels++] = (rel_t){rel->target, 0, (u64)rel->addend, 0};
            continue;
        }
        if (rel->kind != 1 && rel->kind != 6 && rel->kind != 7) {
            snprintf(err, err_len, "unsupported relocation %llu",
                     (unsigned long long)rel->kind);
            return -1;
        }
        if (rel->sym >= mainm.nsyms) {
            snprintf(err, err_len, "relocation symbol out of range");
            return -1;
        }
        const sym_t* sym = &mainm.syms[rel->sym];
        if (sym->section) {
            if (sym->section == 0xfff1) {
                wr64(image + rel->target, sym->value + (u64)rel->addend);
                continue;
            }
            rels = (rel_t*)realloc(rels, (nrels + 1) * sizeof(rel_t));
            rels[nrels++] = (rel_t){rel->target, 0, sym->value + (u64)rel->addend, 0};
            continue;
        }
        ident_t want = {0};
        if (sym->has_identity) {
            copy_field(want.nid, sizeof(want.nid), sym->nid);
            copy_field(want.lib, sizeof(want.lib), sym->lib);
            copy_field(want.mod, sizeof(want.mod), sym->mod);
        } else {
            want.raw = 1;
            copy_field(want.nid, sizeof(want.nid), sym->name);
        }
        int found = 0;
        u32 index = import_index(idents, nnames, &want, &found);
        if (!found) {
            if (nnames >= MAX_NAMES) {
                snprintf(err, err_len, "too many imports");
                return -1;
            }
            index = nnames++;
            idents[index] = want;
        }
        const u64 kind = sym->type == 1 ? 2 : 1;
        if ((kind == 1 && rel->addend) || rel->addend < 0 || rel->addend >= 4096) {
            snprintf(err, err_len, "unsupported import addend");
            return -1;
        }
        rels = (rel_t*)realloc(rels, (nrels + 1) * sizeof(rel_t));
        rels[nrels++] = (rel_t){rel->target, kind, index, rel->addend};
    }

    for (u32 i = 0; i < nnames; ++i) {
        char spelled[NAME_LEN];
        runtime_name(&idents[i], spelled, sizeof(spelled));
        copy_field(names[i], NAME_LEN, spelled);
    }

    typedef struct {
        u64 base, size, init, tls_address, tls_memsz, tls_filesz, tls_module;
    } modentry_t;
    modentry_t mods[MAX_MODULES];
    u32 nmods = 0;
    export_t* exports = (export_t*)calloc(MAX_EXPORTS, sizeof(export_t));
    u32 nexports = 0;
    if (!exports) {
        snprintf(err, err_len, "out of memory for exports");
        return -1;
    }

    u64 base = (size + 65535) & ~65535ull;
    int fs_patched = patch_fs_loads(image, size, mainm.elf.ph, mainm.elf.nph, 0);
    u32 tls_module = 2;
    const char* module_names[] = {"libc.prx", "libSceFios2.prx"};
    for (u32 mi = 0; mi < 2; ++mi) {
        module_t m;
        snprintf(path, sizeof(path), "%s/sce_module/%s", dump, module_names[mi]);
        if (load_module(path, &m, err, err_len) != 0) {
            return -1;
        }
        u64 modsize = 0;
        for (u32 i = 0; i < m.elf.nph; ++i) {
            const ph_t* p = &m.elf.ph[i];
            if ((p->type == 1 || p->type == 0x61000010) && modsize < p->vaddr + p->memsz) {
                modsize = p->vaddr + p->memsz;
            }
        }
        if (base + modsize > PROBE_LIMIT) {
            snprintf(err, err_len, "linked image exceeds probe limit");
            return -1;
        }
        u8* grown = (u8*)realloc(image, (size_t)(base + modsize));
        if (!grown) {
            snprintf(err, err_len, "out of memory growing the image");
            return -1;
        }
        image = grown;
        memset(image + size, 0, (size_t)(base + modsize - size));
        size = base + modsize;
        for (u32 i = 0; i < m.elf.nph; ++i) {
            const ph_t* p = &m.elf.ph[i];
            if (p->type != 1 && p->type != 0x61000010) {
                continue;
            }
            if (p->filesz > p->memsz || !in_range(m.elf.size, p->offset, p->filesz) ||
                !in_range(size, base + p->vaddr, p->filesz)) {
                snprintf(err, err_len, "%s: invalid segment size", module_names[mi]);
                return -1;
            }
            memcpy(image + base + p->vaddr, m.elf.data + p->offset, (size_t)p->filesz);
            if (nsegs < MAX_SEGMENTS) {
                segs[nsegs++] = (seg_t){base + p->vaddr, p->memsz, p->flags};
            }
        }
        const ph_t* tls = find_ph(&m.elf, 7);
        u64 module_id = 0;
        if (tls) {
            if (tls->filesz > tls->memsz || tls->vaddr + tls->memsz > modsize) {
                snprintf(err, err_len, "%s: unsupported TLS layout", module_names[mi]);
                return -1;
            }
            module_id = tls_module++;
        }
        for (u32 r = 0; r < m.nrels; ++r) {
            const rel_t* rel = &m.rels[r];
            if (rel->target + 8 > modsize) {
                snprintf(err, err_len, "%s: relocation outside image", module_names[mi]);
                return -1;
            }
            const u64 target = rel->target + base;
            if (rel->kind == 8) {
                rels = (rel_t*)realloc(rels, (nrels + 1) * sizeof(rel_t));
                rels[nrels++] = (rel_t){target, 0, base + (u64)rel->addend, 0};
                continue;
            }
            if (rel->sym >= m.nsyms) {
                snprintf(err, err_len, "%s: relocation symbol out of range", module_names[mi]);
                return -1;
            }
            const sym_t* sym = &m.syms[rel->sym];
            if (rel->kind == 16) {
                if (sym->name[0] || sym->type != 3 || rel->addend || !module_id) {
                    snprintf(err, err_len, "%s: unsupported external TLS relocation",
                             module_names[mi]);
                    return -1;
                }
                wr64(image + target, module_id);
                continue;
            }
            if (rel->kind != 1 && rel->kind != 6 && rel->kind != 7) {
                snprintf(err, err_len, "%s: unsupported relocation type", module_names[mi]);
                return -1;
            }
            if (sym->section) {
                if (sym->section == 0xfff1) {
                    wr64(image + target, sym->value + (u64)rel->addend);
                    continue;
                }
                rels = (rel_t*)realloc(rels, (nrels + 1) * sizeof(rel_t));
                rels[nrels++] = (rel_t){target, 0, base + sym->value + (u64)rel->addend, 0};
                continue;
            }
            if (!sym->has_identity && sym->binding == 2) {
                wr64(image + target, 0);
                continue;
            }
            const u64 kind = sym->type == 1 ? 2 : 1;
            if ((kind == 1 && rel->addend) || rel->addend < 0 || rel->addend >= 4096) {
                snprintf(err, err_len, "%s: unsupported import addend", module_names[mi]);
                return -1;
            }
            ident_t want = {0};
            copy_field(want.nid, sizeof(want.nid), sym->nid);
            copy_field(want.lib, sizeof(want.lib), sym->lib);
            copy_field(want.mod, sizeof(want.mod), sym->mod);
            int found = 0;
            u32 index = import_index(idents, nnames, &want, &found);
            if (!found) {
                if (nnames >= MAX_NAMES) {
                    snprintf(err, err_len, "too many imports");
                    return -1;
                }
                index = nnames++;
                idents[index] = want;
                char spelled[NAME_LEN];
                runtime_name(&idents[index], spelled, sizeof(spelled));
                copy_field(names[index], NAME_LEN, spelled);
            }
            rels = (rel_t*)realloc(rels, (nrels + 1) * sizeof(rel_t));
            rels[nrels++] = (rel_t){target, kind, index, rel->addend};
        }
        for (u32 s = 0; s < m.nsyms; ++s) {
            const sym_t* sym = &m.syms[s];
            if (!sym->section || (sym->binding != 1 && sym->binding != 2) || !sym->has_identity ||
                (sym->type != 1 && sym->type != 2)) {
                continue;
            }
            if (sym->value + (sym->size ? sym->size : 1) > modsize) {
                snprintf(err, err_len, "%s: export outside load segments", module_names[mi]);
                return -1;
            }
            if (nexports >= MAX_EXPORTS) {
                snprintf(err, err_len, "too many exports");
                return -1;
            }
            exports[nexports].id = (ident_t){0};
            copy_field(exports[nexports].id.nid, sizeof(exports[nexports].id.nid), sym->nid);
            copy_field(exports[nexports].id.lib, sizeof(exports[nexports].id.lib), sym->lib);
            copy_field(exports[nexports].id.mod, sizeof(exports[nexports].id.mod), sym->mod);
            exports[nexports].value = base + sym->value;
            exports[nexports].kind = sym->type == 1 ? 2 : 1;
            nexports++;
        }
        fs_patched += patch_fs_loads(image, size, m.elf.ph, m.elf.nph, base);
        mods[nmods].base = base;
        mods[nmods].size = modsize;
        mods[nmods].init = base + dyn_value(&m, 12, 0);
        mods[nmods].tls_address = tls ? base + tls->vaddr : 0;
        mods[nmods].tls_memsz = tls ? tls->memsz : 0;
        mods[nmods].tls_filesz = tls ? tls->filesz : 0;
        mods[nmods].tls_module = module_id;
        nmods++;
        base = (base + modsize + 65535) & ~65535ull;
    }

    binding_t* bindings = (binding_t*)calloc(MAX_BINDINGS, sizeof(binding_t));
    u32 nbindings = 0;
    if (!bindings) {
        snprintf(err, err_len, "out of memory for bindings");
        return -1;
    }
    for (u32 i = 0; i < nnames; ++i) {
        const export_t* found = NULL;
        for (u32 e = 0; e < nexports; ++e) {
            if (ident_equal(&exports[e].id, &idents[i])) {
                found = &exports[e];
                break;
            }
        }
        if (!found && strcmp(idents[i].lib, "libSceLibcInternal") == 0) {
            const export_t* candidate = NULL;
            int matches = 0;
            for (u32 e = 0; e < nexports; ++e) {
                if (strcmp(exports[e].id.nid, idents[i].nid) == 0 &&
                    strcmp(exports[e].id.lib, "libc") == 0) {
                    candidate = &exports[e];
                    matches++;
                }
            }
            if (matches == 1) {
                found = candidate;
            }
        }
        if (found) {
            bindings[nbindings].index = i;
            bindings[nbindings].value = found->value;
            bindings[nbindings].kind = found->kind;
            nbindings++;
        }
    }

    const ph_t* procparam = find_ph(&mainm.elf, 0x61000001);
    const ph_t* main_tls = find_ph(&mainm.elf, 7);
    if (main_tls && (main_tls->filesz > main_tls->memsz || main_tls->memsz > 1024 * 1024 ||
                     main_tls->vaddr + main_tls->filesz > size)) {
        snprintf(err, err_len, "unsupported eboot TLS layout");
        return -1;
    }

    buf_t out = {0};
    put_bytes(&out, "BBPROBE5", 8);
    put64(&out, size);
    put64(&out, mainm.elf.entry);
    put64(&out, nsegs);
    put64(&out, nrels);
    put64(&out, nnames);
    put64(&out, (u64)1);  // libc _init_env is verified RET by the caller
    put64(&out, procparam ? procparam->vaddr : 0);
    put64(&out, main_tls ? main_tls->vaddr : 0);
    put64(&out, main_tls ? main_tls->filesz : 0);
    put64(&out, main_tls ? main_tls->memsz : 0);
    put64(&out, main_tls ? main_tls->align : 0);
    put64(&out, nmods);
    for (u32 i = 0; i < nmods; ++i) {
        put64(&out, mods[i].base);
        put64(&out, mods[i].size);
        put64(&out, mods[i].init);
        put64(&out, mods[i].tls_address);
        put64(&out, mods[i].tls_memsz);
        put64(&out, mods[i].tls_filesz);
        put64(&out, mods[i].tls_module);
    }
    put64(&out, nbindings);
    for (u32 i = 0; i < nbindings; ++i) {
        put64(&out, bindings[i].index);
        put64(&out, bindings[i].value);
        put64(&out, bindings[i].kind);
    }
    for (u32 i = 0; i < nsegs; ++i) {
        put64(&out, segs[i].vaddr);
        put64(&out, segs[i].memsz);
        put64(&out, segs[i].flags);
    }
    for (u32 i = 0; i < nnames; ++i) {
        put_bytes(&out, names[i], NAME_LEN);
    }
    for (u32 i = 0; i < nrels; ++i) {
        put64(&out, rels[i].target);
        put64(&out, rels[i].kind);
        put64(&out, rels[i].sym);
        put64(&out, (u64)rels[i].addend);
    }
    put_bytes(&out, image, (size_t)size);

    snprintf(path, sizeof(path), "%s/boot-linked.bin", out_dir);
    if (write_file(path, out.p, out.len, err, err_len) != 0) {
        return -1;
    }

    u32 params[4];
    char title[32];
    snprintf(path, sizeof(path), "%s/sce_sys/param.sfo", dump);
    if (sfo_read(path, params, title, sizeof(title), err, err_len) != 0) {
        return -1;
    }
    if (params[0] == 0 && title[0]) {
        u32 digits = 0;
        for (const char* c = title; *c; ++c) {
            if (*c >= '0' && *c <= '9') {
                digits = digits * 10 + (u32)(*c - '0');
            }
        }
        params[0] = digits;
    }
    u8 content[28];
    memcpy(content, "BBCONT01", 8);
    wr64(content + 8, 0);
    for (int i = 0; i < 4; ++i) {
        content[12 + i * 4 + 0] = (u8)(params[i]);
        content[12 + i * 4 + 1] = (u8)(params[i] >> 8);
        content[12 + i * 4 + 2] = (u8)(params[i] >> 16);
        content[12 + i * 4 + 3] = (u8)(params[i] >> 24);
    }
    content[8] = 3;  // full SKU, as the probe setting
    snprintf(path, sizeof(path), "%s/content.bin", out_dir);
    if (write_file(path, content, sizeof(content), err, err_len) != 0) {
        return -1;
    }
    printf("M3 prepare: %llu-byte image, %u imports, %u relocations, fs->gs patched=%d\n",
           (unsigned long long)size, nnames, nrels, fs_patched);
    fflush(stdout);
    return 0;
}
