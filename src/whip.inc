// ------------------------------------------------------------- Tail whip ----
// When an enemy is behind the ship, an animated tail swings through the area
// behind it. Hits use an oVenomStrike (the game's damage logic) with its
// flames, trail and movement disabled. Values: whip_* in [newweapon].

static GmlEventFn o_player_step, o_venom_create;
static int g_obj_player = -1, g_obj_solid = -1, g_obj_venom = -1;
static int g_whip_cooldown;

static double whip_param(const char *key, double def) {
    const char *v = gear_text(&g_gear[1], key);
    return v ? atof(v) : def;
}

// Call a builtin by name (function index is cached)
static RValue call_bi(void *self, void *other, const char *name, int argc, RValue *args) {
    static struct { const char *name; int id; } ids[16];
    int id = -1, i = 0;
    for (; i < 16 && ids[i].name; i++)
        if (!strcmp(ids[i].name, name)) { id = ids[i].id; break; }
    if (id < 0 && i < 16) { id = builtin_id(name); ids[i].name = name; ids[i].id = id; }
    RValue res = { 0 };
    res.kind = 5;  // undefined
    if (id < 0) return res;
    RValue *ptrs[10];
    for (int k = 0; k < argc && k < 10; k++) ptrs[k] = &args[k];
    g_call_builtin(self, other, &res, argc, id, ptrs);
    return res;
}

// Constant GML strings for variable names
static RValue *gml_str(const char *s) {
    static struct { const char *src; RValue rv; } cache[16];
    int i = 0;
    while (i < 16 && cache[i].src && strcmp(cache[i].src, s)) i++;
    if (i == 16) return &cache[15].rv;
    if (!cache[i].src) { g_make_string(&cache[i].rv, s); cache[i].src = s; }
    return &cache[i].rv;
}

static double inst_get(void *self, void *other, RValue inst, const char *var) {
    RValue a[2] = { inst, *gml_str(var) };
    RValue r = call_bi(self, other, "variable_instance_get", 2, a);
    double v = 0;
    rv_get(&r, &v);
    return v;
}

static void inst_set_rv(void *self, void *other, RValue inst, const char *var, RValue value) {
    RValue a[3] = { inst, *gml_str(var), value };
    call_bi(self, other, "variable_instance_set", 3, a);
}

static int is_instance(const RValue *rv) {
    double v = -4;
    if ((rv->kind & 0xffffff) == KIND_REF) return 1;
    return rv_get(rv, &v) && v >= 0;
}

static void set_builtin_at(void *inst, const char *name, int index, double value) {
    RValue v;
    int id = var_id(name);
    if (id >= 0 && g_set_builtin_var) g_set_builtin_var(inst, id, index, make_real(&v, value));
}

static void set_self_var(void *inst, const char *name, double value) {
    int id = var_id(name);
    if (id >= 0) rv_set(g_get_ref(inst, id), value);
}

// The game tints the white ship with the player's image_blend (shield color);
// tail and shots get the same color so they match the ship.
static int resolve_color(void *self, void *other, const char *spec, double *blend) {
    if (!spec) return 0;
    if (!_strnicmp(spec, "shield", 6)) {
        RValue a[2];
        a[0] = obj_ref(find_object("oPlayer"));
        make_real(&a[1], 0);
        RValue player = call_bi(self, other, "instance_find", 2, a);
        if (!is_instance(&player)) return 0;
        *blend = inst_get(self, other, player, "image_blend");
        return 1;
    }
    unsigned rgb = (unsigned)strtoul(spec, NULL, 16);
    *blend = (double)(((rgb & 0xff) << 16) | (rgb & 0xff00) | ((rgb >> 16) & 0xff));  // BGR
    return 1;
}

// Active lashes: instance pointer (direct variables), instance ID (instance_exists)
static struct { void *inst; RValue id; int frame; double dir, side; } g_whips[4];
static int g_whip_count, g_whip_side = 1;
static void *g_whip_new;  // new instance reported by the create hook
static int g_whip_spawning;
static RValue g_whip_sprite;
static int g_whip_sprite_state;  // 0 = not loaded, 1 = loaded, -1 = failed

static int load_whip_sprite(void *self, void *other) {
    if (g_whip_sprite_state) return g_whip_sprite_state > 0;
    const char *file = gear_text(&g_gear[1], "whip_sprite");
    g_whip_sprite_state = -1;
    if (!file) return 0;
    RValue a[9];
    g_make_string(&a[0], file);
    make_real(&a[1], whip_param("whip_anim_frames", 1));  // strip of animation frames
    make_real(&a[2], 0);
    make_real(&a[3], 1);
    make_real(&a[4], whip_param("whip_origin_x", 12));
    make_real(&a[5], whip_param("whip_origin_y", 65));
    g_whip_sprite = call_bi(self, other, "sprite_add", 6, a);
    double idx = -1;
    rv_get(&g_whip_sprite, &idx);
    if (idx < 0) { logf_("Whip: sprite %s not loaded", file); return 0; }
    // Precise collision mask per frame so only the visible tail hits
    a[0] = g_whip_sprite;
    for (int i = 1; i <= 7; i++) make_real(&a[i], 0);
    make_real(&a[1], 1);
    make_real(&a[8], whip_param("whip_mask_tolerance", 60));  // lower = glow counts as hitbox
    call_bi(self, other, "sprite_collision_mask", 9, a);
    g_whip_sprite_state = 1;
    logf_("Whip: sprite %s loaded -> sprite %g", file, idx);
    return 1;
}

// Turn a new oVenomStrike into a tail lash (only while we spawn it)
static void hk_venom_create(void *self, void *other) {
    o_venom_create(self, other);
    if (!g_whip_spawning) return;
    g_whip_new = self;
    set_builtin_at(self, "alarm", 0, -1);  // hue cycling
    set_builtin_at(self, "alarm", 1, -1);  // trail
    set_builtin_at(self, "alarm", 2, -1);  // auto fade (we control it)
    set_builtin_at(self, "alarm", 5, -1);  // flame particles
    set_builtin(self, "sprite_index", &g_whip_sprite);
    double blend;
    if (resolve_color(self, other, gear_text(&g_gear[1], "whip_color"), &blend))
        set_builtin_at(self, "image_blend", (int)0x80000000, blend);
    set_builtin_at(self, "speed", (int)0x80000000, 0);
    set_builtin_at(self, "image_speed", (int)0x80000000, 0);  // we advance frames ourselves
    set_builtin_at(self, "image_index", (int)0x80000000, 0);
    set_builtin_at(self, "image_xscale", (int)0x80000000, whip_param("whip_scale", 1));
    set_builtin_at(self, "image_yscale", (int)0x80000000, whip_param("whip_scale", 1));
    set_self_var(self, "amplitude", 0);  // no wobble
    set_self_var(self, "scale", 1);
}

static void whip_strike(void *self, void *other, RValue player, double px, double py, double face) {
    if (g_whip_count >= 4 || !load_whip_sprite(self, other)) return;
    double range = whip_param("whip_range", 220);
    RValue a[4];

    // Nearest enemy around a point behind the ship (GameMaker: y points down)
    make_real(&a[0], px - cos(face) * range * 0.5);
    make_real(&a[1], py + sin(face) * range * 0.5);
    a[2] = obj_ref(g_obj_solid);
    RValue enemy = call_bi(self, other, "instance_nearest", 3, a);
    if (!is_instance(&enemy)) return;
    double ex = inst_get(self, other, enemy, "x"), ey = inst_get(self, other, enemy, "y");
    double dx = ex - px, dy = ey - py;
    if (dx * dx + dy * dy > range * range) return;
    double dir = atan2(-dy, dx);
    double diff = fabs(remainder(dir - face, 2 * M_PI)) * 180 / M_PI;
    if (diff < 180 - whip_param("whip_angle", 70)) return;  // not behind the ship

    // Damage per frame of contact; the game reads it from oPowers.venomStrikeDamage on hit
    int dmg_id = var_id("venomStrikeDamage");
    if (g_powers && dmg_id >= 0) g_set_real(g_get_ref(g_powers, dmg_id), whip_param("whip_damage", 12));

    make_real(&a[0], px);
    make_real(&a[1], py);
    make_real(&a[2], -100);
    a[3] = obj_ref(g_obj_venom);
    g_whip_new = NULL;
    g_whip_spawning = 1;
    RValue shot = call_bi(self, other, "instance_create_depth", 4, a);
    g_whip_spawning = 0;
    if (!g_whip_new || !is_instance(&shot)) return;
    inst_set_rv(self, other, shot, "leader", player);
    g_whips[g_whip_count].inst = g_whip_new;
    g_whips[g_whip_count].id = shot;
    g_whips[g_whip_count].frame = 0;
    g_whips[g_whip_count].dir = dir * 180 / M_PI;
    g_whips[g_whip_count].side = g_whip_side;
    g_whip_count++;
    g_whip_side = -g_whip_side;  // alternate swing direction
    g_whip_cooldown = (int)whip_param("whip_cooldown", 45);
}

// Keep active lashes at the stern and swing them through the arc
static void whip_update(void *self, void *other, double px, double py, double face) {
    double blend = 0;
    int has_blend = g_whip_count && resolve_color(self, other, gear_text(&g_gear[1], "whip_color"), &blend);
    int frames = (int)whip_param("whip_frames", 12);
    int anim = (int)whip_param("whip_anim_frames", 1);
    double swing = whip_param("whip_swing", 80), scale = whip_param("whip_scale", 1);
    double tx = px - cos(face) * 18, ty = py + sin(face) * 18;  // base at the stern
    for (int i = 0; i < g_whip_count;) {
        RValue a[1] = { g_whips[i].id };
        RValue alive = call_bi(self, other, "instance_exists", 1, a);
        double ok = 0;
        rv_get(&alive, &ok);
        if (!ok) { g_whips[i] = g_whips[--g_whip_count]; continue; }
        void *w = g_whips[i].inst;
        double t = (double)g_whips[i].frame / frames;
        double ang = g_whips[i].dir + g_whips[i].side * swing * (t - 0.5);
        set_builtin_at(w, "x", (int)0x80000000, tx);
        set_builtin_at(w, "y", (int)0x80000000, ty);
        set_builtin_at(w, "direction", (int)0x80000000, ang);
        set_builtin_at(w, "image_angle", (int)0x80000000, ang);
        if (has_blend) set_builtin_at(w, "image_blend", (int)0x80000000, blend);
        // Frame follows the swing; mirrored so the tip trails behind
        set_builtin_at(w, "image_index", (int)0x80000000, t * anim >= anim ? anim - 1 : floor(t * anim));
        set_builtin_at(w, "image_yscale", (int)0x80000000, -g_whips[i].side * scale);
        if (++g_whips[i].frame > frames) {
            // Fade out: the instance destroys itself next step
            set_self_var(w, "fade", 1);
            set_builtin_at(w, "image_xscale", (int)0x80000000, 0.02);
            set_builtin_at(w, "image_yscale", (int)0x80000000, 0.012);
            g_whips[i] = g_whips[--g_whip_count];
            continue;
        }
        i++;
    }
}

static void hk_player_step(void *self, void *other) {
    o_player_step(self, other);
    int active = g_powers && gear_text(&g_gear[1], "whip_damage") && gear_active(&g_gear[1], g_powers);
    if (!active && !g_whip_count) return;
    RValue a[2];
    a[0] = obj_ref(g_obj_player);
    make_real(&a[1], 0);
    RValue player = call_bi(self, other, "instance_find", 2, a);
    if (!is_instance(&player)) return;
    double px = inst_get(self, other, player, "x"), py = inst_get(self, other, player, "y");
    double face = inst_get(self, other, player, "image_angle") * M_PI / 180;
    whip_update(self, other, px, py, face);
    if (g_whip_cooldown > 0) { g_whip_cooldown--; return; }
    if (active) whip_strike(self, other, player, px, py, face);
}

static void install_whip(void) {
    if (!gear_text(&g_gear[1], "whip_damage") || g_gear[1].id < 0) return;
    g_obj_player = find_object("oPlayer");
    g_obj_solid = find_object("oSolid");
    g_obj_venom = find_object("oVenomStrike");
    if (g_obj_player < 0 || g_obj_solid < 0 || g_obj_venom < 0) { logf_("Whip: objects missing"); return; }
    o_venom_create = hook_event("oVenomStrike", "Create_0", hk_venom_create);
    if (!o_venom_create) return;
    o_player_step = hook_event("oPlayer", "Step_0", hk_player_step);
    if (o_player_step)
        logf_("Whip active (damage %g per frame, lash %g frames, every %g frames)", whip_param("whip_damage", 12),
              whip_param("whip_frames", 12), whip_param("whip_cooldown", 45));
}
