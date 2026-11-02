// ------------------------------------------------------------- New gear -----
// Unused upgrade objects serve as selection IDs (gBeam: body, mRanger: weapon).
// When picked they run a base gear's behavior so every game function knows them;
// a flag switches texts, icon, values and skin.
// INI: [newbody]/[newweapon] (texts), [body.<name>]/[weapon.<name>] (values).
// Runtime helpers are only verified for game version 1.2.38.4.

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

// Calls a builtin by index (see BI)
static RValue call_bi(void *self, void *other, int id, int argc, RValue *args) {
    RValue res = { .kind = 5 };  // undefined
    if (id < 0) return res;
    RValue *ptrs[10];
    for (int k = 0; k < argc && k < 10; k++) ptrs[k] = &args[k];
    g_call_builtin(self, other, &res, argc, id, ptrs);
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

// ------------------------------------------------------------ Gear state ----

typedef struct {
    const char *section;    // INI section with texts
    const char *kind;       // "body" / "weapon" (value section [kind.<name>])
    const char *id_object;  // unused upgrade object used as selection ID
    const char *list_var;   // selection list in oPowers
    int id, base;           // object indices: selection ID, base gear (gX)
    int base_type;          // bodyType or object index of the base weapon (oX)
    int selected;           // picked in this run
    int applied;            // value section currently applied (for restoring)
    GmlEventFn o_id_create, o_id_alarm, o_base_create, o_base_alarm;
    RValue icon;
    int icon_state;         // 0 = not loaded, 1 = loaded, -1 = failed
    struct { int32_t *slot; double value; } saved[32];
    int saved_count;
    const char *name, *icon_file;  // cached INI texts (newgear_config)
    char values[96];        // value section name
} NewGear;

static NewGear g_gear[2] = {
    { "newbody", "body", "gBeam", "gearBodies", -1, -1, 1 },
    { "newweapon", "weapon", "mRanger", "gearWeapons", -1, -1, -1 },
};

static const char *g_shot_file, *g_shot_color;

static const char *gear_text(NewGear *g, const char *key) { return gear_text_get(g->section, key); }
static int gear_enabled(NewGear *g) { return g->id >= 0 && g->base >= 0 && g->name; }

static void newgear_config(void) {
    for (int i = 0; i < 2; i++) {
        NewGear *g = &g_gear[i];
        g->name = gear_text(g, "name");
        g->icon_file = gear_text(g, "icon");
        snprintf(g->values, sizeof g->values, "%s.%s", g->kind, g->name ? g->name : "");
    }
    g_shot_file = gear_text(&g_gear[1], "shot");
    g_shot_color = gear_text(&g_gear[1], "shot_color");
}

static int gear_active(NewGear *g, void *self) {
    if (!gear_enabled(g) || !g->selected) return 0;
    int type = g == &g_gear[0] ? VAR("bodyType") : VAR("weaponType");
    return (int)get_var(self, type) == g->base_type;
}

// Apply the value section; original values are saved on first use
static void gear_apply(NewGear *g, void *self, int verbose) {
    if (!g->applied) {
        g->saved_count = 0;
        for (size_t i = 0; i < g_ovr_count && g->saved_count < 32; i++) {
            Override *o = &g_ovr[i];
            if (_stricmp(o->section, g->values) || !o->slot || *o->slot < 0) continue;
            g->saved[g->saved_count].slot = o->slot;
            g->saved[g->saved_count++].value = get_var(self, *o->slot);
        }
        g->applied = 1;
    }
    apply_section(self, g->values, verbose);
}

static void gear_restore(NewGear *g, void *self) {
    if (!g->applied) return;
    for (int i = 0; i < g->saved_count; i++) set_self_var(self, *g->saved[i].slot, g->saved[i].value);
    g->applied = 0;
}

// Before assignBaseStats: set base values (e.g. blasterBaseDamage) the game folds in there
static void gear_pre_assign(void *self) {
    for (int i = 0; i < 2; i++) {
        if (gear_active(&g_gear[i], self)) gear_apply(&g_gear[i], self, 0);
        else gear_restore(&g_gear[i], self);
    }
}

// ---------------------------------------------------------- Upgrade info ----
// Upgrade info functions: answer for new gear like its base gear
#define UPGRADE_HOOKS(X) \
    X(FullString) X(HexGroup) X(HexSlot) X(IsRecursive) X(IsSuper) X(IsTier1) X(ShortString) \
    X(Sprite) X(SuperModList) X(SuperModSprites) X(TagList) X(TitleString) X(UnlockString)

#define X(n) static GmlFn o_up_##n;
UPGRADE_HOOKS(X)
#undef X

// Calls orig; if argv[0] is new gear, its base is passed instead. Returns the gear or NULL.
static NewGear *call_upgrade(GmlFn orig, void *self, void *other, void *ret, int argc, void **argv, void **res) {
    for (int i = 0; i < 2 && argc >= 1 && argc <= 8; i++) {
        NewGear *g = &g_gear[i];
        if (!gear_enabled(g)) continue;
        // UI asking for the base while the new gear is active (pause menu, gear list) gets
        // the new gear's answer. The game never offers the base gear itself.
        if (g_powers && is_obj_ref(argv[0], g->base) && gear_active(g, g_powers)) {
            *res = orig(self, other, ret, argc, argv);
            return g;
        }
        if (!is_obj_ref(argv[0], g->id)) continue;
        RValue sub = obj_ref(g->base);
        void *args[8];
        memcpy(args, argv, argc * sizeof *args);
        args[0] = &sub;
        *res = orig(self, other, ret, argc, args);
        return g;
    }
    *res = orig(self, other, ret, argc, argv);
    return NULL;
}

// Replaces the returned string with the INI text (title / short / full)
static void replace_text(NewGear *g, RValue *ret, const char *key) {
    const char *text = gear_text(g, key);
    if (!text) return;
    // Create GML strings once and reuse them (the getters run every frame)
    static struct { const char *src; RValue rv; } cache[32];
    int i = 0;
    while (i < 32 && cache[i].src && cache[i].src != text) i++;
    if (i == 32) return;
    if (!cache[i].src) {
        g_make_string(&cache[i].rv, text);
        cache[i].src = text;
    }
    g_assign(ret, &cache[i].rv);
}

#define TEXT_HOOK(n, key) \
    static void *hk_up_##n(void *self, void *other, void *ret, int argc, void **argv) { \
        void *r; \
        NewGear *g = call_upgrade(o_up_##n, self, other, ret, argc, argv, &r); \
        if (g) replace_text(g, ret, key); \
        return r; \
    }
TEXT_HOOK(TitleString, "title")
TEXT_HOOK(ShortString, "short")
TEXT_HOOK(FullString, "full")

#define PLAIN_HOOK(n) \
    static void *hk_up_##n(void *self, void *other, void *ret, int argc, void **argv) { \
        void *r; \
        call_upgrade(o_up_##n, self, other, ret, argc, argv, &r); \
        return r; \
    }
PLAIN_HOOK(HexGroup) PLAIN_HOOK(HexSlot) PLAIN_HOOK(IsRecursive) PLAIN_HOOK(IsSuper) PLAIN_HOOK(IsTier1)
PLAIN_HOOK(SuperModList) PLAIN_HOOK(SuperModSprites) PLAIN_HOOK(TagList) PLAIN_HOOK(UnlockString)

// Custom hex icon: loaded once (origin top left like the originals)
static void *hk_up_Sprite(void *self, void *other, void *ret, int argc, void **argv) {
    void *r;
    NewGear *g = call_upgrade(o_up_Sprite, self, other, ret, argc, argv, &r);
    if (!g || !g->icon_file || g->icon_state < 0) return r;
    if (!g->icon_state) g->icon_state = add_sprite(self, other, g->icon_file, 1, 0, 0, &g->icon) ? 1 : -1;
    if (g->icon_state > 0) g_assign(ret, &g->icon);
    return r;
}

// bodyEnumToUpgrade / weaponObjectToUpgrade: base -> new gear while it is picked
static GmlFn o_body_to_upgrade, o_weapon_to_upgrade;

static void map_to_new(NewGear *g, void *ret) {
    if (gear_enabled(g) && g->selected && is_obj_ref(ret, g->base)) ((RValue *)ret)->i64 = obj_ref(g->id).i64;
}

static void *hk_body_to_upgrade(void *self, void *other, void *ret, int argc, void **argv) {
    void *r = o_body_to_upgrade(self, other, ret, argc, argv);
    map_to_new(&g_gear[0], ret);
    return r;
}

static void *hk_weapon_to_upgrade(void *self, void *other, void *ret, int argc, void **argv) {
    void *r = o_weapon_to_upgrade(self, other, ret, argc, argv);
    map_to_new(&g_gear[1], ret);
    return r;
}

// ---------------------------------------------------------------- Events ----
// The selection ID runs the base behavior and flags the new gear;
// the real base gear clears the flag.
#define GEAR_EVENTS(i) \
    static void hk_id_create_##i(void *self, void *other) { \
        NewGear *g = &g_gear[i]; \
        if (!gear_enabled(g)) { g->o_id_create(self, other); return; } \
        g->selected = 1; \
        g->o_base_create(self, other); \
    } \
    static void hk_id_alarm_##i(void *self, void *other) { \
        NewGear *g = &g_gear[i]; \
        if (!gear_enabled(g)) { g->o_id_alarm(self, other); return; } \
        g->selected = 1; \
        logf_("New gear picked: %s", g->name); \
        g->o_base_alarm(self, other); \
    } \
    static void hk_base_create_##i(void *self, void *other) { \
        g_gear[i].selected = 0; \
        g_gear[i].o_base_create(self, other); \
    } \
    static void hk_base_alarm_##i(void *self, void *other) { \
        g_gear[i].selected = 0; \
        g_gear[i].o_base_alarm(self, other); \
    }
GEAR_EVENTS(0)
GEAR_EVENTS(1)

// Run start: add new gear to the selection lists (gearBodies / gearWeapons)
static GmlEventFn o_powers_create;

static void hk_powers_create(void *self, void *other) {
    for (int i = 0; i < 2; i++) g_gear[i].selected = 0, g_gear[i].applied = 0;
    o_powers_create(self, other);
    g_powers = self;
    int add_id = BI("ds_list_add");
    for (int i = 0; i < 2; i++) {
        NewGear *g = &g_gear[i];
        if (!gear_enabled(g)) continue;
        int list_id = var_id(g->list_var);
        if (list_id < 0 || add_id < 0) {
            logf_("New gear: %s (%d) / ds_list_add (%d) not found", g->list_var, list_id, add_id);
            continue;
        }
        RValue a[2] = { *g_get_ref(self, list_id), obj_ref(g->id) };
        call_bi(self, other, add_id, 2, a);
        logf_("New gear '%s' added to selection (%s)", g->name, g->list_var);
    }
}

// ------------------------------------------------------------------ Skin ----
// Enable the new body's skin while it is picked
static GmlFn g_set_skin_data, o_save_settings;
static int g_skin_active, g_skin_index = -1;
static double g_user_skin;

static int skin_row(const char *name) {
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\custom_skins\\custom_skins.csv", g_dir);
    FILE *f = name ? _wfopen(path, L"r") : NULL;
    if (!f) return -1;
    char line[512];
    size_t len = strlen(name);
    int row = -1, found = -1;  // header = -1, first skin row = 0
    while (fgets(line, sizeof line, f)) {
        if (row >= 0 && !strncmp(line, name, len) && line[len] == ',') { found = row; break; }
        row++;
    }
    fclose(f);
    return found < 0 ? -1 : found + 1;  // global.customSkin is 1-based
}

static void set_skin(void *self, void *other, int active) {
    RValue *rv = global_ref(VAR("customSkin"));
    if (!rv || !g_set_skin_data) return;
    if (active) {
        if (g_skin_index < 0) g_skin_index = skin_row(gear_text(&g_gear[0], "skin"));
        if (g_skin_index < 0) { logf_("New body: skin not found in custom_skins.csv"); return; }
        rv_get(rv, &g_user_skin);
        g_set_real(rv, g_skin_index);
    } else
        g_set_real(rv, g_user_skin);
    g_skin_active = active;
    RValue res = { 0 };
    g_set_skin_data(self, other, &res, 0, NULL);
    logf_("New body: skin %s (customSkin=%g)", active ? "enabled" : "restored",
          active ? (double)g_skin_index : g_user_skin);
}

// Saving settings: store the player's own skin choice, not the new body's skin
static void *hk_save_settings(void *self, void *other, void *ret, int argc, void **argv) {
    RValue *rv = g_skin_active ? global_ref(VAR("customSkin")) : NULL;
    double cur = 0;
    if (rv) { rv_get(rv, &cur); g_set_real(rv, g_user_skin); }
    void *r = o_save_settings(self, other, ret, argc, argv);
    if (rv) g_set_real(rv, cur);
    return r;
}

// ------------------------------------------------------------------ Shot ----
// Custom shot look: [newweapon] shot=<png>, shot_origin_x/y, shot_color
static GmlEventFn o_shot_create;
static RValue g_shot_sprite;
static int g_shot_state;  // 0 = not loaded, 1 = loaded, -1 = failed

static void hk_shot_create(void *self, void *other) {
    o_shot_create(self, other);
    if (!g_shot_file || g_shot_state < 0 || !g_powers || !gear_active(&g_gear[1], g_powers)) return;
    if (!g_shot_state) {
        const char *ox = gear_text(&g_gear[1], "shot_origin_x"), *oy = gear_text(&g_gear[1], "shot_origin_y");
        g_shot_state = add_sprite(self, other, g_shot_file, 1, ox ? atof(ox) : 0, oy ? atof(oy) : 0,
                                  &g_shot_sprite) ? 1 : -1;
        if (g_shot_state < 0) return;
    }
    set_builtin(self, VAR("sprite_index"), NO_INDEX, &g_shot_sprite);
    double blend;
    if (resolve_color(self, other, g_shot_color, &blend)) set_builtin_real(self, VAR("image_blend"), NO_INDEX, blend);
}

// --------------------------------------------------------------- Install ----

static GmlEventFn hook_event(const char *object, const char *event, void *detour) {
    char fn[96], label[64];
    snprintf(fn, sizeof fn, "gml_Object_%s_%s", object, event);
    snprintf(label, sizeof label, "%s_%s", object, event);
    return (GmlEventFn)hook(find_gml_function(fn), detour, label);
}

static void install_newgear(void) {
    g_obj_player = find_object("oPlayer");
    g_assign = (AssignFn)resolve_helper(&HELPERS[0]);
    g_make_string = (MakeStringFn)resolve_helper(&HELPERS[1]);
    g_call_builtin = (CallBuiltinFn)resolve_helper(&HELPERS[2]);
    g_set_builtin_var = (SetBuiltinVarFn)resolve_helper(&HELPERS[3]);
    g_with_begin = (WithBeginFn)resolve_helper(&HELPERS[4]);
    g_with_next = (WithNextFn)resolve_helper(&HELPERS[5]);
    g_with_end = (WithEndFn)resolve_helper(&HELPERS[6]);
    g_yy_free = (FreeFn)resolve_helper(&HELPERS[7]);
    g_get_builtin_var = (GetBuiltinVarFn)resolve_helper(&HELPERS[8]);
    if (!g_assign || !g_make_string || !g_call_builtin) { logf_("New gear: disabled"); return; }

    // Base: body is always the standard form, weapon comes from [newweapon] base=<name>
    const char *wb = gear_text(&g_gear[1], "base");
    const char *bases[2] = { "BodyDefault", wb ? wb : "" };
    void *id_create[2] = { hk_id_create_0, hk_id_create_1 }, *id_alarm[2] = { hk_id_alarm_0, hk_id_alarm_1 };
    void *base_create[2] = { hk_base_create_0, hk_base_create_1 }, *base_alarm[2] = { hk_base_alarm_0, hk_base_alarm_1 };
    for (int i = 0; i < 2; i++) {
        NewGear *g = &g_gear[i];
        if (!g->name || !*bases[i]) continue;
        char base_obj[80], type_obj[80];
        snprintf(base_obj, sizeof base_obj, "g%s", bases[i]);
        snprintf(type_obj, sizeof type_obj, "o%s", bases[i]);  // weapon projectile object
        g->id = find_object(g->id_object);
        g->base = find_object(base_obj);
        if (i == 1) g->base_type = find_object(type_obj);
        if (g->id < 0 || g->base < 0 || g->base_type < 0) {
            logf_("New gear %s: objects not found (%s/%s)", g->name, g->id_object, base_obj);
            g->id = -1;
            continue;
        }
        g->o_base_create = hook_event(base_obj, "Create_0", base_create[i]);
        g->o_base_alarm = hook_event(base_obj, "Alarm_0", base_alarm[i]);
        g->o_id_create = hook_event(g->id_object, "Create_0", id_create[i]);
        g->o_id_alarm = hook_event(g->id_object, "Alarm_0", id_alarm[i]);
        if (!g->o_base_create || !g->o_base_alarm || !g->o_id_create || !g->o_id_alarm) g->id = -1;
        // Recolor/replace the base weapon's projectile
        if (i == 1 && g->id >= 0 && g_shot_file && g_set_builtin_var)
            o_shot_create = hook_event(type_obj, "Create_0", hk_shot_create);
    }

    o_powers_create = (GmlEventFn)hook(find_gml_function("gml_Object_oPowers_Create_0"), hk_powers_create, "oPowers_Create_0");
    o_body_to_upgrade = hook(find_gml_function("gml_Script_bodyEnumToUpgrade"), hk_body_to_upgrade, "bodyEnumToUpgrade");
    o_weapon_to_upgrade = hook(find_gml_function("gml_Script_weaponObjectToUpgrade"), hk_weapon_to_upgrade, "weaponObjectToUpgrade");
#define X(n) o_up_##n = hook(find_gml_function("gml_Script_getUpgrade" #n), hk_up_##n, "getUpgrade" #n);
    UPGRADE_HOOKS(X)
#undef X
    g_set_skin_data = (GmlFn)find_gml_function("gml_Script_setSkinData");
    o_save_settings = hook(find_gml_function("gml_Script_saveSettings"), hk_save_settings, "saveSettings");
}

// After assignBaseStats: apply the active new gear's values. Returns 1 if active.
static int newgear_post_assign(void *self, int which, int verbose) {
    NewGear *g = &g_gear[which];
    if (!gear_active(g, self)) return 0;
    if (verbose) logf_("  New gear active: %s", g->name);
    gear_apply(g, self, verbose);
    return 1;
}

static void newgear_update_skin(void *self, void *other) {
    if (!gear_enabled(&g_gear[0])) return;
    int want = gear_active(&g_gear[0], self);
    if (want != g_skin_active) set_skin(self, other, want);
}
