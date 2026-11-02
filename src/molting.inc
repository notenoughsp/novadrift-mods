// --------------------------------------------------------------- Molting ----
// Wild mod (rare). Below molting_threshold hull the ship sheds its shell: the
// husk (oMoltingHusk, a grey copy of the ship sprite) stays behind for a few
// seconds and absorbs enemy projectiles that touch it. Right after molting the
// ship takes more damage (soft shell). Cooldown between molts.
// The mod object mMolting is a clone of a base wild mod (texts and icon of its own,
// other upgrade info from the base). Values and texts: [molting] in the profile.
// The husk's burst (stronger per absorbed projectile) is not built yet.

#define MOLT (&g_gear[2])

static struct {
    int on, force_offer, duration, absorb_max, cooldown, softshell_time;
    double threshold, absorb_bonus, softshell;
} g_mc;

static struct {
    int owned, cooldown, softshell;  // frames left
    void *husk;
    RValue husk_id;
    int husk_frames, absorbed;
} g_molt;

static GmlEventFn g_molt_base_create;
static GmlFn g_gain_mod, o_hull_direct, o_hull_dot;
static int g_obj_husk = -1, g_molt_shot = -1;

static double molt_param(const char *key, double def) {
    const char *v = gear_text(MOLT, key);
    return v ? atof(v) : def;
}

static void molting_config(void) {
    g_mc.on = MOLT->name != NULL;
    g_mc.force_offer = (int)molt_param("molting_force_offer", 0);
    g_mc.threshold = molt_param("molting_threshold", 0.3);
    g_mc.duration = (int)molt_param("molting_duration", 240);
    g_mc.absorb_max = (int)molt_param("molting_absorb_max", 12);
    g_mc.absorb_bonus = molt_param("molting_absorb_bonus", 0.08);
    g_mc.cooldown = (int)molt_param("molting_cooldown", 2700);
    g_mc.softshell = molt_param("molting_softshell", 1.25);
    g_mc.softshell_time = (int)molt_param("molting_softshell_time", 180);
    if (g_mc.duration < 1) g_mc.duration = 1;
}

// ------------------------------------------------------------ Mod object ----
// Like the game's wild mods: Create sets alarm[0], Alarm 0 applies the mod in
// oPowers (wildModsOwned, gainMod: owned list, out of the pool) and destroys itself.

static void ev_molt_create(void *self, void *other) { g_molt_base_create(self, other); }

static void ev_molt_pick(void *self, void *other) {
    int pool = var_id(MOLT->list_var);
    if (g_powers && pool >= 0) {
        set_self_var(g_powers, VAR("wildModsOwned"), get_var(g_powers, VAR("wildModsOwned")) + 1);
        RValue a[2] = { *g_get_ref(g_powers, pool), obj_ref(MOLT->id) }, res = { .kind = 5 };
        void *argv[2] = { &a[0], &a[1] };
        g_gain_mod(g_powers, self, &res, 2, argv);
    }
    g_molt.owned = 1;
    logf_("Molting picked");
    call_bi(self, other, BI("instance_destroy"), 0, NULL);
}

// Run start (oPowers create): reset, offer in the rare wild pool (all pools for testing)
static void molting_run_start(void *powers, void *other) {
    memset(&g_molt, 0, sizeof g_molt);
    if (!g_mc.on || !g_mc.force_offer || MOLT->id < 0) return;
    const char *extra[] = { "wildMods", "wildModsVeryRare" };
    for (int i = 0; i < 2; i++) {
        int list = var_id(extra[i]);
        if (list < 0) continue;
        RValue a[2] = { *g_get_ref(powers, list), obj_ref(MOLT->id) };
        call_bi(powers, other, BI("ds_list_add"), 2, a);
    }
    logf_("Molting: offered in all wild pools (molting_force_offer)");
}

// ------------------------------------------------------------------ Husk ----

static void husk_absorb(void *p, void *ctx) {
    if (g_molt.absorbed >= g_mc.absorb_max) return;
    RValue a[3] = { get_builtin(p, VAR("x")), get_builtin(p, VAR("y")), g_molt.husk_id };
    RValue hit = call_bi(p, p, BI("place_meeting"), 3, a);
    double touching = 0;
    if (!rv_get(&hit, &touching) || !touching) return;
    call_bi(p, p, BI("instance_destroy"), 0, NULL);
    g_molt.absorbed++;
}

static void husk_end(void *self, void *other, const char *why) {
    logf_("Molting: husk %s, %d projectiles absorbed (burst x%.2f, not built yet)", why, g_molt.absorbed,
          1 + g_molt.absorbed * g_mc.absorb_bonus);
    g_molt.husk = NULL;
    call_bi(self, other, BI("instance_destroy"), 0, NULL);
}

static void ev_husk_create(void *self, void *other) { g_molt.husk = self; }

static void ev_husk_step(void *self, void *other) {
    if (self != g_molt.husk) return;
    if (g_molt_shot >= 0) with_instances(self, other, obj_ref(g_molt_shot), husk_absorb, NULL);
    set_builtin_real(self, VAR("image_alpha"), NO_INDEX, 0.2 + 0.5 * g_molt.husk_frames / g_mc.duration);
    if (g_molt.absorbed >= g_mc.absorb_max) husk_end(self, other, "cracked");
    else if (--g_molt.husk_frames <= 0) husk_end(self, other, "expired");
}

static const ObjEvent HUSK_EVENTS[] = {
    { EV_CREATE, 0, ev_husk_create },
    { EV_STEP, 0, ev_husk_step },
};
static ObjDef g_husk_def = { "oMoltingHusk", NULL, NULL, 1, HUSK_EVENTS, 2, -1, &g_obj_husk };

// Shed the shell at the player's position with the player's look
static void molt(void *player, void *other) {
    RValue a[4] = { get_builtin(player, VAR("x")), get_builtin(player, VAR("y")) };
    double depth = 0;
    RValue d = get_builtin(player, VAR("depth"));
    rv_get(&d, &depth);
    make_real(&a[2], depth + 1);  // just behind the ship
    a[3] = obj_ref(g_obj_husk);
    g_molt.husk = NULL;
    g_molt.husk_id = call_bi(player, other, BI("instance_create_depth"), 4, a);
    void *h = g_molt.husk;
    if (!h) return;
    const char *copy[] = { "sprite_index", "image_index", "image_angle", "image_xscale", "image_yscale" };
    for (int i = 0; i < 5; i++) {
        int id = var_id(copy[i]);
        RValue v = get_builtin(player, id);
        set_builtin(h, id, NO_INDEX, &v);
    }
    set_builtin_real(h, VAR("image_speed"), NO_INDEX, 0);
    set_builtin_real(h, VAR("image_blend"), NO_INDEX, 0x909090);
    set_builtin_real(h, VAR("image_alpha"), NO_INDEX, 0.7);
    g_molt.husk_frames = g_mc.duration;
    g_molt.absorbed = 0;
    g_molt.cooldown = g_mc.cooldown;
    g_molt.softshell = g_mc.softshell_time;
}

// Called every player step (whip.inc)
static void molting_step(void *self, void *other) {
    if (!g_molt.owned || g_obj_husk < 0 || !g_powers) return;
    if (g_molt.cooldown > 0) g_molt.cooldown--;
    if (g_molt.softshell > 0) g_molt.softshell--;
    if (g_molt.husk || g_molt.cooldown > 0) return;
    double hull = get_var(g_powers, VAR("hull")), max = get_var(g_powers, VAR("hullMax"));
    if (max <= 0 || hull <= 0 || hull >= max * g_mc.threshold) return;
    molt(self, other);
    logf_("Molting: shed shell at %.0f / %.0f hull", hull, max);
}

// ------------------------------------------------------------ Soft shell ----
// hullDamageDirect / hullDamageDOT(damage): more damage while the shell is soft

static void *soft_shell(GmlFn orig, void *self, void *other, void *ret, int argc, void **argv) {
    double damage;
    if (g_molt.softshell <= 0 || argc < 1 || argc > 8 || !rv_get(argv[0], &damage))
        return orig(self, other, ret, argc, argv);
    RValue scaled = { .real = damage * g_mc.softshell, .kind = KIND_REAL };
    void *args[8];
    memcpy(args, argv, argc * sizeof *args);
    args[0] = &scaled;
    return orig(self, other, ret, argc, args);
}

static void *hk_hull_direct(void *self, void *other, void *ret, int argc, void **argv) {
    return soft_shell(o_hull_direct, self, other, ret, argc, argv);
}

static void *hk_hull_dot(void *self, void *other, void *ret, int argc, void **argv) {
    return soft_shell(o_hull_dot, self, other, ret, argc, argv);
}

// --------------------------------------------------------------- Install ----

static void install_molting(void) {
    NewGear *g = MOLT;
    if (!g_mc.on || !objects_available() || !g_assign) return;
    const char *base = gear_text(g, "base");
    snprintf(g->base_obj, sizeof g->base_obj, "%s", base ? base : "mBravado");
    char create[96];
    snprintf(create, sizeof create, "gml_Object_%s_Create_0", g->base_obj);
    g->base = find_object(g->base_obj);
    g_molt_base_create = (GmlEventFn)find_gml_function(create);
    g_gain_mod = (GmlFn)find_gml_function("gml_Script_gainMod");
    g_molt_shot = find_object("oEnemyProjectile");
    if (g->base < 0 || !g_molt_base_create || !g_gain_mod) {
        logf_("Molting: base mod %s or gainMod not found, disabled", g->base_obj);
        return;
    }
    snprintf(g->obj_name, sizeof g->obj_name, "m%s", g->name);
    g->events[0] = (ObjEvent){ EV_CREATE, 0, ev_molt_create };
    g->events[1] = (ObjEvent){ EV_ALARM, 0, ev_molt_pick };
    g->def = (ObjDef){ g->obj_name, NULL, g->base_obj, 0, g->events, 2, -1, &g->id };
    register_object(&g->def);
    register_object(&g_husk_def);
    o_hull_direct = hook(find_gml_function("gml_Script_hullDamageDirect"), hk_hull_direct, "hullDamageDirect");
    o_hull_dot = hook(find_gml_function("gml_Script_hullDamageDOT"), hk_hull_dot, "hullDamageDOT");
    logf_("Molting ready (below %.0f%% hull, %d frames cooldown, absorbs up to %d)", g_mc.threshold * 100,
          g_mc.cooldown, g_mc.absorb_max);
}
