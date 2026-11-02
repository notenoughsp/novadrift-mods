// ---------------------------------------------------------- Runtime + GML ----
// Runtime helpers (verified for game version 1.2.38.4) and small GML wrappers
typedef void (*AssignFn)(RValue *dst, const RValue *src);
typedef void (*MakeStringFn)(RValue *out, const char *utf8);
typedef void *(*CallBuiltinFn)(void *self, void *other, RValue *ret, int argc, int64_t fn, RValue **argv);
typedef void (*GmlEventFn)(void *self, void *other);
// Sets / reads built-in instance variables (sprite_index, image_blend, ...)
typedef void (*SetBuiltinVarFn)(void *inst, int id, int array_index, RValue *value);
typedef void (*GetBuiltinVarFn)(void *inst, int id, int array_index, RValue *out);
static GetBuiltinVarFn g_get_builtin_var;

static AssignFn g_assign;
static MakeStringFn g_make_string;
static CallBuiltinFn g_call_builtin;
static SetBuiltinVarFn g_set_builtin_var;
static void *g_powers;  // oPowers instance of the current run (build and stats)
static void spawn_probe(void *self, void *other);  // objects.inc
static int g_obj_player = -1;

// GML "with" iterator: begin returns the instance count and sets *self per instance
typedef int (*WithBeginFn)(void *it, void **self, void **other, RValue *target);
typedef char (*WithNextFn)(void *it, void **self, void **other);
typedef void (*WithEndFn)(void *it, void **self, void **other);
typedef void (*FreeFn)(void *p);

static WithBeginFn g_with_begin;
static WithNextFn g_with_next;
static WithEndFn g_with_end;
static FreeFn g_yy_free;

typedef struct { const char *name; uint32_t rva; const char *bytes; size_t len; } Helper;
#define HELPER(name, rva, bytes) { name, rva, bytes, sizeof bytes - 1 }
static const Helper HELPERS[] = {
    HELPER("assign", 0x2c010, "\x40\x53\x48\x83\xec\x30\x48\x8b\xd9"),
    HELPER("makeString", 0xf2cb60, "\x48\x89\x5c\x24\x10\x57\x48\x83\xec\x20"),
    HELPER("callBuiltin", 0xf2d190, "\x40\x55\x41\x54\x41\x55\x41\x56\x41\x57"),
    HELPER("setBuiltinVar", 0xf17460, "\x48\x89\x5c\x24\x08\x48\x89\x6c\x24\x10\x48\x89\x74\x24\x18"),
    HELPER("withBegin", 0xf2d960, "\x48\x89\x5c\x24\x08\x48\x89\x6c\x24\x10\x48\x89\x74\x24\x18\x57"),
    HELPER("withNext", 0xf2e080, "\x48\x83\x79\x10\x00\x74\x17\x48\x83\x41\x18\xf8"),
    HELPER("withEnd", 0xf2d630, "\x48\x89\x5c\x24\x08\x48\x89\x74\x24\x10\x57\x48\x83\xec\x20"),
    HELPER("free", 0xf088e0, "\x33\xd2\xe9"),
    HELPER("getBuiltinVar", 0xf16380, "\x41\xc7\x41\x0c\xff\xff\xff\x00\x45\x8b\xd0\x81\xfa\x0f\x27"),
};

static void *resolve_helper(const Helper *h) {
    uint8_t *p = (uint8_t *)GetModuleHandleW(NULL) + h->rva;
    if (!in_range(g_text, p) || memcmp(p, h->bytes, h->len)) {
        logf_("New gear: helper %s does not match this game version", h->name);
        return NULL;
    }
    return p;
}

// ---------------------------------------------------------- GML helpers ----

#define NO_INDEX ((int)0x80000000)  // array index for non-array builtin variables

static RValue obj_ref(int idx) {
    RValue r = { .i64 = 0x0100000000000000ll | (uint32_t)idx, .flags = 0, .kind = KIND_REF };
    return r;
}

static int is_obj_ref(const RValue *rv, int idx) {
    return idx >= 0 && rv && (rv->kind & 0xffffff) == KIND_REF && (int32_t)rv->i64 == idx;
}

static int is_instance(const RValue *rv) {
    double v = -4;
    if ((rv->kind & 0xffffff) == KIND_REF) return 1;
    return rv_get(rv, &v) && v >= 0;
}

static int find_object(const char *name) {
    for (uint32_t i = 0; i < g_obj_count; i++)
        if (g_obj_names[i] && !strcmp(g_obj_names[i], name)) return (int)i;
    return -1;
}

static RValue *make_real(RValue *rv, double v) {
    rv->real = v;
    rv->flags = 0;
    rv->kind = KIND_REAL;
    return rv;
}

// Constant GML string, created once per call site (s must be a literal)
#define GSTR(s) ({ static RValue rv_; static int made_; if (!made_) made_ = 1, g_make_string(&rv_, s); rv_; })

// Builtins the game calls by index have a slot in the variable table. Others
// (YYC calls them directly) are found through their registration in the runtime:
// lea rdx, <function>; lea rcx, "<name>"; ... call Function_Add
typedef void (*BuiltinFn)(RValue *res, void *self, void *other, int argc, RValue *args);
typedef struct { int32_t *slot; BuiltinFn fn; } Builtin;

static BuiltinFn find_builtin_fn(const char *name) {
    size_t len = strlen(name) + 1;
    for (uint8_t *s = g_rdata.start + 1; s + len <= g_rdata.end; s++) {
        if (*s != (uint8_t)*name || s[-1] || memcmp(s, name, len)) continue;
        for (uint8_t *q = g_text.start + 7; q + 7 <= g_text.end; q++)
            if (q[0] == 0x48 && q[1] == 0x8d && q[2] == 0x0d && rel32_target(q + 3) == s &&
                q[-7] == 0x48 && q[-6] == 0x8d && q[-5] == 0x15)
                return (BuiltinFn)rel32_target(q - 4);
    }
    return NULL;
}

static Builtin find_builtin(const char *name) {
    Builtin b = { builtin_slot(name), NULL };
    if (!b.slot) b.fn = find_builtin_fn(name);
    if (!b.slot && !b.fn) logf_("ERROR: builtin %s not found", name);
    return b;
}

// Builtin handle with the lookup cached per call site (name must be a literal)
#define BI(name) ({ static Builtin b_; static int tried_; if (!tried_) tried_ = 1, b_ = find_builtin(name); b_; })

static RValue call_bi(void *self, void *other, Builtin b, int argc, RValue *args) {
    RValue res = { .kind = 5 };  // undefined
    if (b.slot && *b.slot >= 0) {
        RValue *ptrs[10];
        for (int k = 0; k < argc && k < 10; k++) ptrs[k] = &args[k];
        g_call_builtin(self, other, &res, argc, *b.slot, ptrs);
    } else if (b.fn) {
        b.fn(&res, self, other, argc, args);
    }
    return res;
}

static RValue inst_get_rv(void *self, void *other, RValue inst, RValue var) {
    RValue a[2] = { inst, var };
    return call_bi(self, other, BI("variable_instance_get"), 2, a);
}

static double inst_get(void *self, void *other, RValue inst, RValue var) {
    RValue r = inst_get_rv(self, other, inst, var);
    double v = 0;
    rv_get(&r, &v);
    return v;
}

// Runs fn(instance pointer) for each instance of target, like GML "with"
static void with_instances(void *self, void *other, RValue target, void (*fn)(void *inst, void *ctx), void *ctx) {
    if (!g_with_begin || !g_with_next || !g_with_end) return;
    struct { uint8_t head[16]; void *list; uint8_t tail[40]; } it = { 0 };
    void *s = self, *o = other;
    if (g_with_begin(&it, &s, &o, &target) > 0)
        do fn(s, ctx); while (g_with_next(&it, &s, &o));
    g_with_end(&it, &s, &o);
    if (it.list && g_yy_free) g_yy_free(it.list);
}

static void inst_set(void *self, void *other, RValue inst, RValue var, RValue value) {
    RValue a[3] = { inst, var, value };
    call_bi(self, other, BI("variable_instance_set"), 3, a);
}

static void set_builtin(void *inst, int id, int index, RValue *value) {
    if (id >= 0 && g_set_builtin_var) g_set_builtin_var(inst, id, index, value);
}

static RValue get_builtin(void *inst, int id) {
    RValue v = { .kind = 5 };  // undefined
    if (id >= 0 && g_get_builtin_var) g_get_builtin_var(inst, id, NO_INDEX, &v);
    return v;
}

static void set_builtin_real(void *inst, int id, int index, double value) {
    RValue v;
    set_builtin(inst, id, index, make_real(&v, value));
}

static void set_self_var(void *inst, int id, double value) {
    if (id >= 0) rv_set(g_get_ref(inst, id), value);
}

// sprite_add(file, frames, 0, 1, xorig, yorig); returns 1 if loaded
static int add_sprite(void *self, void *other, const char *file, double frames, double ox, double oy, RValue *out) {
    RValue a[6];
    g_make_string(&a[0], file);
    make_real(&a[1], frames);
    make_real(&a[2], 0);
    make_real(&a[3], 1);
    make_real(&a[4], ox);
    make_real(&a[5], oy);
    *out = call_bi(self, other, BI("sprite_add"), 6, a);
    double idx = -1;
    rv_get(out, &idx);
    logf_("Sprite %s loaded -> sprite %g", file, idx);
    return idx >= 0;
}

// The game tints the white ship with the player's image_blend (shield color);
// "shield" gives the same color, anything else is RRGGBB
static int resolve_color(void *self, void *other, const char *spec, double *blend) {
    if (!spec) return 0;
    if (!_strnicmp(spec, "shield", 6)) {
        if (g_obj_player < 0) return 0;
        RValue a[2] = { obj_ref(g_obj_player) };
        make_real(&a[1], 0);
        RValue player = call_bi(self, other, BI("instance_find"), 2, a);
        if (!is_instance(&player)) return 0;
        *blend = inst_get(self, other, player, GSTR("image_blend"));
        return 1;
    }
    unsigned rgb = (unsigned)strtoul(spec, NULL, 16);
    *blend = (double)(((rgb & 0xff) << 16) | (rgb & 0xff00) | ((rgb >> 16) & 0xff));  // BGR
    return 1;
}

