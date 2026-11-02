// ------------------------------------------------------------- Tail whip ----
// When an enemy is behind the ship, an animated tail swings through the area
// behind it. Hits use an oVenomStrike (the game's damage logic) with its
// flames, trail and movement disabled. Values: whip_* in [newweapon].

static GmlEventFn o_player_step, o_venom_create;
static int g_obj_solid = -1, g_obj_venom = -1, g_obj_enemy_shot = -1, g_obj_reflected = -1;
static int g_reflected;  // reflected projectiles (log)
static int g_whip_cooldown;

// whip_* values, read after each INI load
static struct {
    const char *sprite, *color;
    double damage, range, angle, swing, scale, origin_x, origin_y, tolerance;
    int on, frames, anim, cooldown, block;
} g_wc;

static double whip_param(const char *key, double def) {
    const char *v = gear_text(&g_gear[1], key);
    return v ? atof(v) : def;
}

static void whip_config(void) {
    g_wc.on = gear_text(&g_gear[1], "whip_damage") != NULL;
    g_wc.sprite = gear_text(&g_gear[1], "whip_sprite");
    g_wc.color = gear_text(&g_gear[1], "whip_color");
    g_wc.damage = whip_param("whip_damage", 12);
    g_wc.range = whip_param("whip_range", 220);
    g_wc.angle = whip_param("whip_angle", 70);
    g_wc.swing = whip_param("whip_swing", 80);
    g_wc.scale = whip_param("whip_scale", 1);
    g_wc.origin_x = whip_param("whip_origin_x", 12);
    g_wc.origin_y = whip_param("whip_origin_y", 65);
    g_wc.tolerance = whip_param("whip_mask_tolerance", 60);
    g_wc.frames = (int)whip_param("whip_frames", 12);
    g_wc.anim = (int)whip_param("whip_anim_frames", 1);
    g_wc.cooldown = (int)whip_param("whip_cooldown", 45);
    g_wc.block = (int)whip_param("whip_block", 1);
    if (g_wc.frames < 1) g_wc.frames = 1;
    if (g_wc.anim < 1) g_wc.anim = 1;
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
    g_whip_sprite_state = -1;
    // Strip of animation frames
    if (!g_wc.sprite ||
        !add_sprite(self, other, g_wc.sprite, g_wc.anim, g_wc.origin_x, g_wc.origin_y, &g_whip_sprite))
        return 0;
    // Precise collision mask per frame so only the visible tail hits
    RValue a[9] = { g_whip_sprite };
    for (int i = 1; i <= 7; i++) make_real(&a[i], 0);
    make_real(&a[1], 1);
    make_real(&a[8], g_wc.tolerance);  // lower = glow counts as hitbox
    call_bi(self, other, BI("sprite_collision_mask"), 9, a);
    g_whip_sprite_state = 1;
    return 1;
}

// Turn a new oVenomStrike into a tail lash (only while we spawn it)
static void hk_venom_create(void *self, void *other) {
    o_venom_create(self, other);
    if (!g_whip_spawning) return;
    g_whip_new = self;
    int alarm = VAR("alarm");
    set_builtin_real(self, alarm, 0, -1);  // hue cycling
    set_builtin_real(self, alarm, 1, -1);  // trail
    set_builtin_real(self, alarm, 2, -1);  // auto fade (we control it)
    set_builtin_real(self, alarm, 5, -1);  // flame particles
    set_builtin(self, VAR("sprite_index"), NO_INDEX, &g_whip_sprite);
    double blend;
    if (resolve_color(self, other, g_wc.color, &blend)) set_builtin_real(self, VAR("image_blend"), NO_INDEX, blend);
    set_builtin_real(self, VAR("speed"), NO_INDEX, 0);
    set_builtin_real(self, VAR("image_speed"), NO_INDEX, 0);  // we advance frames ourselves
    set_builtin_real(self, VAR("image_index"), NO_INDEX, 0);
    set_builtin_real(self, VAR("image_xscale"), NO_INDEX, g_wc.scale);
    set_builtin_real(self, VAR("image_yscale"), NO_INDEX, g_wc.scale);
    set_self_var(self, VAR("amplitude"), 0);  // no wobble
    set_self_var(self, VAR("scale"), 1);
}

// Nearest instance of obj around a point behind the ship; 1 if it is in range and in
// the cone behind (GameMaker: y points down). *dir = direction to it in radians.
static int find_behind(void *self, void *other, int obj, double px, double py, double face, double *dir) {
    if (obj < 0) return 0;
    double range = g_wc.range;
    RValue a[3];
    make_real(&a[0], px - cos(face) * range * 0.5);
    make_real(&a[1], py + sin(face) * range * 0.5);
    a[2] = obj_ref(obj);
    RValue target = call_bi(self, other, BI("instance_nearest"), 3, a);
    if (!is_instance(&target)) return 0;
    double dx = inst_get(self, other, target, GSTR("x")) - px, dy = inst_get(self, other, target, GSTR("y")) - py;
    if (dx * dx + dy * dy > range * range) return 0;
    *dir = atan2(-dy, dx);
    double diff = fabs(remainder(*dir - face, 2 * M_PI)) * 180 / M_PI;
    return diff >= 180 - g_wc.angle;
}

static void whip_strike(void *self, void *other, RValue player, double px, double py, double face) {
    if (g_whip_count >= 4 || !load_whip_sprite(self, other)) return;
    // Enemies first, else incoming projectiles (to bat them back)
    double dir;
    if (!find_behind(self, other, g_obj_solid, px, py, face, &dir) &&
        !(g_wc.block && find_behind(self, other, g_obj_enemy_shot, px, py, face, &dir)))
        return;
    RValue a[4];

    // Damage per frame of contact; the game reads it from oPowers.venomStrikeDamage on hit
    int dmg_id = VAR("venomStrikeDamage");
    if (g_powers && dmg_id >= 0) g_set_real(g_get_ref(g_powers, dmg_id), g_wc.damage);

    make_real(&a[0], px);
    make_real(&a[1], py);
    make_real(&a[2], -100);
    a[3] = obj_ref(g_obj_venom);
    g_whip_new = NULL;
    g_whip_spawning = 1;
    RValue shot = call_bi(self, other, BI("instance_create_depth"), 4, a);
    g_whip_spawning = 0;
    if (!g_whip_new || !is_instance(&shot)) return;
    inst_set(self, other, shot, GSTR("leader"), player);
    g_whips[g_whip_count++] = (typeof(g_whips[0])){ g_whip_new, shot, 0, dir * 180 / M_PI, g_whip_side };
    g_whip_side = -g_whip_side;  // alternate swing direction
    g_whip_cooldown = g_wc.cooldown;
}

// Bat a projectile back like the game's reflect(): it turns into a reflected clone
// (hits enemies, not the player) that remembers its original object, sprite and mask.
typedef struct { double dir, revenge; RValue object, sprite, mask; } Reflect;

static void reflect_shot(void *p, void *ctx) {
    Reflect *r = ctx;
    set_builtin_real(p, VAR("direction"), NO_INDEX, r->dir);
    set_builtin_real(p, VAR("image_angle"), NO_INDEX, r->dir);
    set_self_var(p, VAR("revenge"), r->revenge);  // damage multiplier, also marks it as reflected
    int orig = VAR("object_index_original");
    if (orig >= 0) g_assign(g_get_ref(p, orig), &r->object);
    RValue a[2] = { obj_ref(g_obj_reflected) };
    make_real(&a[1], 0);  // no events
    call_bi(p, p, BI("instance_change"), 2, a);
    set_builtin(p, VAR("sprite_index"), NO_INDEX, &r->sprite);
    double mask = -1;
    rv_get(&r->mask, &mask);
    if (mask != -1) set_builtin(p, VAR("mask_index"), NO_INDEX, &r->mask);
}

static void whip_reflect(void *w, RValue shot, double tx, double ty) {
    if (inst_get(w, w, shot, GSTR("revenge")) > 0) return;  // already reflected
    double sx = inst_get(w, w, shot, GSTR("x")), sy = inst_get(w, w, shot, GSTR("y"));
    Reflect r;
    // Back at the shooter if it still exists, else straight back
    RValue shooter = inst_get_rv(w, w, shot, GSTR("shooter"));
    RValue alive = call_bi(w, w, BI("instance_exists"), 1, &shooter);
    double ok = 0;
    rv_get(&alive, &ok);
    if (ok && is_instance(&shooter)) {
        double ex = inst_get(w, w, shooter, GSTR("x")), ey = inst_get(w, w, shooter, GSTR("y"));
        r.dir = atan2(-(ey - sy), ex - sx) * 180 / M_PI;
    } else if ((sx - tx) * (sx - tx) + (sy - ty) * (sy - ty) > 1) {
        r.dir = atan2(-(sy - ty), sx - tx) * 180 / M_PI;  // away from the tail base
    } else {
        r.dir = inst_get(w, w, shot, GSTR("direction")) + 180;
    }
    r.revenge = g_powers ? get_var(g_powers, VAR("reflectMult")) : 1;
    if (r.revenge <= 0) r.revenge = 1;
    r.object = inst_get_rv(w, w, shot, GSTR("object_index"));
    r.sprite = inst_get_rv(w, w, shot, GSTR("sprite_index"));
    r.mask = inst_get_rv(w, w, shot, GSTR("mask_index"));
    with_instances(w, w, shot, reflect_shot, &r);
    if (++g_reflected <= 5 || g_reflected % 50 == 0) {
        double obj = -1;
        rv_get(&r.object, &obj);
        logf_("Whip: projectile %s batted back (%d so far)", object_name((int)obj), g_reflected);
    }
}

// Enemy projectiles touching the tail (its precise mask, current frame) are batted back
static void whip_block(void *w, double x, double y) {
    RValue a[3];
    make_real(&a[0], x);
    make_real(&a[1], y);
    a[2] = obj_ref(g_obj_enemy_shot);
    for (int n = 0; n < 16; n++) {  // changed instances no longer match oEnemyProjectile
        RValue shot = call_bi(w, w, BI("instance_place"), 3, a);
        if (!is_instance(&shot)) break;
        double before = g_reflected;
        whip_reflect(w, shot, x, y);
        if (g_reflected == before) break;  // not reflectable, avoid finding it again
    }
}

// Keep active lashes at the stern and swing them through the arc
static void whip_update(void *self, void *other, double px, double py, double face) {
    if (!g_whip_count) return;
    double blend = 0;
    int has_blend = resolve_color(self, other, g_wc.color, &blend);
    int vx = VAR("x"), vy = VAR("y"), vdir = VAR("direction"), vangle = VAR("image_angle");
    int vblend = VAR("image_blend"), vindex = VAR("image_index");
    int vxscale = VAR("image_xscale"), vyscale = VAR("image_yscale");
    double tx = px - cos(face) * 18, ty = py + sin(face) * 18;  // base at the stern
    for (int i = 0; i < g_whip_count;) {
        RValue alive = call_bi(self, other, BI("instance_exists"), 1, &g_whips[i].id);
        double ok = 0;
        rv_get(&alive, &ok);
        if (!ok) { g_whips[i] = g_whips[--g_whip_count]; continue; }
        void *w = g_whips[i].inst;
        double t = (double)g_whips[i].frame / g_wc.frames;
        double ang = g_whips[i].dir + g_whips[i].side * g_wc.swing * (t - 0.5);
        set_builtin_real(w, vx, NO_INDEX, tx);
        set_builtin_real(w, vy, NO_INDEX, ty);
        set_builtin_real(w, vdir, NO_INDEX, ang);
        set_builtin_real(w, vangle, NO_INDEX, ang);
        if (has_blend) set_builtin_real(w, vblend, NO_INDEX, blend);
        // Frame follows the swing; mirrored so the tip trails behind
        double frame = floor(t * g_wc.anim);
        set_builtin_real(w, vindex, NO_INDEX, frame >= g_wc.anim ? g_wc.anim - 1 : frame);
        set_builtin_real(w, vyscale, NO_INDEX, -g_whips[i].side * g_wc.scale);
        if (g_wc.block && g_obj_enemy_shot >= 0) whip_block(w, tx, ty);
        if (++g_whips[i].frame > g_wc.frames) {
            // Fade out: the instance destroys itself next step
            set_self_var(w, VAR("fade"), 1);
            set_builtin_real(w, vxscale, NO_INDEX, 0.02);
            set_builtin_real(w, vyscale, NO_INDEX, 0.012);
            g_whips[i] = g_whips[--g_whip_count];
            continue;
        }
        i++;
    }
}

static void hk_player_step(void *self, void *other) {
    o_player_step(self, other);
    int active = g_wc.on && g_powers && gear_active(&g_gear[1], g_powers);
    if (!active && !g_whip_count) return;
    RValue a[2] = { obj_ref(g_obj_player) };
    make_real(&a[1], 0);
    RValue player = call_bi(self, other, BI("instance_find"), 2, a);
    if (!is_instance(&player)) return;
    double px = inst_get(self, other, player, GSTR("x")), py = inst_get(self, other, player, GSTR("y"));
    double face = inst_get(self, other, player, GSTR("image_angle")) * M_PI / 180;
    whip_update(self, other, px, py, face);
    if (g_whip_cooldown > 0) { g_whip_cooldown--; return; }
    if (active) whip_strike(self, other, player, px, py, face);
}

static void install_whip(void) {
    if (!g_wc.on || g_gear[1].id < 0) return;
    g_obj_solid = find_object("oSolid");
    g_obj_venom = find_object("oVenomStrike");
    g_obj_enemy_shot = find_object("oEnemyProjectile");  // parent of all enemy shots
    g_obj_reflected = find_object("oEnemyProjectileReflectedObjectClone");
    if (g_obj_reflected < 0 || !g_with_begin || !g_with_next || !g_with_end) {
        g_obj_enemy_shot = -1;
        logf_("Whip: projectile reflection unavailable");
    }
    if (g_obj_player < 0 || g_obj_solid < 0 || g_obj_venom < 0) { logf_("Whip: objects missing"); return; }
    o_venom_create = hook_event("oVenomStrike", "Create_0", hk_venom_create);
    if (!o_venom_create) return;
    o_player_step = hook_event("oPlayer", "Step_0", hk_player_step);
    if (o_player_step)
        logf_("Whip active (damage %g per frame, lash %d frames, every %d frames)", g_wc.damage, g_wc.frames,
              g_wc.cooldown);
}
