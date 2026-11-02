// ------------------------------------------------------------- New gear -----
// Each new gear gets its own upgrade object (g<Name>, a clone of its base gear
// object). When picked it runs the base gear's behavior so every game function
// knows it; a flag switches texts, icon, values and skin.
// INI: [newbody]/[newweapon] (texts), [body.<name>]/[weapon.<name>] (values).

// ------------------------------------------------------------ Gear state ----

typedef struct {
    const char *section;    // INI section with texts
    const char *kind;       // "body" / "weapon" (value section [kind.<name>])
    const char *list_var;   // selection list in oPowers
    int id, base;           // object indices: own upgrade object, base gear (gX)
    int base_type;          // bodyType or object index of the base weapon (oX)
    int selected;           // picked in this run
    int applied;            // value section currently applied (for restoring)
    GmlEventFn o_base_create, o_base_alarm;
    char obj_name[64], base_obj[64];
    ObjEvent events[2];
    ObjDef def;
    RValue icon;
    int icon_state;         // 0 = not loaded, 1 = loaded, -1 = failed
    struct { int32_t *slot; double value; } saved[32];
    int saved_count;
    const char *name, *icon_file;  // cached INI texts (newgear_config)
    char values[96];        // value section name
} NewGear;

// 0 = body, 1 = weapon, 2 = the Molting wild mod (molting.inc; only texts, icon and offer here)
static NewGear g_gear[3] = {
    { .section = "newbody", .kind = "body", .list_var = "gearBodies", .id = -1, .base = -1, .base_type = 1 },
    { .section = "newweapon", .kind = "weapon", .list_var = "gearWeapons", .id = -1, .base = -1, .base_type = -1 },
    { .section = "molting", .kind = "mod", .list_var = "wildModsRare", .id = -1, .base = -1, .base_type = -1 },
};
static void molting_run_start(void *powers, void *other);  // molting.inc

static const char *g_shot_file, *g_shot_color;

static const char *gear_text(NewGear *g, const char *key) { return gear_text_get(g->section, key); }
static int gear_enabled(NewGear *g) { return g->id >= 0 && g->base >= 0 && g->name; }

static void newgear_config(void) {
    for (int i = 0; i < 3; i++) {
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
    for (int i = 0; i < 3 && argc >= 1 && argc <= 8; i++) {
        NewGear *g = &g_gear[i];
        if (!gear_enabled(g)) continue;
        // UI asking for the base while the new gear is active (pause menu, gear list) gets
        // the new gear's answer. The game never offers the base gear itself (not for mods).
        if (i < 2 && g_powers && is_obj_ref(argv[0], g->base) && gear_active(g, g_powers)) {
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
    // Diagnostics: which GML function asks for the Molting icon (once per caller)
    static void *callers[16];
    void *caller = __builtin_return_address(0);
    int known = 0;
    for (int i = 0; i < 16 && callers[i]; i++) known |= callers[i] == caller;
    if (g == &g_gear[2] && !known) {
        for (int i = 0; i < 16; i++)
            if (!callers[i]) { callers[i] = caller; break; }
        logf_("Sprite request for %s from %s (kind %u)", g->name, function_name_at(caller),
              ((RValue *)argv[0])->kind & 0xffffff);
    }
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
// The own upgrade object runs the base behavior and flags the new gear;
// the real base gear clears the flag.
#define GEAR_EVENTS(i) \
    static void ev_gear_create_##i(void *self, void *other) { \
        g_gear[i].selected = 1; \
        g_gear[i].o_base_create(self, other); \
    } \
    static void ev_gear_alarm_##i(void *self, void *other) { \
        g_gear[i].selected = 1; \
        logf_("New gear picked: %s", g_gear[i].name); \
        g_gear[i].o_base_alarm(self, other); \
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
    spawn_probe(self, other);
    molting_run_start(self, other);
    for (int i = 0; i < 3; i++) {
        NewGear *g = &g_gear[i];
        if (!gear_enabled(g)) continue;
        int list_id = var_id(g->list_var);
        if (list_id < 0) {
            logf_("New gear: %s not found", g->list_var);
            continue;
        }
        RValue a[2] = { *g_get_ref(self, list_id), obj_ref(g->id) };
        call_bi(self, other, BI("ds_list_add"), 2, a);
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
    if (!objects_available()) { logf_("New gear: object factory unavailable, disabled"); return; }

    // Base: body is always the standard form, weapon comes from [newweapon] base=<name>
    const char *wb = gear_text(&g_gear[1], "base");
    const char *bases[2] = { "BodyDefault", wb ? wb : "" };
    GmlEventFn gear_create[2] = { ev_gear_create_0, ev_gear_create_1 }, gear_alarm[2] = { ev_gear_alarm_0, ev_gear_alarm_1 };
    void *base_create[2] = { hk_base_create_0, hk_base_create_1 }, *base_alarm[2] = { hk_base_alarm_0, hk_base_alarm_1 };
    for (int i = 0; i < 2; i++) {
        NewGear *g = &g_gear[i];
        if (!g->name || !*bases[i]) continue;
        char type_obj[80];
        snprintf(g->base_obj, sizeof g->base_obj, "g%s", bases[i]);
        snprintf(type_obj, sizeof type_obj, "o%s", bases[i]);  // weapon projectile object
        g->base = find_object(g->base_obj);
        if (i == 1) g->base_type = find_object(type_obj);
        if (g->base < 0 || g->base_type < 0) {
            logf_("New gear %s: objects not found (%s)", g->name, g->base_obj);
            continue;
        }
        g->o_base_create = hook_event(g->base_obj, "Create_0", base_create[i]);
        g->o_base_alarm = hook_event(g->base_obj, "Alarm_0", base_alarm[i]);
        if (!g->o_base_create || !g->o_base_alarm) continue;
        // Own upgrade object, created while data.win loads; g->id is set then
        snprintf(g->obj_name, sizeof g->obj_name, "g%s", g->name);
        g->events[0] = (ObjEvent){ EV_CREATE, 0, gear_create[i] };
        g->events[1] = (ObjEvent){ EV_ALARM, 0, gear_alarm[i] };
        g->def = (ObjDef){ g->obj_name, NULL, g->base_obj, 0, g->events, 2, -1, &g->id };
        register_object(&g->def);
        // Recolor/replace the base weapon's projectile
        if (i == 1 && g_shot_file && g_set_builtin_var)
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
