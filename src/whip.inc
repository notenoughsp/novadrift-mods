// ------------------------------------------------------------- Tail whip ----
// When an enemy is behind the ship, an animated tail swings through the area
// behind it. The tail is its own object oStingerTail, a child of oVenomStrike:
// it inherits the game's hit logic (step, collision with oSolid) while its
// flame, trail, hue and fade alarms are empty. Values: whip_* in [newweapon].

static GmlEventFn o_player_step, g_venom_create;
static int g_obj_solid = -1, g_obj_tail = -1, g_obj_enemy_shot = -1, g_obj_reflected = -1;
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
static void *g_whip_new;  // new instance reported by the create event
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

// oStingerTail: the venom strike setup, then the tail look
static void ev_tail_create(void *self, void *other) {
    g_venom_create(self, other);
    g_whip_new = self;
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

// Replaces oVenomStrike's hue (0), trail (1), auto fade (2) and flame (5) alarms
static void ev_tail_noop(void *self, void *other) {}

// Hit: the venom strike damage (venomStrikeHitsSolid) as a direct hit. The game
// also ignites the enemy there (dotDurationList[4] = igniteTime), which we restore,
// and books the damage as burn (yellow numbers). Tail damage is collected per
// enemy instead and shown as white direct-hit numbers (showDamage) every 8 frames.
static GmlEventFn g_venom_collision;
static GmlFn o_record_burn, g_show_damage;
static int g_tail_hit;
static struct { RValue id; double sum; } g_tail_dmg[32];
static int g_tail_dmg_count;

static void *hk_record_burn(void *self, void *other, void *ret, int argc, void **argv) {
    double amount = 0;
    if (!g_tail_hit || !g_show_damage || argc < 1 || !rv_get(argv[0], &amount))
        return o_record_burn(self, other, ret, argc, argv);
    RValue id = get_builtin(self, VAR("id"));  // self = the enemy
    int i = 0;
    while (i < g_tail_dmg_count && memcmp(&g_tail_dmg[i].id, &id, 8)) i++;
    if (i == g_tail_dmg_count && i < 32) g_tail_dmg[g_tail_dmg_count++] = (typeof(g_tail_dmg[0])){ id, 0 };
    if (i < 32) g_tail_dmg[i].sum += amount;
    ((RValue *)ret)->kind = 5;
    return ret;
}

static void show_tail_damage(void *self, void *other) {
    for (int i = 0; i < g_tail_dmg_count; i++) {
        RValue alive = call_bi(self, other, BI("instance_exists"), 1, &g_tail_dmg[i].id), res = { .kind = 5 };
        double ok = 0;
        if (!rv_get(&alive, &ok) || !ok || g_tail_dmg[i].sum <= 0) continue;
        RValue a[3] = { { .real = g_tail_dmg[i].sum, .kind = KIND_REAL }, { .real = 1, .kind = KIND_BOOL },
                        g_tail_dmg[i].id };  // amount, at target, target
        void *argv[3] = { &a[0], &a[1], &a[2] };
        g_show_damage(self, other, &res, 3, argv);
    }
    g_tail_dmg_count = 0;
}

static void tail_hit(void *self, void *other) {
    g_tail_hit = 1;
    g_venom_collision(self, other);
    g_tail_hit = 0;
}

static void ev_tail_collision(void *self, void *other) {
    RValue id = get_builtin(other, VAR("id"));
    RValue list = inst_get_rv(self, other, id, GSTR("dotDurationList"));
    if ((list.kind & 0xffffff) != 2) { tail_hit(self, other); return; }  // 2 = GML array
    RValue a[3] = { list };
    make_real(&a[1], 4);
    a[2] = call_bi(self, other, BI("array_get"), 2, a);
    tail_hit(self, other);
    if ((a[2].kind & 0xffffff) != 5) call_bi(self, other, BI("array_set"), 3, a);
}

static ObjEvent g_tail_events[] = {
    { EV_CREATE, 0, ev_tail_create },
    { EV_ALARM, 0, ev_tail_noop },
    { EV_ALARM, 1, ev_tail_noop },
    { EV_ALARM, 2, ev_tail_noop },
    { EV_ALARM, 5, ev_tail_noop },
    { EV_COLLISION, -1, ev_tail_collision },  // subtype = oSolid, set at install
};
static ObjDef g_tail_def = { "oStingerTail", "oVenomStrike", "oVenomStrike", 0, g_tail_events, 6, -1, &g_obj_tail };

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
    if (g_whip_count >= 4 || g_obj_tail < 0 || !load_whip_sprite(self, other)) return;
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
    a[3] = obj_ref(g_obj_tail);
    g_whip_new = NULL;
    RValue shot = call_bi(self, other, BI("instance_create_depth"), 4, a);
    if (!g_whip_new || !is_instance(&shot)) return;
    inst_set(self, other, shot, GSTR("leader"), player);
    g_whips[g_whip_count++] = (typeof(g_whips[0])){ g_whip_new, shot, 0, dir * 180 / M_PI, g_whip_side };
    g_whip_side = -g_whip_side;  // alternate swing direction
    g_whip_cooldown = g_wc.cooldown;
}

// Bat a projectile back like the game's reflect(): it turns into a reflected clone
// (hits enemies, not the player) that remembers its original object, sprite and mask.
typedef struct { double dir, revenge, object; } Reflect;

static void reflect_shot(void *p, void *ctx) {
    Reflect *r = ctx;
    RValue object = get_builtin(p, VAR("object_index"));
    RValue sprite = get_builtin(p, VAR("sprite_index")), mask = get_builtin(p, VAR("mask_index"));
    rv_get(&object, &r->object);
    set_builtin_real(p, VAR("direction"), NO_INDEX, r->dir);
    set_builtin_real(p, VAR("image_angle"), NO_INDEX, r->dir);
    set_self_var(p, VAR("revenge"), r->revenge);  // damage multiplier, also marks it as reflected
    int orig = VAR("object_index_original");
    if (orig >= 0) g_assign(g_get_ref(p, orig), &object);
    RValue a[2] = { obj_ref(g_obj_reflected) };
    make_real(&a[1], 0);  // no events
    call_bi(p, p, BI("instance_change"), 2, a);
    set_builtin(p, VAR("sprite_index"), NO_INDEX, &sprite);
    double m = -1;
    rv_get(&mask, &m);
    if (m != -1) set_builtin(p, VAR("mask_index"), NO_INDEX, &mask);
}

typedef struct { RValue whip; double tx, ty, revenge; } BlockCtx;

// Runs on each enemy projectile (with): touching the tail -> batted back
static void block_shot(void *p, void *ctx) {
    BlockCtx *b = ctx;
    if (get_var(p, VAR("revenge")) > 0) return;  // already reflected
    RValue x = get_builtin(p, VAR("x")), y = get_builtin(p, VAR("y"));
    RValue a[3] = { x, y, b->whip };
    RValue hit = call_bi(p, p, BI("place_meeting"), 3, a);  // tail's precise mask
    double touching = 0, sx = 0, sy = 0;
    if (!rv_get(&hit, &touching) || !touching) return;
    rv_get(&x, &sx);
    rv_get(&y, &sy);

    // Back at the shooter if it still exists, else away from the tail base
    Reflect r = { .revenge = b->revenge, .object = -1 };
    int shooter_id = VAR("shooter");
    RValue shooter = shooter_id >= 0 ? *g_get_ref(p, shooter_id) : (RValue){ .kind = 5 };
    RValue alive = call_bi(p, p, BI("instance_exists"), 1, &shooter);
    double ok = 0;
    rv_get(&alive, &ok);
    if (ok && is_instance(&shooter)) {
        double ex = inst_get(p, p, shooter, GSTR("x")), ey = inst_get(p, p, shooter, GSTR("y"));
        r.dir = atan2(-(ey - sy), ex - sx) * 180 / M_PI;
    } else if ((sx - b->tx) * (sx - b->tx) + (sy - b->ty) * (sy - b->ty) > 1) {
        r.dir = atan2(-(sy - b->ty), sx - b->tx) * 180 / M_PI;
    } else {
        RValue d = get_builtin(p, VAR("direction"));
        rv_get(&d, &r.dir);
        r.dir += 180;
    }
    reflect_shot(p, &r);
    if (++g_reflected <= 5 || g_reflected % 50 == 0)
        logf_("Whip: projectile %s batted back (%d so far)", object_name((int)r.object), g_reflected);
}

// Like the game: with (oEnemyProjectile) check contact with the tail instance
static void whip_block(void *w, RValue whip, double tx, double ty) {
    BlockCtx b = { whip, tx, ty, g_powers ? get_var(g_powers, VAR("reflectMult")) : 1 };
    if (b.revenge <= 0) b.revenge = 1;
    with_instances(w, w, obj_ref(g_obj_enemy_shot), block_shot, &b);
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
        if (g_wc.block && g_obj_enemy_shot >= 0) whip_block(w, g_whips[i].id, tx, ty);
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

static int g_whip_ready;

// oPlayer step: Molting, then the tail
static void hk_player_step(void *self, void *other) {
    o_player_step(self, other);
    molting_step(self, other);
    int active = g_whip_ready && g_powers && gear_active(&g_gear[1], g_powers);
    if (!active && !g_whip_count) return;
    RValue a[2] = { obj_ref(g_obj_player) };
    make_real(&a[1], 0);
    RValue player = call_bi(self, other, BI("instance_find"), 2, a);
    if (!is_instance(&player)) return;
    double px = inst_get(self, other, player, GSTR("x")), py = inst_get(self, other, player, GSTR("y"));
    double face = inst_get(self, other, player, GSTR("image_angle")) * M_PI / 180;
    whip_update(self, other, px, py, face);
    static int tick;
    if (g_tail_dmg_count && (++tick % 8 == 0 || !g_whip_count)) show_tail_damage(self, other);
    if (g_whip_cooldown > 0) { g_whip_cooldown--; return; }
    if (active) whip_strike(self, other, player, px, py, face);
}

static void install_whip(void) {
    if (!g_wc.on || !g_gear[1].def.name) return;  // the weapon's object is registered
    g_obj_solid = find_object("oSolid");
    g_obj_enemy_shot = find_object("oEnemyProjectile");  // parent of all enemy shots
    g_obj_reflected = find_object("oEnemyProjectileReflectedObjectClone");
    if (g_obj_reflected < 0 || !g_with_begin || !g_with_next || !g_with_end || !g_get_builtin_var) {
        g_obj_enemy_shot = -1;
        logf_("Whip: projectile reflection unavailable");
    }
    g_venom_create = (GmlEventFn)find_gml_function("gml_Object_oVenomStrike_Create_0");
    g_venom_collision = (GmlEventFn)find_gml_function("gml_Object_oVenomStrike_Collision_oSolid");
    if (g_obj_player < 0 || g_obj_solid < 0 || !g_venom_create || !g_venom_collision) {
        logf_("Whip: objects missing");
        return;
    }
    g_tail_events[5].subtype = g_obj_solid;
    register_object(&g_tail_def);
    g_show_damage = (GmlFn)find_gml_function("gml_Script_showDamage");
    o_record_burn = hook(find_gml_function("gml_Script_recordDamageBurn"), hk_record_burn, "recordDamageBurn");
    g_whip_ready = 1;
    logf_("Whip active (damage %g per frame, lash %d frames, every %d frames)", g_wc.damage, g_wc.frames,
          g_wc.cooldown);
}

static void install_player_step(void) {
    if (g_whip_ready || (g_mc.on && MOLT->def.name))
        o_player_step = hook_event("oPlayer", "Step_0", hk_player_step);
}
