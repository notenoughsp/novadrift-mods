// ------------------------------------------------------------ Neue Gear -----
// Ungenutzte Upgrade-Objekte dienen als Auswahl-IDs (gBeam: Body, mRanger: Waffe).
// Gewaehlt fuehren sie das Verhalten einer Basis-Ausruestung aus, damit jede
// Spielfunktion sie kennt; ein Flag schaltet Texte, Icon, Werte und Skin um.
// INI: [newbody]/[newweapon] (Texte), [body.<name>]/[weapon.<name>] (Werte).
// Runtime-Helfer sind nur fuer Spielversion 1.2.38.4 verifiziert.

typedef void (*AssignFn)(RValue *dst, const RValue *src);
typedef void (*MakeStringFn)(RValue *out, const char *utf8);
typedef void *(*CallBuiltinFn)(void *self, void *other, RValue *ret, int argc, int64_t fn, RValue **argv);
typedef void (*GmlEventFn)(void *self, void *other);

static AssignFn g_assign;
static MakeStringFn g_make_string;
static CallBuiltinFn g_call_builtin;

typedef struct { const char *name; uint32_t rva; const char *bytes; } Helper;
static const Helper HELPERS[] = {
    { "assign", 0x2c010, "\x40\x53\x48\x83\xec\x30\x48\x8b\xd9" },
    { "makeString", 0xf2cb60, "\x48\x89\x5c\x24\x10\x57\x48\x83\xec\x20" },
    { "callBuiltin", 0xf2d190, "\x40\x55\x41\x54\x41\x55\x41\x56\x41\x57" },
    { "setBuiltinVar", 0xf17460, "\x48\x89\x5c\x24\x08\x48\x89\x6c\x24\x10\x48\x89\x74\x24\x18" },
};

// Setzt eingebaute Instanzvariablen (sprite_index, image_blend, ...)
typedef void (*SetBuiltinVarFn)(void *inst, int id, int array_index, RValue *value);
static SetBuiltinVarFn g_set_builtin_var;
static void *g_powers;  // oPowers-Instanz des laufenden Runs (Build und Werte)

static void *resolve_helper(const Helper *h) {
    uint8_t *p = (uint8_t *)GetModuleHandleW(NULL) + h->rva;
    if (!in_range(g_text, p) || memcmp(p, h->bytes, strlen(h->bytes))) {
        logf_("Neue Gear: Helfer %s passt nicht zu dieser Spielversion", h->name);
        return NULL;
    }
    return p;
}

static RValue obj_ref(int idx) {
    RValue r = { .i64 = 0x0100000000000000ll | (uint32_t)idx, .flags = 0, .kind = KIND_REF };
    return r;
}

static int is_obj_ref(const RValue *rv, int idx) {
    return idx >= 0 && rv && (rv->kind & 0xffffff) == KIND_REF && (int32_t)rv->i64 == idx;
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

typedef struct {
    const char *section;    // INI-Abschnitt mit Texten
    const char *kind;       // "body" / "weapon" (Werte-Abschnitt [kind.<name>])
    const char *id_object;  // ungenutztes Upgrade-Objekt als Auswahl-ID
    const char *list_var;   // Auswahlliste in oPowers
    int id, base;           // Objektindizes: Auswahl-ID, Basis-Gear (gX)
    int base_type;          // bodyType bzw. Objektindex der Basiswaffe (oX)
    int selected;           // in diesem Run gewaehlt
    int applied;            // Werte-Abschnitt gerade aktiv (fuer Wiederherstellung)
    GmlEventFn o_id_create, o_id_alarm, o_base_create, o_base_alarm;
    RValue icon;
    int icon_state;         // 0 = ungeladen, 1 = geladen, -1 = Fehler
    struct { char name[64]; double value; } saved[32];
    int saved_count;
} NewGear;

static NewGear g_gear[2] = {
    { "newbody", "body", "gBeam", "gearBodies", -1, -1, 1 },
    { "newweapon", "weapon", "mRanger", "gearWeapons", -1, -1, -1 },
};

static const char *gear_text(NewGear *g, const char *key) { return gear_text_get(g->section, key); }
static int gear_enabled(NewGear *g) { return g->id >= 0 && g->base >= 0 && gear_text(g, "name"); }

static int gear_type_now(NewGear *g, void *self) {
    return (int)self_var(self, g == &g_gear[0] ? "bodyType" : "weaponType");
}

static int gear_active(NewGear *g, void *self) {
    return gear_enabled(g) && g->selected && gear_type_now(g, self) == g->base_type;
}

// Werte-Abschnitt anwenden; Originalwerte werden beim ersten Mal gesichert
static void gear_apply(NewGear *g, void *self, int verbose) {
    char sec[96];
    snprintf(sec, sizeof sec, "%s.%s", g->kind, gear_text(g, "name"));
    if (!g->applied) {
        g->saved_count = 0;
        for (size_t i = 0; i < g_ovr_count && g->saved_count < 32; i++) {
            if (_stricmp(g_ovr[i].section, sec) || var_id(g_ovr[i].name) < 0) continue;
            snprintf(g->saved[g->saved_count].name, 64, "%s", g_ovr[i].name);
            g->saved[g->saved_count++].value = self_var(self, g_ovr[i].name);
        }
        g->applied = 1;
    }
    apply_section(self, sec, verbose);
}

static void gear_restore(NewGear *g, void *self) {
    if (!g->applied) return;
    for (int i = 0; i < g->saved_count; i++) {
        int id = var_id(g->saved[i].name);
        if (id >= 0) rv_set(g_get_ref(self, id), g->saved[i].value);
    }
    g->applied = 0;
}

// Vor assignBaseStats: Grundwerte (z. B. dartBaseDamage) setzen, die das Spiel dort verrechnet
static void gear_pre_assign(void *self) {
    for (int i = 0; i < 2; i++) {
        if (gear_active(&g_gear[i], self)) gear_apply(&g_gear[i], self, 0);
        else gear_restore(&g_gear[i], self);
    }
}

// Upgrade-Info-Funktionen: fuer neue Gear wie die Basis-Gear antworten
#define UPGRADE_HOOKS(X) \
    X(FullString) X(HexGroup) X(HexSlot) X(IsRecursive) X(IsSuper) X(IsTier1) X(ShortString) \
    X(Sprite) X(SuperModList) X(SuperModSprites) X(TagList) X(TitleString) X(UnlockString)

#define X(n) static GmlFn o_up_##n;
UPGRADE_HOOKS(X)
#undef X

// Ruft orig auf; ist argv[0] eine neue Gear, mit ihrer Basis als Ersatz. Liefert die Gear oder NULL.
static NewGear *call_upgrade(GmlFn orig, void *self, void *other, void *ret, int argc, void **argv, void **res) {
    for (int i = 0; i < 2 && argc >= 1 && argc <= 8; i++) {
        NewGear *g = &g_gear[i];
        if (!gear_enabled(g)) continue;
        // Fragt die Anzeige nach der Basis, waehrend die neue Gear aktiv ist (z. B. Pause-Menue,
        // Gear-Liste), antworten wir als neue Gear. Die Basis selbst bietet das Spiel nie an.
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

// Ersetzt den Rueckgabe-String; Sprache wird am Original-Text der Basis erkannt
static void replace_text(NewGear *g, RValue *ret, const char *key) {
    char k[40];
    const char *orig = (ret->kind & 0xffffff) == 1 && ret->ptr ? *(const char **)ret->ptr : "";
    int german = !strncmp(orig, "FORM", 4) || !strncmp(orig, "WAFFE", 5) || strstr(orig, "Hülle") ||
                 strstr(orig, "Schaden") || strstr(orig, "Antrieb");
    snprintf(k, sizeof k, "%s.%s", key, german ? "de" : "en");
    const char *text = gear_text(g, k);
    if (!text) text = gear_text(g, key);
    if (!text) return;
    // GML-Strings einmal anlegen und wiederverwenden (die Getter laufen jeden Frame)
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

// Eigenes Hex-Icon: einmal per sprite_add laden (Ursprung oben links wie die Originale)
static void *hk_up_Sprite(void *self, void *other, void *ret, int argc, void **argv) {
    void *r;
    NewGear *g = call_upgrade(o_up_Sprite, self, other, ret, argc, argv, &r);
    const char *file = g ? gear_text(g, "icon") : NULL;
    if (!file || g->icon_state < 0) return r;
    if (!g->icon_state) {
        int add_id = builtin_id("sprite_add");
        if (add_id < 0) { g->icon_state = -1; logf_("Neue Gear: sprite_add nicht gefunden"); return r; }
        RValue a[6];
        g_make_string(&a[0], file);
        make_real(&a[1], 1);
        make_real(&a[2], 0);
        make_real(&a[3], 1);
        make_real(&a[4], 0);
        make_real(&a[5], 0);
        RValue *args[6] = { &a[0], &a[1], &a[2], &a[3], &a[4], &a[5] };
        g_call_builtin(self, other, &g->icon, 6, add_id, args);
        double idx = -1;
        rv_get(&g->icon, &idx);
        g->icon_state = idx >= 0 ? 1 : -1;
        logf_("Neue Gear: Icon %s geladen -> Sprite %g", file, idx);
        if (g->icon_state < 0) return r;
    }
    g_assign(ret, &g->icon);
    return r;
}

// bodyEnumToUpgrade / weaponObjectToUpgrade: Basis -> neue Gear, solange sie gewaehlt ist
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

// Events: Auswahl-ID fuehrt das Basis-Verhalten aus und markiert die neue Gear,
// die echte Basis-Gear hebt die Markierung wieder auf.
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
        logf_("Neue Gear gewaehlt: %s", gear_text(g, "name")); \
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

// Run-Start: neue Gear in die Auswahllisten (gearBodies / gearWeapons) aufnehmen
static GmlEventFn o_powers_create;

static void hk_powers_create(void *self, void *other) {
    for (int i = 0; i < 2; i++) g_gear[i].selected = 0, g_gear[i].applied = 0;
    o_powers_create(self, other);
    g_powers = self;
    static int add_id = -2;
    if (add_id == -2) add_id = builtin_id("ds_list_add");
    for (int i = 0; i < 2; i++) {
        NewGear *g = &g_gear[i];
        if (!gear_enabled(g)) continue;
        int list_id = var_id(g->list_var);
        if (list_id < 0 || add_id < 0) {
            logf_("Neue Gear: %s (%d) / ds_list_add (%d) nicht gefunden", g->list_var, list_id, add_id);
            continue;
        }
        RValue item = obj_ref(g->id), res = { 0 };
        RValue *args[2] = { g_get_ref(self, list_id), &item };
        g_call_builtin(self, other, &res, 2, add_id, args);
        logf_("Neue Gear '%s' zur Auswahl (%s) hinzugefuegt", gear_text(g, "name"), g->list_var);
    }
}

// Aussehen: Skin des neuen Bodys aktivieren, solange er gewaehlt ist
static GmlFn g_set_skin_data, o_save_settings;
static int g_skin_active, g_skin_index = -1;
static double g_user_skin;

static int skin_row(const char *name) {
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\custom_skins\\custom_skins.csv", g_dir);
    FILE *f = _wfopen(path, L"r");
    if (!f || !name) { if (f) fclose(f); return -1; }
    char line[512];
    int row = -1, found = -1;  // Kopfzeile = -1, erste Skin-Zeile = 0
    while (fgets(line, sizeof line, f)) {
        char *comma = strchr(line, ',');
        if (comma && row >= 0 && (size_t)(comma - line) == strlen(name) && !strncmp(line, name, comma - line)) {
            found = row;
            break;
        }
        row++;
    }
    fclose(f);
    return found < 0 ? -1 : found + 1;  // global.customSkin ist 1-basiert
}

static void set_skin(void *self, void *other, int active) {
    RValue *rv = global_ref("customSkin");
    if (!rv || !g_set_skin_data) return;
    if (active) {
        if (g_skin_index < 0) g_skin_index = skin_row(gear_text(&g_gear[0], "skin"));
        if (g_skin_index < 0) { logf_("Neuer Body: Skin nicht in custom_skins.csv gefunden"); return; }
        rv_get(rv, &g_user_skin);
        g_set_real(rv, g_skin_index);
    } else
        g_set_real(rv, g_user_skin);
    g_skin_active = active;
    RValue res = { 0 };
    g_set_skin_data(self, other, &res, 0, NULL);
    logf_("Neuer Body: Skin %s (customSkin=%g)", active ? "aktiviert" : "zurueckgesetzt",
          active ? (double)g_skin_index : g_user_skin);
}

static void update_skin(void *self, void *other, int want) {
    if (want != g_skin_active) set_skin(self, other, want);
}

// Einstellungen speichern: die eigene Skin-Wahl sichern, nicht den Skin des neuen Bodys
static void *hk_save_settings(void *self, void *other, void *ret, int argc, void **argv) {
    RValue *rv = g_skin_active ? global_ref("customSkin") : NULL;
    double cur = 0;
    if (rv) { rv_get(rv, &cur); g_set_real(rv, g_user_skin); }
    void *r = o_save_settings(self, other, ret, argc, argv);
    if (rv) g_set_real(rv, cur);
    return r;
}

static GmlEventFn hook_event(const char *object, const char *event, void *detour) {
    char fn[96], label[64];
    snprintf(fn, sizeof fn, "gml_Object_%s_%s", object, event);
    snprintf(label, sizeof label, "%s_%s", object, event);
    return (GmlEventFn)hook(find_gml_function(fn), detour, label);
}

// Eigenes Aussehen der Schuesse: [newweapon] shot=<png>, shot_origin_x/y, shot_color=RRGGBB
static GmlEventFn o_shot_create;
static RValue g_shot_sprite;
static int g_shot_state;  // 0 = ungeladen, 1 = geladen, -1 = Fehler

// Farbwert fuer image_blend: "shield" = Schildfarbe des Spielers, sonst RRGGBB (siehe whip.inc)
static int resolve_color(void *self, void *other, const char *spec, double *blend);

static void set_builtin(void *inst, const char *name, RValue *value) {
    int id = var_id(name);
    if (id >= 0 && g_set_builtin_var) g_set_builtin_var(inst, id, (int)0x80000000, value);
}

static void hk_shot_create(void *self, void *other) {
    o_shot_create(self, other);
    NewGear *g = &g_gear[1];
    const char *file = gear_text(g, "shot");
    if (!file || !g_powers || !gear_active(g, g_powers) || g_shot_state < 0) return;
    if (!g_shot_state) {
        int add_id = builtin_id("sprite_add");
        const char *ox = gear_text(g, "shot_origin_x"), *oy = gear_text(g, "shot_origin_y");
        RValue a[6];
        g_make_string(&a[0], file);
        make_real(&a[1], 1);
        make_real(&a[2], 0);
        make_real(&a[3], 1);
        make_real(&a[4], ox ? atof(ox) : 0);
        make_real(&a[5], oy ? atof(oy) : 0);
        RValue *args[6] = { &a[0], &a[1], &a[2], &a[3], &a[4], &a[5] };
        if (add_id >= 0) g_call_builtin(self, other, &g_shot_sprite, 6, add_id, args);
        double idx = -1;
        rv_get(&g_shot_sprite, &idx);
        g_shot_state = idx >= 0 ? 1 : -1;
        logf_("Neue Gear: Schuss-Sprite %s geladen -> Sprite %g", file, idx);
        if (g_shot_state < 0) return;
    }
    set_builtin(self, "sprite_index", &g_shot_sprite);
    double blend;
    if (resolve_color(self, other, gear_text(g, "shot_color"), &blend)) {
        RValue c;
        set_builtin(self, "image_blend", make_real(&c, blend));
    }
}

static void install_newgear(void) {
    g_assign = (AssignFn)resolve_helper(&HELPERS[0]);
    g_make_string = (MakeStringFn)resolve_helper(&HELPERS[1]);
    g_call_builtin = (CallBuiltinFn)resolve_helper(&HELPERS[2]);
    g_set_builtin_var = (SetBuiltinVarFn)resolve_helper(&HELPERS[3]);
    if (!g_assign || !g_make_string || !g_call_builtin) { logf_("Neue Gear: deaktiviert"); return; }

    // Basis: Body immer Standardform, Waffe aus [newweapon] base=<Name> (z. B. Dart)
    static char weapon_base[64];
    const char *wb = gear_text(&g_gear[1], "base");
    snprintf(weapon_base, sizeof weapon_base, "%s", wb ? wb : "");
    const char *bases[2] = { "BodyDefault", weapon_base };
    void *id_create[2] = { hk_id_create_0, hk_id_create_1 }, *id_alarm[2] = { hk_id_alarm_0, hk_id_alarm_1 };
    void *base_create[2] = { hk_base_create_0, hk_base_create_1 }, *base_alarm[2] = { hk_base_alarm_0, hk_base_alarm_1 };
    for (int i = 0; i < 2; i++) {
        NewGear *g = &g_gear[i];
        if (!gear_text(g, "name") || !*bases[i]) continue;
        char base_obj[80];
        snprintf(base_obj, sizeof base_obj, "g%s", bases[i]);
        g->id = find_object(g->id_object);
        g->base = find_object(base_obj);
        if (i == 1) {
            char weapon_obj[80];
            snprintf(weapon_obj, sizeof weapon_obj, "o%s", bases[i]);
            g->base_type = find_object(weapon_obj);
        }
        if (g->id < 0 || g->base < 0 || g->base_type < 0) {
            logf_("Neue Gear %s: Objekte nicht gefunden (%s/%s)", gear_text(g, "name"), g->id_object, base_obj);
            g->id = -1;
            continue;
        }
        g->o_base_create = hook_event(base_obj, "Create_0", base_create[i]);
        g->o_base_alarm = hook_event(base_obj, "Alarm_0", base_alarm[i]);
        g->o_id_create = hook_event(g->id_object, "Create_0", id_create[i]);
        g->o_id_alarm = hook_event(g->id_object, "Alarm_0", id_alarm[i]);
        if (!g->o_base_create || !g->o_base_alarm || !g->o_id_create || !g->o_id_alarm) g->id = -1;
        // Projektil der Basiswaffe (oX) einfaerben/ersetzen
        if (i == 1 && g->id >= 0 && gear_text(g, "shot") && g_set_builtin_var) {
            char shot_obj[80];
            snprintf(shot_obj, sizeof shot_obj, "o%s", bases[i]);
            o_shot_create = hook_event(shot_obj, "Create_0", hk_shot_create);
        }
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

// Nach assignBaseStats: Werte der aktiven neuen Gear anwenden. Liefert 1, wenn aktiv.
static int newgear_post_assign(void *self, int which, int verbose) {
    NewGear *g = &g_gear[which];
    if (!gear_active(g, self)) return 0;
    if (verbose) logf_("  Neue Gear aktiv: %s", gear_text(g, "name"));
    gear_apply(g, self, verbose);
    return 1;
}

static void newgear_update_skin(void *self, void *other) {
    if (gear_enabled(&g_gear[0])) update_skin(self, other, gear_active(&g_gear[0], self));
}
