// ---------------------------------------------------------- New objects ----
// Creates real GameMaker objects at runtime. The runtime keeps objects in a hash
// (index -> CObjectGM, 0x98 bytes) and creates its own __YYInternalObject__ the
// same way. New objects are added right before the runtime links parents and
// prepares events, so inheritance and event lists include them. Their events
// are C functions: a CCode whose function entry points at our code.
// Layout and helpers verified for game version 1.2.38.4.

enum { EV_CREATE = 0, EV_DESTROY = 1, EV_ALARM = 2, EV_STEP = 3, EV_COLLISION = 4, EV_OTHER = 7, EV_DRAW = 8, EV_CLEANUP = 12 };

typedef struct { int type, subtype; GmlEventFn fn; } ObjEvent;
typedef struct {
    const char *name, *parent;  // parent: existing object name or NULL
    int visible;
    const ObjEvent *events;
    int event_count;
    int index;                  // object index once created, else -1
} ObjDef;

// YYC function table entry the runtime calls events through
typedef struct { const char *name; GmlEventFn func; void *func_var; } YYFunc;

typedef void *(*RtAllocFn)(size_t size);
typedef void *(*ObjCtorFn)(void *obj, int index);
typedef void (*ObjSetNameFn)(void *obj, const char *name);  // copies the name
typedef void (*ObjHashInsertFn)(void *hash, int index, void *obj);
typedef void *(*CodeCtorFn)(void *code, int code_index, int flags);
typedef void (*EventInsertFn)(void *map, int64_t key, void *event);
typedef void (*VoidFn)(void);

static const Helper OBJ_HELPERS[] = {
    HELPER("alloc", 0xf2f550, "\x41\xb1\x01\x48\x8d\x15"),
    HELPER("objCtor", 0xfc6c80, "\x48\x89\x5c\x24\x10\x48\x89\x6c\x24\x20\x48\x89\x4c\x24\x08\x56"),
    HELPER("objSetName", 0xfc8a80, "\x48\x89\x74\x24\x10\x57\x48\x83\xec\x20\x48\x8b\xf1\x48\x8b\xfa"),
    HELPER("objHashInsert", 0xfc5d40, "\x48\x89\x5c\x24\x08\x48\x89\x6c\x24\x10\x48\x89\x74\x24\x18\x57"),
    HELPER("codeCtor", 0xf04490, "\x40\x53\x55\x48\x83\xec\x28\x33\xed\x48\x89\x74\x24\x48"),
    HELPER("eventInsert", 0xfc8080, "\x48\x89\x5c\x24\x10\x48\x89\x6c\x24\x20\x4c\x89\x44\x24\x18\x56"),
    HELPER("objPrepare", 0xfc6950, "\x48\x89\x5c\x24\x08\x48\x89\x74\x24\x10\x48\x89\x7c\x24\x18\x4c"),
};
// Runtime globals (same version as the helpers)
#define RVA_OBJECT_HASH 0x1b26ea0  // CHash<CObjectGM>*
#define RVA_OBJECT_COUNT 0x1b26e98 // int
#define RVA_GML_FUNCS 0x1b1c8e8    // struct with the YYFunc array at +0x28
#define RVA_GML_FUNC_COUNT 0x1b1c884

static RtAllocFn g_rt_alloc;
static ObjCtorFn g_obj_ctor;
static ObjSetNameFn g_obj_set_name;
static ObjHashInsertFn g_obj_hash_insert;
static CodeCtorFn g_code_ctor;
static EventInsertFn g_event_insert;
static VoidFn o_obj_prepare;

static ObjDef *g_objdefs[16];
static int g_objdef_count;

static void register_object(ObjDef *def) {
    def->index = -1;
    if (g_objdef_count < 16) g_objdefs[g_objdef_count++] = def;
}

static void *rt_zalloc(size_t size) {
    void *p = g_rt_alloc(size);
    if (p) memset(p, 0, size);
    return p;
}

static uint8_t *rt_global(uint32_t rva) { return (uint8_t *)GetModuleHandleW(NULL) + rva; }

// Index of an existing event function, used as template for CCode flags
static int gml_func_index(const char *name) {
    YYFunc *funcs = *(YYFunc **)(*(uint8_t **)rt_global(RVA_GML_FUNCS) + 0x28);
    int count = *(int *)rt_global(RVA_GML_FUNC_COUNT);
    for (int i = 0; funcs && i < count; i++)
        if (funcs[i].name && !strcmp(funcs[i].name, name)) return i;
    return -1;
}

static void *make_event(const char *obj_name, const ObjEvent *e, int obj_index, int template_code) {
    char name[96];
    snprintf(name, sizeof name, "gml_Object_%s_%d_%d", obj_name, e->type, e->subtype);
    YYFunc *entry = calloc(1, sizeof *entry);  // lives as long as the game
    entry->name = _strdup(name);
    entry->func = e->fn;
    uint8_t *code = rt_zalloc(0xb8);
    void **event = rt_zalloc(0x10);
    if (!code || !event) return NULL;
    g_code_ctor(code, template_code, 0);
    *(const char **)(code + 0x80) = entry->name;
    *(YYFunc **)(code + 0x90) = entry;
    event[0] = code;
    *(int *)(event + 1) = obj_index;
    return event;
}

static void create_object(ObjDef *def, int template_code) {
    int *count = (int *)rt_global(RVA_OBJECT_COUNT);
    void *hash = *(void **)rt_global(RVA_OBJECT_HASH);
    int index = *count;
    if ((uint32_t)index != g_obj_count) {
        logf_("Objects: count mismatch (runtime %d, data.win %u), %s skipped", index, g_obj_count, def->name);
        return;
    }
    uint8_t *obj = rt_zalloc(0x98);
    if (!obj) return;
    g_obj_ctor(obj, index);
    g_obj_set_name(obj, def->name);
    int parent = def->parent ? find_object(def->parent) : -1;
    *(uint32_t *)(obj + 0x80) = (def->visible ? 2 : 0) | 0x80;  // visible, managed
    *(int *)(obj + 0x84) = -1;                                 // sprite
    *(int *)(obj + 0x8c) = parent;                              // linked by the prepare step
    *(int *)(obj + 0x90) = -1;                                 // mask
    for (int i = 0; i < def->event_count; i++) {
        void *event = make_event(def->name, &def->events[i], index, template_code);
        int64_t key = (int64_t)def->events[i].type << 32 | (uint32_t)def->events[i].subtype;
        if (event) g_event_insert(*(void **)(obj + 0x18), key, event);
    }
    g_obj_hash_insert(hash, index, obj);
    (*count)++;
    // Keep our name list in sync so find_object/object_name know it
    g_obj_names = realloc(g_obj_names, (g_obj_count + 1) * sizeof *g_obj_names);
    g_obj_names[g_obj_count++] = _strdup(def->name);
    def->index = index;
    logf_("Objects: created %s (index %d, parent %s, %d events)", def->name, index,
          parent >= 0 ? def->parent : "none", def->event_count);
}

// Runs once while data.win is loaded, after all OBJT objects exist
static void hk_obj_prepare(void) {
    int template_code = g_objdef_count ? gml_func_index("gml_Object_oPlayer_Step_0") : -1;
    if (g_objdef_count && template_code < 0) logf_("Objects: template event not found, no objects created");
    for (int i = 0; template_code >= 0 && i < g_objdef_count; i++) create_object(g_objdefs[i], template_code);
    o_obj_prepare();
}

// ------------------------------------------------------------ Probe ----
// Test object (object_probe=1 in [settings]): spawned at run start, logs its
// events and destroys itself after 120 frames.

static int g_probe_steps;
static ObjDef g_probe_def;

static void probe_create(void *self, void *other) {
    g_probe_steps = 0;
    RValue a[1] = { obj_ref(g_probe_def.index) };
    RValue n = call_bi(self, other, BI("instance_number"), 1, a);
    double count = 0;
    rv_get(&n, &count);
    logf_("Probe: create event (instances %g)", count);
}

static double probe_count(void *self, void *other) {
    RValue a[1] = { obj_ref(g_probe_def.index) };
    RValue n = call_bi(self, other, BI("instance_number"), 1, a);
    double count = -1;
    rv_get(&n, &count);
    return count;
}

static void probe_step(void *self, void *other) {
    ++g_probe_steps;
    if (g_probe_steps == 30) {
        RValue id = get_builtin(self, VAR("id"));
        double before = probe_count(self, other);
        call_bi(self, other, BI("instance_destroy"), 1, &id);
        logf_("Probe: step event ran 30 times, destroyed (instances %g -> %g)", before, probe_count(self, other));
    }
    if (g_probe_steps == 31 || g_probe_steps == 90) logf_("Probe: still stepping (%d)", g_probe_steps);
}

static void probe_destroy(void *self, void *other) { logf_("Probe: destroy event after %d steps", g_probe_steps); }
static void probe_cleanup(void *self, void *other) { logf_("Probe: clean up event"); }

static const ObjEvent PROBE_EVENTS[] = {
    { EV_CREATE, 0, probe_create },
    { EV_STEP, 0, probe_step },
    { EV_DESTROY, 0, probe_destroy },
    { EV_CLEANUP, 0, probe_cleanup },
};
static ObjDef g_probe_def = { "oModProbe", NULL, 0, PROBE_EVENTS, 4, -1 };

static void spawn_probe(void *self, void *other) {
    if (!g_object_probe || g_probe_def.index < 0) return;
    RValue a[4];
    make_real(&a[0], 0);
    make_real(&a[1], 0);
    make_real(&a[2], 0);
    a[3] = obj_ref(g_probe_def.index);
    RValue inst = call_bi(self, other, BI("instance_create_depth"), 4, a);
    RValue n = call_bi(self, other, BI("object_get_name"), 1, &a[3]);
    const char *name = (n.kind & 0xffffff) == 1 && n.ptr ? *(const char **)n.ptr : "?";
    logf_("Probe: spawned (object_get_name = %s, instance %s)", name, is_instance(&inst) ? "ok" : "missing");
}

static void install_objects(void) {
    void *p[7];
    for (int i = 0; i < 7; i++)
        if (!(p[i] = resolve_helper(&OBJ_HELPERS[i]))) { logf_("Objects: disabled"); return; }
    g_rt_alloc = p[0];
    g_obj_ctor = p[1];
    g_obj_set_name = p[2];
    g_obj_hash_insert = p[3];
    g_code_ctor = p[4];
    g_event_insert = p[5];
    if (g_object_probe) register_object(&g_probe_def);
    if (g_objdef_count) o_obj_prepare = (VoidFn)hook(p[6], hk_obj_prepare, "objPrepare");
}
