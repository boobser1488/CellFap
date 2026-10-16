/*
 * CellFap native mod SDK.
 *
 * A native mod is a shared library (.dll on Windows, .so on Android) shipped inside a CellFap modification under
 *     native/windows-x64/        native/android-arm64-v8a/
 * It talks to the game through this header only: plain C99, fixed-size types, no C++ and no allocator crossing the
 * boundary, so any language with a C FFI can implement it (C, C++, Rust, Zig, Go c-shared, Odin, Nim, Pascal, D...).
 * Full documentation, in Russian, with build instructions for every language: docs/mods.md.
 *
 * The library exports two functions:
 *
 *     CELLFAP_MOD_ENTRY uint32_t cellfap_mod_api_version(void) { return CELLFAP_MOD_API_VERSION; }
 *     CELLFAP_MOD_ENTRY int cellfap_mod_load(const CellFapApi* api, CellFapMod* mod) { ... return CELLFAP_OK; }
 *
 * cellfap_mod_load keeps `api` (it stays valid until unload), fills the callbacks it wants in `mod` and returns
 * CELLFAP_OK. Anything else refuses the mod (the reason given with api->set_error is shown to the player).
 *
 * THREADS. The game calls a mod from two threads, never at the same time: every call is made while the game holds
 * the lock of the dish, so a mod needs no locks of its own for game state.
 *     UI thread:          cellfap_mod_load, unload, world_reset (after load), settings_changed, panel, draw
 *     simulation thread:  before_step, after_step, cell_event, cell_update, world_reset (new dish noticed by a step)
 * The CellFapWorld / CellFapUi / CellFapDraw handles are valid only during the callback they were passed to and only
 * on that thread; API calls with them at any other time return CELLFAP_E_STATE. Threads a mod starts itself must not
 * call the world functions at all (they get CELLFAP_E_STATE as well).
 *
 * ERRORS. Callbacks return CELLFAP_OK (0). Any other value switches the mod off: unload() is called, the library is
 * released and the player sees the text set with api->set_error (or the code). A crash or an endless loop inside
 * native code cannot be caught: it takes the game down with it. Native code runs with all the rights of the game.
 *
 * COMPATIBILITY. New functions are only ever appended to CellFapApi and new fields to CellFapMod; api->struct_size and
 * mod->struct_size say how much of each the other side knows. A mod built for version N runs on every game that
 * speaks version N or later; a game refuses mods that need a newer API than its own.
 */
#ifndef CELLFAP_MOD_H
#define CELLFAP_MOD_H

#include <stddef.h>
#include <stdint.h>

#define CELLFAP_MOD_API_VERSION 1u

#if defined(_WIN32)
#define CELLFAP_EXPORT __declspec(dllexport)
#else
#define CELLFAP_EXPORT __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
#define CELLFAP_EXTERN_C extern "C"
#else
#define CELLFAP_EXTERN_C
#endif
/* Put this in front of the two exported functions. */
#define CELLFAP_MOD_ENTRY CELLFAP_EXTERN_C CELLFAP_EXPORT

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------------------------------ results */
enum {
    CELLFAP_OK = 0,
    CELLFAP_ERROR = -1,     /* generic failure */
    CELLFAP_E_ARG = -2,     /* a bad argument: NULL, not a finite number, unknown name */
    CELLFAP_E_STATE = -3,   /* not allowed now: outside the callback, wrong thread, no dish */
    CELLFAP_E_RANGE = -4,   /* index out of range, or the cell / link / food at that index is gone */
    CELLFAP_E_FULL = -5,    /* a limit of the dish was reached (cells, food, links of a cell) */
    CELLFAP_E_NOT_FOUND = -6
};

/* Opaque handles. */
typedef struct CellFapWorld CellFapWorld; /* the dish, during one callback */
typedef struct CellFapUi CellFapUi;       /* the mod's panel, during panel() */
typedef struct CellFapDraw CellFapDraw;   /* the dish view, during draw() */
typedef struct CellFapMod CellFapMod;
typedef struct CellFapApi CellFapApi;

/* Colours: 0xRRGGBBAA. */
#define CELLFAP_RGBA(r, g, b, a) \
    ((((uint32_t)(r) & 255u) << 24) | (((uint32_t)(g) & 255u) << 16) | (((uint32_t)(b) & 255u) << 8) | ((uint32_t)(a) & 255u))

/* ------------------------------------------------------------------------------------------------ cell types */
/* CellFapCell.type, bits of CellFapMod.cell_types. api->cell_type_name(t) gives the Latin name; the game may add
 * types after the last one listed here (api->world_info().cell_type_count says how many there are). */
enum {
    CELLFAP_PHAGOCYTE = 0, CELLFAP_FLAGELLOCYTE, CELLFAP_PHOTOCYTE, CELLFAP_DEVOROCYTE, CELLFAP_LIPOCYTE,
    CELLFAP_KERATINOCYTE, CELLFAP_BUOYOCYTE, CELLFAP_GLUEOCYTE, CELLFAP_VIROCYTE, CELLFAP_NITROCYTE,
    CELLFAP_STEREOCYTE, CELLFAP_SENSEOCYTE, CELLFAP_MYOCYTE, CELLFAP_NEUROCYTE, CELLFAP_SECROCYTE,
    CELLFAP_STEMOCYTE, CELLFAP_GAMETE, CELLFAP_CILIOCYTE,
    CELLFAP_ELECTROCYTE, CELLFAP_CNIDOCYTE, CELLFAP_CHROMATOCYTE, CELLFAP_LUMINOCYTE, CELLFAP_CYSTOCYTE,
    CELLFAP_GRAPPLER, CELLFAP_FIBROBLAST, CELLFAP_BARORECEPTOR, CELLFAP_ERYTHROCYTE, CELLFAP_CHRONOCYTE,
    CELLFAP_CYCLOSPORINOCYTE, CELLFAP_GRANULOCYTE, CELLFAP_GRIFFOCYTE, CELLFAP_SPIROCYTE, CELLFAP_CHROMOCYTE,
    CELLFAP_SIGNAL_CELL
};
#define CELLFAP_TYPE_BIT(t) (1ull << (t))

/* Drug timers of a cell (CellFapCell.substances[], hours left). */
enum {
    CELLFAP_SUB_AMYLASE = 0, CELLFAP_SUB_HYALURONIC_ACID, CELLFAP_SUB_EPIBRASSINOLIDE, CELLFAP_SUB_BENZYLADIN,
    CELLFAP_SUB_GIBBERELLIC_ACID, CELLFAP_SUB_COLCHICINE, CELLFAP_SUB_PENICILLIN, CELLFAP_SUB_INTERFERON,
    CELLFAP_SUB_RESVERATROL, CELLFAP_SUB_RYANODINE, CELLFAP_SUB_CYCLOHEXIMIDE, CELLFAP_SUB_OSTEOPONTIN,
    CELLFAP_SUB_CHONDROITIN_SULFATE, CELLFAP_SUB_EDROPHONIUM, CELLFAP_SUB_ATRACTYLOSIDE,
    CELLFAP_SUB_COUNT
};

/* What the micro-needle injects (api->inject) or pours into the water (api->release). Doses as in the game's tool:
 * ng for glucose, poisons, drugs and enzymes; % for nitrate; signal units (channel 0..7) for a signal; a count for
 * the mutagen. */
enum {
    CELLFAP_INJECT_GLUCOSE = 0, CELLFAP_INJECT_NITRATE, CELLFAP_INJECT_SIGNAL, CELLFAP_INJECT_CYANIDE,
    CELLFAP_INJECT_TETRODOTOXIN, CELLFAP_INJECT_ADRENALINE, CELLFAP_INJECT_PROTEASE, CELLFAP_INJECT_REGENERANT,
    CELLFAP_INJECT_MUTAGEN, CELLFAP_INJECT_AMYLASE, CELLFAP_INJECT_HYALURONIC_ACID, CELLFAP_INJECT_EPIBRASSINOLIDE,
    CELLFAP_INJECT_BENZYLADIN, CELLFAP_INJECT_GIBBERELLIC_ACID, CELLFAP_INJECT_COLCHICINE, CELLFAP_INJECT_PENICILLIN,
    CELLFAP_INJECT_INTERFERON, CELLFAP_INJECT_RESVERATROL, CELLFAP_INJECT_RYANODINE, CELLFAP_INJECT_CYCLOHEXIMIDE,
    CELLFAP_INJECT_OSTEOPONTIN, CELLFAP_INJECT_CHONDROITIN_SULFATE, CELLFAP_INJECT_EDROPHONIUM,
    CELLFAP_INJECT_ATRACTYLOSIDE
};

/* ------------------------------------------------------------------------------------------------ data */
/* Units: length µm (the dish centre is 0,0; y points up), mass ng, time hours (one tick = 1/400 h), angles radians. */

typedef struct CellFapWorldInfo {
    uint64_t ticks;          /* ticks since the dish started */
    double hours;            /* simulated time */
    int32_t cells;           /* live cells */
    int32_t cell_slots;      /* valid cell indices are 0 .. cell_slots-1 (dead slots have alive == 0) */
    int32_t link_slots;      /* valid link indices are 0 .. link_slots-1 */
    int32_t food;            /* food pieces: indices 0 .. food-1 */
    int32_t genomes;         /* genome slots in the dish (CellFapCell.genome indexes them) */
    int32_t cell_type_count; /* number of cell types this game knows */
    float dish_radius;
    float daylight;          /* 0 at night .. 1 at noon */
    int32_t max_cells, max_food;
    int32_t reserved[8];
} CellFapWorldInfo;

typedef struct CellFapCell {
    /* read-only (set_cell ignores them) */
    uint32_t id;             /* unique for the dish's lifetime; indices are reused, ids never */
    int32_t alive;
    int32_t type;            /* CELLFAP_<TYPE>; change it through the mode (set_cell_mode) or the genome */
    int32_t mode;            /* 0..127, change with set_cell_mode */
    int32_t genome;          /* genome slot in the dish (genome_text, get_mode) */
    int32_t link_count;
    float radius;            /* follows the mass */
    float actuator;          /* last programmable output (swim force, contraction...), for visuals */
    /* writable with set_cell */
    float x, y, vx, vy;      /* µm, µm per tick */
    float angle, angular_velocity;
    float mass;              /* clamped to 0..capacity of the type */
    float age;               /* hours */
    float nitrogen;          /* 0..1 */
    float injury;            /* 0..1 (1 kills) */
    int32_t tag;             /* 0 player, 1 level, 2 contamination, 3..5 infected, 6 hybrid */
    int32_t mirrored;
    int32_t telomeres;       /* -1 infinite */
    int32_t mutations;
    float signals[8];        /* S1..S8 */
    float charge, stun, paralysis, stimulant;
    float substances[CELLFAP_SUB_COUNT]; /* CELLFAP_SUB_* timers, hours */
    float memory[4];         /* a program cell's own memory */
    float reserved[8];
} CellFapCell;

typedef struct CellFapLink {
    int32_t alive;
    int32_t a, b;            /* cell indices; b == -1: glued to the dish wall at wall_x, wall_y */
    int32_t from_division;
    float extra_length;      /* µm added to the two radii */
    float stiffness;         /* 0..1 angular stiffness */
    float angle_a, angle_b;  /* anchors in each cell's own frame */
    float wall_x, wall_y;
    float reserved[4];
} CellFapLink;

typedef struct CellFapFood {
    float x, y, vx, vy;
    float mass;              /* ng */
    float coating;           /* 0..1 */
} CellFapFood;

/* One programmable value of a mode (the same as in the genome editor). */
typedef struct CellFapProg {
    int32_t input;           /* 0 fixed, 1..4 S1..S4, 5 mass, 6 age, 7 nitrogen, 8 links, 9..12 S5..S8, 13 light, 14 food, 15 injury */
    int32_t kind;            /* 0 linear a*x+b, 1 step x<c ? a : b, 2 wave a*sin(2pi x/c)+b */
    float a, b, c;
} CellFapProg;

/* A genome mode (a "mode" of the genome editor). */
typedef struct CellFapMode {
    int32_t type;
    float color[3];
    int32_t make_adhesin, prioritize;
    float split_mass, split_ratio, nutrient_priority, split_angle;
    float child_angle[2];
    int32_t child_mode[2];
    int32_t keep_adhesin[2], mirror[2];
    float adhesin_stiffness, cytoskeleton, adhesin_length;
    int32_t max_connections;
    CellFapProg prog[11];    /* swim, density, contraction, bend, lift, stem1, stem2, neuro1..4 */
    int32_t stem_mode[2];
    int32_t neuro_channel[4];
    int32_t virus_copy_from;
    int32_t sense_target, sense_channel;
    float sense_output, sense_threshold;
    float sense_color[3];
    int32_t secretion;
    int32_t gamete_compatible_mode, gamete_mode_after, gamete_telomeres;
    int32_t reserved[8];
} CellFapMode;

/* cell_event() */
enum {
    CELLFAP_EVENT_BORN = 1,    /* a cell appeared without a division: put in by the player, a mod, contamination */
    CELLFAP_EVENT_DIVIDED = 2, /* cell = the child in the parent's slot (keeps its id), other = the second child */
    CELLFAP_EVENT_DIED = 3     /* cell = the slot it had (now free); id, type, x, y are the dead cell's */
};
#define CELLFAP_EVENT_BIT(k) (1u << (k))

typedef struct CellFapEvent {
    int32_t kind;
    int32_t cell, other;
    uint32_t id, other_id;
    int32_t type;
    float x, y;
} CellFapEvent;

/* request_dish() */
enum { CELLFAP_DISH_SAVE = 1, CELLFAP_DISH_LOAD = 2 };

/* ------------------------------------------------------------------------------------------------ the mod */
struct CellFapMod {
    /* Set by the game before cellfap_mod_load. */
    uint32_t struct_size;      /* sizeof(CellFapMod) of the game */
    uint32_t api_version;      /* CELLFAP_MOD_API_VERSION of the game */
    const char* id;            /* the mod's id from mod.txt */
    const char* lib_dir;       /* folder the library was loaded from (a cache: do not write there) */
    const char* data_dir;      /* a folder of the mod's own for its files (kept between runs) */
    const char* platform;      /* "windows-x64", "android-arm64-v8a", ... */
    void* host;                /* the game's: do not touch */

    /* Filled by the mod (everything optional; NULL = not used). The game reads cell_types and events anew before
     * every step, so a mod may change them at any time. */
    void* user;                /* the mod's own pointer */
    const char* panel_title;   /* heading of the panel (default: the mod's name); must outlive the mod */
    uint64_t cell_types;       /* CELLFAP_TYPE_BIT(t): cell_update() for every live cell of type t, every tick */
    uint32_t events;           /* CELLFAP_EVENT_BIT(k): cell_event() for these kinds */
    uint32_t flags;            /* reserved, 0 */

    /* UI thread, when the mod is switched off or the game ends. No dish. */
    int (*unload)(CellFapMod* mod);
    /* Simulation thread, before each tick. *skip_physics = 1 skips the game's own physics and biology for this tick
     * (time moves on, after_step still runs): replace the simulation with your own. */
    int (*before_step)(CellFapMod* mod, CellFapWorld* world, int* skip_physics);
    /* Simulation thread, after each tick: the whole dish may be read and changed. */
    int (*after_step)(CellFapMod* mod, CellFapWorld* world);
    /* Simulation thread, after each tick, before after_step: once for every live cell whose type is in cell_types.
     * A custom behaviour for a cell type: move it, feed it, change its signals, kill it... */
    int (*cell_update)(CellFapMod* mod, CellFapWorld* world, int32_t cell);
    /* Simulation thread, after each tick, before cell_update: births, divisions and deaths of that tick (events). */
    int (*cell_event)(CellFapMod* mod, CellFapWorld* world, const CellFapEvent* event);
    /* Right after load with the dish the mod finds (UI thread), and whenever the dish is replaced (a new experiment,
     * a loaded file) — on the thread that notices it first. */
    int (*world_reset)(CellFapMod* mod, CellFapWorld* world);
    /* UI thread: the player changed one of the mod's settings (mod.txt `setting`). world may be NULL. */
    int (*settings_changed)(CellFapMod* mod, CellFapWorld* world);
    /* UI thread, every frame the mod's panel is shown: build it with api->ui_*. */
    int (*panel)(CellFapMod* mod, CellFapWorld* world, CellFapUi* ui);
    /* UI thread, every frame the dish is on screen: draw over it with api->draw_* (world coordinates). */
    int (*draw)(CellFapMod* mod, CellFapWorld* world, CellFapDraw* draw);

    void* reserved[16];
};

/* ------------------------------------------------------------------------------------------------ the game */
struct CellFapApi {
    uint32_t api_version;      /* CELLFAP_MOD_API_VERSION of the game */
    uint32_t struct_size;      /* sizeof(CellFapApi) of the game: functions past it are missing */

    /* ---- the mod itself (any callback and cellfap_mod_load) */
    void (*log)(CellFapMod* mod, const char* text_utf8);          /* a line in the mod's log (the mods window) */
    void (*message)(CellFapMod* mod, const char* text_utf8);      /* a message on the screen */
    void (*set_error)(CellFapMod* mod, const char* text_utf8);    /* why the next error return happens */
    /* A setting of mod.txt: number, flag (0/1) or the index of the chosen option; `fallback` if there is none. */
    double (*setting)(CellFapMod* mod, const char* key, double fallback);
    /* Change one of the mod's own settings (remembered in settings/mods.ini). */
    int (*set_setting)(CellFapMod* mod, const char* key, double value);
    /* A file of the mod (any file in its folder or .cfmod). The bytes stay valid until the mod is unloaded. */
    int (*mod_file)(CellFapMod* mod, const char* path, const void** data, size_t* size);
    /* A genome (the text of a .genome.txt file) for the genome browser and for add_cell. Returns a handle >= 0. */
    int32_t (*register_genome)(CellFapMod* mod, const char* name, const char* genome_text);
    /* A dypt program for programmable signal cells (the program editor's examples). */
    int (*register_program)(CellFapMod* mod, const char* name, const char* dypt_source);
    /* Save the dish to / open it from substrates/<file_name>.substrate. Done by the game after the callback. */
    int (*request_dish)(CellFapMod* mod, int32_t action, const char* file_name);
    /* Latin name of a cell type ("Phagocyte"); NULL past the last type. */
    const char* (*cell_type_name)(int32_t type);
    /* Names of the dish parameters for get_param / set_param; NULL past the last one. */
    const char* (*param_name)(int32_t index);

    /* ---- the dish (only with the world handed to the current callback) */
    int (*world_info)(CellFapWorld* world, CellFapWorldInfo* out);
    int (*get_param)(CellFapWorld* world, const char* name, double* value);
    int (*set_param)(CellFapWorld* world, const char* name, double value); /* clamped to the parameter's range */

    int (*get_cell)(CellFapWorld* world, int32_t cell, CellFapCell* out);
    int (*set_cell)(CellFapWorld* world, int32_t cell, const CellFapCell* in); /* the writable fields */
    int32_t (*find_cell)(CellFapWorld* world, uint32_t id);                  /* index or -1 */
    int32_t (*pick_cell)(CellFapWorld* world, float x, float y);             /* the cell under a point or -1 */
    /* Indices of live cells whose centre is within radius of (x, y); returns how many there are (may exceed cap). */
    int32_t (*cells_near)(CellFapWorld* world, float x, float y, float radius, int32_t* out, int32_t cap);
    /* A new cell of a genome registered by this mod (mode -1: its initial mode, mass <= 0: the default). Index or < 0. */
    int32_t (*add_cell)(CellFapWorld* world, int32_t genome_handle, float x, float y, float angle, int32_t mode, float mass);
    /* A new cell sharing the genome of an existing one. */
    int32_t (*add_cell_like)(CellFapWorld* world, int32_t source_cell, float x, float y, float angle, int32_t mode, float mass);
    int (*kill_cell)(CellFapWorld* world, int32_t cell, int32_t leave_food);
    int (*set_cell_mode)(CellFapWorld* world, int32_t cell, int32_t mode);
    int (*inject)(CellFapWorld* world, int32_t cell, int32_t substance, float amount, int32_t channel);
    int (*release)(CellFapWorld* world, float x, float y, int32_t substance, float amount, int32_t channel);

    int (*get_link)(CellFapWorld* world, int32_t link, CellFapLink* out);
    /* Links of a cell; returns how many it has (may exceed cap). */
    int32_t (*cell_links)(CellFapWorld* world, int32_t cell, int32_t* out, int32_t cap);
    int32_t (*add_link)(CellFapWorld* world, int32_t a, int32_t b, float extra_length, float stiffness); /* index or < 0 */
    int (*set_link)(CellFapWorld* world, int32_t link, float extra_length, float stiffness);
    int (*remove_link)(CellFapWorld* world, int32_t link);

    int (*get_food)(CellFapWorld* world, int32_t index, CellFapFood* out);
    int (*set_food)(CellFapWorld* world, int32_t index, const CellFapFood* in);
    int (*add_food)(CellFapWorld* world, float x, float y, float mass);
    int (*remove_food)(CellFapWorld* world, int32_t index); /* the last piece takes its index */

    /* The genome in a dish slot as .genome.txt text. Writes up to cap bytes (always 0-terminated when cap > 0) and
     * stores the full length (without the 0) in *needed. */
    int (*genome_text)(CellFapWorld* world, int32_t genome, char* buf, size_t cap, size_t* needed);
    int (*get_mode)(CellFapWorld* world, int32_t genome, int32_t mode, CellFapMode* out);
    /* Give a cell (or its whole organism: every cell reachable through links) a new genome / one changed mode. */
    int (*set_cell_genome)(CellFapWorld* world, int32_t cell, const char* genome_text, int32_t whole_organism);
    int (*edit_cell_mode)(CellFapWorld* world, int32_t cell, int32_t mode, const CellFapMode* in, int32_t whole_organism);

    /* ---- the panel (only inside panel()). Labels must be unique within the panel ("Name##2" hides the suffix). */
    void (*ui_text)(CellFapUi* ui, const char* text);
    void (*ui_text_colored)(CellFapUi* ui, uint32_t rgba, const char* text);
    void (*ui_heading)(CellFapUi* ui, const char* text);
    void (*ui_separator)(CellFapUi* ui);
    void (*ui_same_line)(CellFapUi* ui);
    int (*ui_button)(CellFapUi* ui, const char* label);                                   /* 1 when clicked */
    int (*ui_checkbox)(CellFapUi* ui, const char* label, int32_t* value);                 /* 1 when changed */
    int (*ui_slider_float)(CellFapUi* ui, const char* label, float* value, float min, float max);
    int (*ui_slider_int)(CellFapUi* ui, const char* label, int32_t* value, int32_t min, int32_t max);
    int (*ui_input_text)(CellFapUi* ui, const char* label, char* buf, size_t cap);        /* 1 when changed */

    /* ---- over the dish (only inside draw()); world coordinates, widths in screen pixels */
    void (*draw_line)(CellFapDraw* draw, float x0, float y0, float x1, float y1, uint32_t rgba, float width);
    void (*draw_circle)(CellFapDraw* draw, float x, float y, float radius, uint32_t rgba, float width); /* width 0: filled */
    void (*draw_text)(CellFapDraw* draw, float x, float y, uint32_t rgba, const char* text);
};

/* What the library exports. */
typedef uint32_t (*CellFapModApiVersionFn)(void);
typedef int (*CellFapModLoadFn)(const CellFapApi* api, CellFapMod* mod);

#ifdef __cplusplus
}
#endif

#endif /* CELLFAP_MOD_H */
