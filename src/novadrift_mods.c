// Nova Drift mod loader (version.dll proxy)
// Hooks the YYC-compiled GML functions and sets game variables from novadrift_mods.ini.
// Addresses are resolved at runtime by name and byte pattern.

#define _USE_MATH_DEFINES
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

// ---------------------------------------------------------------- Proxy ----

#define PROXY_EXPORTS(X) \
    X(GetFileVersionInfoA) X(GetFileVersionInfoByHandle) X(GetFileVersionInfoExA) \
    X(GetFileVersionInfoExW) X(GetFileVersionInfoSizeA) X(GetFileVersionInfoSizeExA) \
    X(GetFileVersionInfoSizeExW) X(GetFileVersionInfoSizeW) X(GetFileVersionInfoW) \
    X(VerFindFileA) X(VerFindFileW) X(VerInstallFileA) X(VerInstallFileW) \
    X(VerLanguageNameA) X(VerLanguageNameW) X(VerQueryValueA) X(VerQueryValueW)

#define X(n) void *p_##n;
PROXY_EXPORTS(X)
#undef X

#define X(n) \
    __asm__(".globl " #n "\n" #n ":\n jmp *p_" #n "(%rip)\n");
PROXY_EXPORTS(X)
#undef X

static void load_real_version(void) {
    wchar_t path[MAX_PATH];
    GetSystemDirectoryW(path, MAX_PATH);
    wcscat(path, L"\\version.dll");
    HMODULE h = LoadLibraryW(path);
    if (!h) return;
#define X(n) p_##n = (void *)GetProcAddress(h, #n);
    PROXY_EXPORTS(X)
#undef X
}

// ------------------------------------------------------------------ Log ----

static wchar_t g_dir[MAX_PATH];
static FILE *g_log;

static void logf_(const char *fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

// -------------------------------------------------------- Image helpers ----

typedef struct { uint8_t *start, *end; } Range;
static Range g_text, g_rdata, g_data;

static void find_sections(void) {
    uint8_t *base = (uint8_t *)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        Range r = { base + s->VirtualAddress, base + s->VirtualAddress + s->Misc.VirtualSize };
        if (!memcmp(s->Name, ".text", 6)) g_text = r;
        else if (!memcmp(s->Name, ".rdata", 7)) g_rdata = r;
        else if (!memcmp(s->Name, ".data", 6)) g_data = r;
    }
}

static int in_range(Range r, const void *p) { return (uint8_t *)p >= r.start && (uint8_t *)p < r.end; }

static uint8_t *rel32_target(uint8_t *p) { return p + 4 + *(int32_t *)p; }

// YYC function table {name, func, ...}: collected once, sorted by name
typedef struct { const char *name; uint8_t *func; size_t order; } FuncEntry;
static FuncEntry *g_funcs;
static size_t g_func_count;

static int func_cmp(const void *a, const void *b) {
    const FuncEntry *x = a, *y = b;
    int c = strcmp(x->name, y->name);
    return c ? c : (x->order > y->order) - (x->order < y->order);  // first entry wins
}

static void build_func_table(void) {
    size_t cap = 1 << 14;
    g_funcs = malloc(cap * sizeof *g_funcs);
    for (uint8_t **q = (uint8_t **)g_data.start; (uint8_t *)(q + 2) <= g_data.end; q++) {
        if (!in_range(g_rdata, q[0]) || q[0] + 4 > g_rdata.end || memcmp(q[0], "gml_", 4) ||
            !in_range(g_text, q[1]))
            continue;
        if (g_func_count == cap) g_funcs = realloc(g_funcs, (cap *= 2) * sizeof *g_funcs);
        g_funcs[g_func_count] = (FuncEntry){ (const char *)q[0], q[1], g_func_count };
        g_func_count++;
    }
    qsort(g_funcs, g_func_count, sizeof *g_funcs, func_cmp);
}

// Finds "gml_Script_xyz" / "gml_Object_..." by name
static uint8_t *find_gml_function(const char *name) {
    size_t lo = 0, hi = g_func_count;
    while (lo < hi) {  // lower bound, so duplicates resolve to the first table entry
        size_t mid = (lo + hi) / 2;
        if (strcmp(g_funcs[mid].name, name) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo < g_func_count && !strcmp(g_funcs[lo].name, name) ? g_funcs[lo].func : NULL;
}

static uint8_t *find_pattern(uint8_t *from, size_t len, const char *pat, const char *mask) {
    size_t n = strlen(mask);
    for (uint8_t *p = from; p + n <= from + len; p++) {
        size_t i = 0;
        while (i < n && (mask[i] == '?' || p[i] == (uint8_t)pat[i])) i++;
        if (i == n) return p;
    }
    return NULL;
}

// --------------------------------------------------------- Variables -------

// YYC emits an entry {const char *name; int id;} per GML variable.
// id is -1 on disk; the runtime fills in the real ID later.
typedef struct { const char *name; int32_t *id; } VarEntry;
static VarEntry *g_vars;
static size_t g_var_count;

static int var_cmp(const void *a, const void *b) {
    return strcmp(((const VarEntry *)a)->name, ((const VarEntry *)b)->name);
}

static void build_var_table(void) {
    size_t cap = 1 << 15;
    g_vars = malloc(cap * sizeof *g_vars);
    for (uint8_t *p = g_data.start; p + 16 <= g_data.end; p += 8) {
        const char *name = *(const char **)p;
        if (*(uint64_t *)(p + 8) != 0xffffffffull || !in_range(g_rdata, name)) continue;
        const char *c = name;
        while (in_range(g_rdata, c) && *c && ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                                              (*c >= '0' && *c <= '9') || *c == '_'))
            c++;
        if (*c || c == name) continue;
        if (g_var_count == cap) g_vars = realloc(g_vars, (cap *= 2) * sizeof *g_vars);
        g_vars[g_var_count++] = (VarEntry){ name, (int32_t *)(p + 8) };
    }
    qsort(g_vars, g_var_count, sizeof *g_vars, var_cmp);
}

// Builtin functions (ds_list_add, sprite_add, ...) live in the same table but start
// at 0 instead of -1. At runtime the entry holds the function index.
static int32_t *builtin_slot(const char *name) {
    for (uint8_t *p = g_data.start; p + 16 <= g_data.end; p += 8) {
        const char *s = *(const char **)p;
        if (in_range(g_rdata, s) && !strcmp(s, name)) return (int32_t *)(p + 8);
    }
    return NULL;
}

// The slot address is fixed; the runtime writes the real ID into it later
static int32_t *var_slot(const char *name) {
    VarEntry key = { name, NULL };
    VarEntry *e = bsearch(&key, g_vars, g_var_count, sizeof *g_vars, var_cmp);
    return e ? e->id : NULL;
}

static int var_id(const char *name) {
    int32_t *s = var_slot(name);
    return s ? *s : -1;
}

// Variable / builtin ID with the lookup cached per call site (name must be a literal)
#define CACHED_SLOT(find, name) ({ \
    static int32_t *slot_; static int tried_; \
    if (!tried_) tried_ = 1, slot_ = find(name); \
    slot_ ? *slot_ : -1; })
#define VAR(name) CACHED_SLOT(var_slot, name)
#define BI(name) CACHED_SLOT(builtin_slot, name)

// GameMaker runtime RValue
typedef struct { union { double real; int32_t i32; int64_t i64; void *ptr; }; uint32_t flags, kind; } RValue;
enum { KIND_REAL = 0, KIND_INT32 = 2, KIND_INT64 = 10, KIND_BOOL = 13, KIND_REF = 15 };

typedef RValue *(*GetRefFn)(void *inst, int id);
typedef void (*SetIntFn)(RValue *rv, int v);
typedef void (*SetRealFn)(RValue *rv, double v);
typedef RValue *(*VtGetRefFn)(void *inst, int id);

static GetRefFn g_get_ref;
static SetIntFn g_set_int;
static SetRealFn g_set_real;
static void **g_global_ptr;  // pointer to the GML "global" object

static int rv_get(const RValue *rv, double *out) {
    switch (rv->kind & 0xffffff) {
    case KIND_REAL: case KIND_BOOL: *out = rv->real; return 1;
    case KIND_INT32: *out = rv->i32; return 1;
    case KIND_INT64: *out = (double)rv->i64; return 1;
    case KIND_REF: *out = (double)(int32_t)rv->i64; return 1;  // asset reference: low bits hold the index
    }
    return 0;
}

static void rv_set(RValue *rv, double v) {
    uint32_t kind = rv->kind & 0xffffff;
    if (kind == KIND_INT32 && v == floor(v) && fabs(v) < 2147483648.0) g_set_int(rv, (int)v);
    else g_set_real(rv, v);
}

static double get_var(void *self, int id) {
    double v = -1;
    if (id >= 0) rv_get(g_get_ref(self, id), &v);
    return v;
}

static RValue *global_ref(int id) {
    void *g = g_global_ptr ? *g_global_ptr : NULL;
    if (id < 0 || !g) return NULL;
    VtGetRefFn fn = (*(VtGetRefFn **)g)[1];
    return fn(g, id);
}

// ------------------------------------------------------------ Assets -------

// Object names from data.win (OBJT chunk) so weapons can be addressed by name.
static char **g_obj_names;
static uint32_t g_obj_count;

static void load_object_names(void) {
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\data.win", g_dir);
    FILE *f = _wfopen(path, L"rb");
    if (!f) return;
    uint32_t hdr[2];
    long pos = 8, objt = -1;
    while (fseek(f, pos, SEEK_SET) == 0 && fread(hdr, 4, 2, f) == 2) {
        if (!memcmp(hdr, "OBJT", 4)) { objt = pos + 8; break; }
        pos += 8 + hdr[1];
    }
    if (objt >= 0 && fread(&g_obj_count, 4, 1, f) == 1 && g_obj_count < 100000) {
        uint32_t *offs = malloc(g_obj_count * 4);
        fread(offs, 4, g_obj_count, f);
        g_obj_names = calloc(g_obj_count, sizeof *g_obj_names);
        for (uint32_t i = 0; i < g_obj_count; i++) {
            uint32_t str;
            char buf[128] = "";
            fseek(f, offs[i], SEEK_SET);
            if (fread(&str, 4, 1, f) != 1) continue;
            fseek(f, str, SEEK_SET);
            if (!fgets(buf, sizeof buf, f)) continue;
            buf[strcspn(buf, "\r\n")] = 0;
            g_obj_names[i] = _strdup(buf);
        }
        free(offs);
    }
    fclose(f);
    logf_("Read %u object names from data.win", g_obj_count);
}

static const char *object_name(int idx) {
    return idx >= 0 && (uint32_t)idx < g_obj_count && g_obj_names[idx] ? g_obj_names[idx] : "?";
}

// Same order as the game (localization); 0 = no gear
static const char *BODY_NAMES[] = { "None", "Standard", "Assault", "Hullbreaker", "Spectre", "Firefly",
                                    "Sentinel", "Engineer", "Research", "Battery", "Architect", "Viper",
                                    "Carrier", "Courser", "Leviathan" };
static const char *SHIELD_NAMES[] = { "None", "Standard", "Halo", "Reflect", "Bastion", "Temporal",
                                      "Amp", "Warp", "Orbital", "Siphon", "Shockwave" };

static const char *table_name(const char **names, int count, int idx) {
    return idx >= 0 && idx < count ? names[idx] : "?";
}

// ---------------------------------------------------------------- INI ------

typedef struct { char section[32]; char name[64]; double value; int32_t *slot; } Override;
static Override *g_ovr;
static size_t g_ovr_count, g_ovr_cap;
static int g_dump, g_modded, g_block_always = 1;

static void trim(char *s) {
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    size_t lead = strspn(s, " \t");
    memmove(s, s + lead, strlen(s + lead) + 1);
}

// [newbody]/[newweapon] texts: "section.key" -> UTF-8 text (\n becomes a line break)
typedef struct { char key[64]; char *text; } GearText;
static GearText g_texts[48];
static int g_text_count;

static void gear_text_set(const char *section, const char *key, const char *val) {
    char full[64];
    snprintf(full, sizeof full, "%s.%s", section, key);
    GearText *t = NULL;
    for (int i = 0; i < g_text_count; i++)
        if (!_stricmp(g_texts[i].key, full)) t = &g_texts[i];
    if (!t) {
        if (g_text_count == 48) return;
        t = &g_texts[g_text_count++];
        snprintf(t->key, sizeof t->key, "%s", full);
    }
    char *text = malloc(strlen(val) * 2 + 1), *o = text;
    for (const char *c = val; *c; c++) {
        if (c[0] == '\\' && c[1] == 'n') { *o++ = '\r'; *o++ = '\n'; c++; }
        else *o++ = *c;
    }
    *o = 0;
    // Old texts stay allocated: cached GML strings may still point to them
    if (t->text && !strcmp(t->text, text)) { free(text); return; }
    t->text = text;
}

static const char *gear_text_get(const char *section, const char *key) {
    char full[64];
    snprintf(full, sizeof full, "%s.%s", section, key);
    for (int i = 0; i < g_text_count; i++)
        if (!_stricmp(g_texts[i].key, full)) return g_texts[i].text;
    return NULL;
}

// ------------------------------------------------------- Embedded files ----
// Built into the DLL at compile time (paths relative to the repo root), so
// copying version.dll is the whole installation.

#define EMBED(sym, path) \
    __asm__(".section .rdata,\"dr\"\n" #sym ":\n.incbin \"" path "\"\n" #sym "_end:\n.byte 0\n.text\n"); \
    extern const char sym[], sym##_end[];

EMBED(emb_ini_template, "novadrift_mods.ini")
EMBED(emb_scorpion_ini, "profiles/scorpion/scorpion.ini")
EMBED(emb_scorpion_png, "profiles/scorpion/skin/scorpion.png")
EMBED(emb_scorpion_force_png, "profiles/scorpion/skin/scorpion_force.png")
EMBED(emb_scorpion_drone_png, "profiles/scorpion/skin/scorpion_drone.png")
EMBED(emb_scorpion_icon_png, "profiles/scorpion/skin/scorpion_icon.png")
EMBED(emb_stinger_icon_png, "profiles/scorpion/skin/stinger_icon.png")
EMBED(emb_stinger_shot_png, "profiles/scorpion/skin/stinger_shot.png")
EMBED(emb_stinger_whip_png, "profiles/scorpion/skin/stinger_whip.png")

typedef struct { const wchar_t *path; const char *data, *end; } EmbeddedFile;
#define FILE_ENTRY(name, sym) { L"custom_skins\\" name, sym, sym##_end }
static const EmbeddedFile SCORPION_FILES[] = {
    FILE_ENTRY(L"scorpion.png", emb_scorpion_png),
    FILE_ENTRY(L"scorpion_force.png", emb_scorpion_force_png),
    FILE_ENTRY(L"scorpion_drone.png", emb_scorpion_drone_png),
    FILE_ENTRY(L"scorpion_icon.png", emb_scorpion_icon_png),
    FILE_ENTRY(L"stinger_icon.png", emb_stinger_icon_png),
    FILE_ENTRY(L"stinger_shot.png", emb_stinger_shot_png),
    FILE_ENTRY(L"stinger_whip.png", emb_stinger_whip_png),
};
static int g_builtin_profile;  // the built-in scorpion profile is active

static void game_path(wchar_t *out, const wchar_t *rel) { swprintf(out, MAX_PATH, L"%ls\\%ls", g_dir, rel); }

// Writes the file if it is missing or differs in size
static void write_if_changed(const wchar_t *rel, const char *data, size_t len) {
    wchar_t path[MAX_PATH];
    game_path(path, rel);
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &a) && !a.nFileSizeHigh && a.nFileSizeLow == len) return;
    FILE *f = _wfopen(path, L"wb");
    if (!f) { logf_("ERROR: cannot write %ls", path); return; }
    fwrite(data, 1, len, f);
    fclose(f);
    logf_("Wrote %ls", rel);
}

static char *read_file(const wchar_t *path) {
    FILE *f = _wfopen(path, L"rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(n + 1);
    buf[fread(buf, 1, n, f)] = 0;
    fclose(f);
    return buf;
}

// First start: default settings file
static void install_settings_file(void) {
    wchar_t path[MAX_PATH];
    game_path(path, L"novadrift_mods.ini");
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
        write_if_changed(L"novadrift_mods.ini", emb_ini_template, emb_ini_template_end - emb_ini_template);
}

// Built-in profile: graphics and custom skin row (the game reads them after startup)
static void install_profile_files(void) {
    if (!g_builtin_profile) return;
    wchar_t path[MAX_PATH];
    game_path(path, L"custom_skins");
    CreateDirectoryW(path, NULL);
    for (size_t i = 0; i < sizeof SCORPION_FILES / sizeof *SCORPION_FILES; i++)
        write_if_changed(SCORPION_FILES[i].path, SCORPION_FILES[i].data, SCORPION_FILES[i].end - SCORPION_FILES[i].data);

    const char *row = gear_text_get("newbody", "skin_row");
    if (!row || !strchr(row, ',')) return;
    game_path(path, L"custom_skins\\custom_skins.csv");
    char *csv = read_file(path);
    size_t name_len = strchr(row, ',') - row;
    for (char *l = csv; l && *l; l += strcspn(l, "\n"), l += *l == '\n')
        if (!strncmp(l, row, name_len + 1)) { free(csv); return; }  // already listed
    FILE *f = _wfopen(path, L"ab");
    if (!f) { free(csv); return; }
    if (!csv) fputs("name,body_artwork,body_force_armor_artwork,drone_artwork,jet_angle,jet_distance\r\n", f);
    else if (*csv && csv[strlen(csv) - 1] != '\n') fputs("\r\n", f);
    fprintf(f, "%s\r\n", row);
    fclose(f);
    free(csv);
    logf_("Added custom skin row: %.*s", (int)name_len, row);
}

static int parse_ini(const wchar_t *path, int allow_profile);

// Parses INI text; profile=<name> in [settings] also loads novadrift_profiles\<name>.ini,
// or the built-in profile (profiles cannot load further profiles).
static int parse_ini_text(const char *text, int allow_profile) {
    char line[512], section[32] = "";
    for (const char *p = text; *p;) {
        size_t n = strcspn(p, "\n");
        snprintf(line, sizeof line, "%.*s", (int)(n < sizeof line ? n : sizeof line - 1), p);
        p += n + (p[n] == '\n');
        char *c = strpbrk(line, ";#");
        if (c) *c = 0;
        trim(line);
        if (!*line) continue;
        if (*line == '[') {
            char *e = strchr(line, ']');
            if (e) *e = 0;
            snprintf(section, sizeof section, "%s", line + 1);
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = line, *val = eq + 1;
        trim(key);
        trim(val);
        if (!_stricmp(section, "newbody") || !_stricmp(section, "newweapon")) {
            gear_text_set(section, key, val);
            continue;
        }
        if (!_stricmp(section, "settings")) {
            if (!_stricmp(key, "dump")) g_dump = atoi(val);
            if (!_stricmp(key, "block_leaderboards")) g_block_always = atoi(val);
            if (!_stricmp(key, "profile") && *val && allow_profile) {
                wchar_t ppath[MAX_PATH];
                swprintf(ppath, MAX_PATH, L"%ls\\novadrift_profiles\\%hs.ini", g_dir, val);
                if (!_stricmp(val, "scorpion")) {
                    g_builtin_profile = 1;
                    parse_ini_text(emb_scorpion_ini, 0);
                    logf_("Profile loaded: %s (built in)", val);
                } else if (parse_ini(ppath, 0)) logf_("Profile loaded: %s", val);
                else logf_("ERROR: profile not found: %ls", ppath);
            }
            continue;
        }
        char *end;
        double v = strtod(val, &end);
        if (end == val) { logf_("  Invalid value: [%s] %s=%s", section, key, val); continue; }
        if (g_ovr_count == g_ovr_cap)
            g_ovr = realloc(g_ovr, (g_ovr_cap = g_ovr_cap ? g_ovr_cap * 2 : 64) * sizeof *g_ovr);
        Override *o = &g_ovr[g_ovr_count++];
        snprintf(o->section, sizeof o->section, "%s", section);
        snprintf(o->name, sizeof o->name, "%s", key);
        o->value = v;
        o->slot = var_slot(key);
    }
    return 1;
}

static int parse_ini(const wchar_t *path, int allow_profile) {
    char *text = read_file(path);
    if (!text) return 0;
    parse_ini_text(text, allow_profile);
    free(text);
    return 1;
}

// Caches the [newbody]/[newweapon] values (newgear.inc, whip.inc)
static void newgear_config(void);
static void whip_config(void);

static void load_ini(void) {
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\novadrift_mods.ini", g_dir);
    g_ovr_count = 0;
    g_builtin_profile = 0;
    g_dump = 0;
    g_block_always = 1;
    if (parse_ini(path, 1)) logf_("novadrift_mods.ini loaded: %zu entries", g_ovr_count);
    else logf_("No novadrift_mods.ini found");
    newgear_config();
    whip_config();
}

static void apply_section(void *self, const char *section, int verbose) {
    for (size_t i = 0; i < g_ovr_count; i++) {
        Override *o = &g_ovr[i];
        if (_stricmp(o->section, section)) continue;
        int id = o->slot ? *o->slot : -1;
        if (id < 0) {
            if (verbose) logf_("  [%s] %s: unknown variable", section, o->name);
            continue;
        }
        RValue *rv = g_get_ref(self, id);
        double old = NAN;
        rv_get(rv, &old);
        rv_set(rv, o->value);
        g_modded = 1;
        if (verbose) logf_("  [%s] %s: %g -> %g", section, o->name, old, o->value);
    }
}

// Applies [prefix.<number>] and [prefix.<name>]
static void apply_gear(void *self, const char *prefix, int idx, const char *name, int verbose) {
    char sec[96];
    snprintf(sec, sizeof sec, "%s.%d", prefix, idx);
    apply_section(self, sec, verbose);
    snprintf(sec, sizeof sec, "%s.%s", prefix, name);
    apply_section(self, sec, verbose);
}

// Always blocked when values are modded, otherwise per block_leaderboards (default: on)
static int block_leaderboards(void) {
    if (!g_modded && !g_block_always) return 0;
    RValue *rv = global_ref(VAR("enableLeaderboards"));
    if (!rv) return 0;
    g_set_real(rv, 0.0);
    return 1;
}

// ---------------------------------------------------------------- Hooks ----

// Length decoder for common prologue instructions only; anything else -> give up.
// *rip_disp receives the offset of a RIP-relative disp32 (else -1).
static int insn_len(const uint8_t *p, int *rip_disp) {
    int n = 0;
    *rip_disp = -1;
    if ((p[0] & 0xf0) == 0x40) n++;
    uint8_t op = p[n++];
    if (op >= 0x50 && op <= 0x57) return n;
    int imm = 0;
    if (op == 0x88 || op == 0x89 || op == 0x8a || op == 0x8b || op == 0x8d) imm = 0;
    else if (op == 0x83) imm = 1;
    else if (op == 0x81) imm = 4;
    else return 0;
    uint8_t modrm = p[n++], mod = modrm >> 6, rm = modrm & 7;
    if (mod == 0 && rm == 5) {  // RIP-relative: disp32 is fixed up when copied
        *rip_disp = n;
        return n + 4 + imm;
    }
    if (mod != 3 && rm == 4) {
        uint8_t sib = p[n++];
        if (mod == 0 && (sib & 7) == 5) n += 4;
    }
    if (mod == 1) n += 1;
    if (mod == 2) n += 4;
    return n + imm;
}

static void write_abs_jmp(uint8_t *at, void *to) {
    at[0] = 0xff; at[1] = 0x25;
    *(uint32_t *)(at + 2) = 0;
    *(void **)(at + 6) = to;
}

static void *hook(uint8_t *target, void *detour, const char *name) {
    if (!target) { logf_("ERROR: %s not found", name); return NULL; }
    int stolen = 0, rip_at[8], rip_end[8], rip_count = 0;
    while (stolen < 14) {
        int disp, l = insn_len(target + stolen, &disp);
        if (!l) { logf_("ERROR: unknown prologue in %s", name); return NULL; }
        if (disp >= 0) rip_at[rip_count] = stolen + disp, rip_end[rip_count++] = stolen + l;
        stolen += l;
    }
    // Trampoline within +-2 GB of the game code so RIP-relative addresses still reach
    uint8_t *tramp = NULL;
    for (uintptr_t a = ((uintptr_t)target & ~0xffffull) - 0x10000; !tramp && a > (uintptr_t)target - 0x70000000; a -= 0x10000)
        tramp = VirtualAlloc((void *)a, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) { logf_("ERROR: no trampoline memory for %s", name); return NULL; }
    memcpy(tramp, target, stolen);
    for (int i = 0; i < rip_count; i++) {
        uint8_t *dest = target + rip_end[i] + *(int32_t *)(target + rip_at[i]);
        *(int32_t *)(tramp + rip_at[i]) = (int32_t)(dest - (tramp + rip_end[i]));
    }
    write_abs_jmp(tramp + stolen, target + stolen);
    DWORD old;
    VirtualProtect(target, stolen, PAGE_EXECUTE_READWRITE, &old);
    write_abs_jmp(target, detour);
    for (int i = 14; i < stolen; i++) target[i] = 0x90;
    VirtualProtect(target, stolen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, stolen);
    logf_("Hooked %s @ %p", name, target);
    return tramp;
}

// YYC script function: RValue* f(self, other, result, argc, argv)
typedef void *(*GmlFn)(void *, void *, void *, int, void **);
static GmlFn o_init_vars, o_assign_base_stats, o_spawn_enemies, o_end_score;

static void *hk_init_vars(void *self, void *other, void *ret, int argc, void **argv) {
    void *r = o_init_vars(self, other, ret, argc, argv);
    load_ini();
    apply_section(self, "vars", 1);
    block_leaderboards();
    return r;
}

static const char *DUMP_VARS[] = {
    "bodySetHull", "bodySetHullMax", "bodySetAccell", "bodyMultFireTime", "bodyMultDamage",
    "bodyMultShield", "bodyMultSpread", "bodyMultVelocity", "bodyMultShipSize", "bodyMultSize",
    "bodyAddProjectiles", "bodyMagnetism", "weaponSetDamage", "weaponSetVelocity",
    "weaponBaseFireTime", "weaponSetProjectiles", "weaponSetSpread", "weaponSetSize",
    "weaponAddSeeking", "seekRate", "shieldSetShield", "shieldSetShieldMax", "shieldSetShieldCool",
    "shieldMultDamage", "shieldMultAccell",
};

static void gear_pre_assign(void *self);
static int newgear_post_assign(void *self, int which, int verbose);
static void newgear_update_skin(void *self, void *other);

static void *hk_assign_base_stats(void *self, void *other, void *ret, int argc, void **argv) {
    gear_pre_assign(self);
    void *r = o_assign_base_stats(self, other, ret, argc, argv);
    static int last_body = -2, last_weapon = -2, last_shield = -2;
    int body = (int)get_var(self, VAR("bodyType"));
    int weapon = (int)get_var(self, VAR("weaponType"));
    int shield = (int)get_var(self, VAR("shieldType"));
    const char *body_name = table_name(BODY_NAMES, sizeof BODY_NAMES / sizeof *BODY_NAMES, body);
    const char *shield_name = table_name(SHIELD_NAMES, sizeof SHIELD_NAMES / sizeof *SHIELD_NAMES, shield);
    const char *weapon_name = object_name(weapon);
    if (weapon_name[0] == 'o') weapon_name++;  // oRailgun -> Railgun

    // Only log when the gear changes (this runs very often)
    int verbose = body != last_body || weapon != last_weapon || shield != last_shield;
    last_body = body, last_weapon = weapon, last_shield = shield;
    if (verbose) {
        logf_("Gear: [body.%s] (%d)  [weapon.%s] (%d)  [shield.%s] (%d)", body_name, body,
              weapon_name, weapon, shield_name, shield);
        if (g_dump)
            for (size_t i = 0; i < sizeof DUMP_VARS / sizeof *DUMP_VARS; i++)
                logf_("  dump %s = %g", DUMP_VARS[i], get_var(self, var_id(DUMP_VARS[i])));
    }
    // New gear (custom body/weapon) instead of the regular sections
    if (!newgear_post_assign(self, 0, verbose)) apply_gear(self, "body", body, body_name, verbose);
    newgear_update_skin(self, other);
    if (!newgear_post_assign(self, 1, verbose)) apply_gear(self, "weapon", weapon, weapon_name, verbose);
    apply_gear(self, "shield", shield, shield_name, verbose);
    block_leaderboards();
    return r;
}

static void *hk_spawn_enemies(void *self, void *other, void *ret, int argc, void **argv) {
    void *r = o_spawn_enemies(self, other, ret, argc, argv);
    block_leaderboards();
    return r;
}

static void *hk_end_score(void *self, void *other, void *ret, int argc, void **argv) {
    if (block_leaderboards()) logf_("Run end: score upload blocked");
    else logf_("Run end: score upload NOT blocked");
    return o_end_score(self, other, ret, argc, argv);
}

#include "newgear.inc"
#include "whip.inc"

static void install(void) {
    find_sections();
    build_var_table();
    build_func_table();
    logf_("Found %zu GML variables, %zu functions", g_var_count, g_func_count);

    uint8_t *init_vars = find_gml_function("gml_Script_initializeVariables");
    uint8_t *assign = find_gml_function("gml_Script_assignBaseStats");
    uint8_t *spawn = find_gml_function("gml_Script_spawnEnemies");
    uint8_t *end_score = find_gml_function("gml_Object_oDrawEndScore_Create_0");
    if (!init_vars || !assign) { logf_("ERROR: core functions not found, mods disabled"); return; }

    // Derive runtime helpers from call patterns in initializeVariables
    uint8_t *p;
    if ((p = find_pattern(init_vars, 0x10000, "\x8b\x15????\x49\x8b\xce\xe8", "xx????xxxx")))
        g_get_ref = (GetRefFn)rel32_target(p + 10);
    if ((p = find_pattern(init_vars, 0x10000, "\xba????\x48\x8b\xc8\xe8", "x????xxxx")))
        g_set_int = (SetIntFn)rel32_target(p + 9);
    if ((p = find_pattern(init_vars, 0x10000, "\xf2\x0f\x10\x0d????\x48\x8b\xc8\xe8", "xxxx????xxxx")))
        g_set_real = (SetRealFn)rel32_target(p + 12);
    // global object: mov rcx,[global]; mov r8,[rcx]; mov edx,[id]; call [r8+8]
    if (spawn && (p = find_pattern(spawn, 0x4000, "\x48\x8b\x0d????\x4c\x8b\x01\x8b\x15????\x41\xff\x50\x08",
                                    "xxx????xxxxx????xxxx")))
        g_global_ptr = (void **)rel32_target(p + 3);
    logf_("Helpers: getRef=%p setInt=%p setReal=%p global=%p", g_get_ref, g_set_int, g_set_real, g_global_ptr);
    if (!g_get_ref || !g_set_int || !g_set_real) { logf_("ERROR: runtime helpers not found"); return; }

    o_init_vars = hook(init_vars, hk_init_vars, "initializeVariables");
    o_assign_base_stats = hook(assign, hk_assign_base_stats, "assignBaseStats");
    if (g_global_ptr) {
        o_spawn_enemies = hook(spawn, hk_spawn_enemies, "spawnEnemies");
        o_end_score = hook(end_score, hk_end_score, "oDrawEndScore_Create_0");
    }
    if (!g_global_ptr || !o_end_score)
        logf_("WARNING: leaderboard block inactive - do not submit scores!");
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);
    load_real_version();
    GetModuleFileNameW(NULL, g_dir, MAX_PATH);
    wchar_t *slash = wcsrchr(g_dir, L'\\');
    if (slash) *slash = 0;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (!wcsstr(exe, L"NovaDrift.exe")) return TRUE;
    wchar_t log[MAX_PATH];
    swprintf(log, MAX_PATH, L"%ls\\novadrift_mods.log", g_dir);
    g_log = _wfopen(log, L"w");
    logf_("Nova Drift mod loader started");
    load_object_names();
    install();
    install_settings_file();
    load_ini();
    install_profile_files();
    install_newgear();
    install_whip();
    logf_("Leaderboard block: %s", g_block_always ? "always on" : "only with modded values");
    return TRUE;
}
