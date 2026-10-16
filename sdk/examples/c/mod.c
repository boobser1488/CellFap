/*
 * «Водоворот»: an example native CellFap mod in plain C99 (docs/mods.md, «Нативный код»).
 *
 * What it shows:
 *   after_step     the whole dish each tick: a whirlpool pushes every cell around the centre
 *   cell_update    a behaviour of one cell type: phagocytes heal their wounds
 *   cell_event     births, divisions and deaths, counted
 *   before_step    skipping the game's physics (setting «пауза»)
 *   world_reset    a new dish
 *   panel          its own controls in the «Модификации» window
 *   draw           the whirlpool drawn over the dish
 *
 * Build (the game's CMake does it for you; by hand, MSYS2 UCRT64):
 *   gcc -std=c99 -O2 -shared -static-libgcc -I../../ mod.c -o native_example.dll
 * then put it into <mod>/native/windows-x64/ next to mod.txt.
 */
#include "cellfap_mod.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct State {
    const CellFapApi* api;
    unsigned long long ticks;
    long long born, divided, died;
    int32_t show; /* draw the whirlpool */
} State;

/* One library is loaded once: a static is the mod's state. */
static State g;

static int setting_on(CellFapMod* mod, const char* key) { return g.api->setting(mod, key, 0.0) != 0.0; }

static int before_step(CellFapMod* mod, CellFapWorld* world, int* skip_physics) {
    (void)world;
    if (setting_on(mod, "пауза")) *skip_physics = 1;
    return CELLFAP_OK;
}

static int cell_event(CellFapMod* mod, CellFapWorld* world, const CellFapEvent* e) {
    (void)mod;
    (void)world;
    if (e->kind == CELLFAP_EVENT_BORN) g.born++;
    else if (e->kind == CELLFAP_EVENT_DIVIDED) g.divided++;
    else if (e->kind == CELLFAP_EVENT_DIED) g.died++;
    return CELLFAP_OK;
}

/* Called for every live phagocyte (mod->cell_types). */
static int cell_update(CellFapMod* mod, CellFapWorld* world, int32_t index) {
    CellFapCell c;
    if (!setting_on(mod, "лечение")) return CELLFAP_OK;
    if (g.api->get_cell(world, index, &c) != CELLFAP_OK) return CELLFAP_ERROR;
    if (c.injury > 0.0f) {
        c.injury *= 0.98f;
        if (c.injury < 0.001f) c.injury = 0.0f;
        g.api->set_cell(world, index, &c);
    }
    return CELLFAP_OK;
}

static int after_step(CellFapMod* mod, CellFapWorld* world) {
    const CellFapApi* api = g.api;
    CellFapWorldInfo info;
    float strength;
    int32_t i;
    g.ticks++;
    if (setting_on(mod, "сбой")) {
        api->set_error(mod, "включена настройка «Проверка: after_step вернёт ошибку»");
        return CELLFAP_ERROR;
    }
    strength = (float)api->setting(mod, "сила", 0.02);
    if (strength <= 0.0f) return CELLFAP_OK;
    if (api->world_info(world, &info) != CELLFAP_OK) return CELLFAP_ERROR;
    for (i = 0; i < info.cell_slots; ++i) {
        CellFapCell c;
        float r, rel, k;
        if (api->get_cell(world, i, &c) != CELLFAP_OK || !c.alive) continue;
        r = sqrtf(c.x * c.x + c.y * c.y);
        if (r < 1.0f) continue;
        /* Counter-clockwise, strongest halfway to the rim, still at the centre and at the glass. */
        rel = r / info.dish_radius;
        k = strength * 4.0f * rel * (1.0f - rel);
        c.vx += -c.y / r * k;
        c.vy += c.x / r * k;
        api->set_cell(world, i, &c);
    }
    return CELLFAP_OK;
}

static int world_reset(CellFapMod* mod, CellFapWorld* world) {
    CellFapWorldInfo info;
    char line[128];
    g.born = g.divided = g.died = 0;
    g.ticks = 0;
    if (g.api->world_info(world, &info) != CELLFAP_OK) return CELLFAP_ERROR;
    snprintf(line, sizeof line, "чашка: %d клеток, радиус %.0f мкм", (int)info.cells, info.dish_radius);
    g.api->log(mod, line);
    return CELLFAP_OK;
}

static int settings_changed(CellFapMod* mod, CellFapWorld* world) {
    char line[128];
    (void)world;
    snprintf(line, sizeof line, "настройки: сила %.3f, лечение %d, пауза %d", g.api->setting(mod, "сила", 0.0),
             setting_on(mod, "лечение"), setting_on(mod, "пауза"));
    g.api->log(mod, line);
    return CELLFAP_OK;
}

static int panel(CellFapMod* mod, CellFapWorld* world, CellFapUi* ui) {
    const CellFapApi* api = g.api;
    char line[160];
    float s = (float)api->setting(mod, "сила", 0.02);
    snprintf(line, sizeof line, "Тиков: %llu · рождений: %lld · делений: %lld · гибелей: %lld", g.ticks, g.born, g.divided, g.died);
    api->ui_text(ui, line);
    if (api->ui_slider_float(ui, "Сила водоворота", &s, 0.0f, 0.2f)) api->set_setting(mod, "сила", s);
    api->ui_checkbox(ui, "Рисовать вихрь", &g.show);
    if (api->ui_button(ui, "Насыпать пищу в центр") && world) {
        int k, added = 0;
        for (k = 0; k < 24; ++k) {
            const float a = (float)k * 0.2618f, r = 20.0f + 4.0f * (float)k;
            added += api->add_food(world, r * cosf(a), r * sinf(a), 0.3f) == CELLFAP_OK;
        }
        snprintf(line, sizeof line, "насыпано порций: %d", added);
        api->log(mod, line);
    }
    return CELLFAP_OK;
}

static int draw(CellFapMod* mod, CellFapWorld* world, CellFapDraw* d) {
    const CellFapApi* api = g.api;
    CellFapWorldInfo info;
    int ring, k;
    if (!g.show || api->setting(mod, "сила", 0.0) <= 0.0) return CELLFAP_OK;
    if (api->world_info(world, &info) != CELLFAP_OK) return CELLFAP_ERROR;
    for (ring = 1; ring <= 3; ++ring) {
        const float r = info.dish_radius * 0.25f * (float)ring;
        const uint32_t col = CELLFAP_RGBA(90, 170, 255, 70 + 40 * (ring == 2));
        api->draw_circle(d, 0.0f, 0.0f, r, col, 1.5f);
        /* Arrows along the flow. */
        for (k = 0; k < 8; ++k) {
            const float a = (float)k * 0.7854f + (float)g.ticks * 0.002f * (float)ring;
            const float x = r * cosf(a), y = r * sinf(a), tx = -sinf(a), ty = cosf(a), len = r * 0.12f;
            api->draw_line(d, x, y, x + tx * len, y + ty * len, CELLFAP_RGBA(120, 200, 255, 160), 2.0f);
        }
    }
    api->draw_text(d, 0.0f, 0.0f, CELLFAP_RGBA(200, 230, 255, 200), "водоворот");
    return CELLFAP_OK;
}

static int unload(CellFapMod* mod) {
    char line[128];
    snprintf(line, sizeof line, "выгружен после %llu тиков", g.ticks);
    g.api->log(mod, line);
    return CELLFAP_OK;
}

CELLFAP_MOD_ENTRY uint32_t cellfap_mod_api_version(void) { return CELLFAP_MOD_API_VERSION; }

CELLFAP_MOD_ENTRY int cellfap_mod_load(const CellFapApi* api, CellFapMod* mod) {
    char line[128];
    if (api->api_version < 1 || api->struct_size < sizeof(CellFapApi)) return CELLFAP_ERROR;
    memset(&g, 0, sizeof g);
    g.api = api;
    g.show = 1;
    mod->user = &g;
    mod->panel_title = "Водоворот";
    mod->cell_types = CELLFAP_TYPE_BIT(CELLFAP_PHAGOCYTE);
    mod->events = CELLFAP_EVENT_BIT(CELLFAP_EVENT_BORN) | CELLFAP_EVENT_BIT(CELLFAP_EVENT_DIVIDED) | CELLFAP_EVENT_BIT(CELLFAP_EVENT_DIED);
    mod->unload = unload;
    mod->before_step = before_step;
    mod->after_step = after_step;
    mod->cell_update = cell_update;
    mod->cell_event = cell_event;
    mod->world_reset = world_reset;
    mod->settings_changed = settings_changed;
    mod->panel = panel;
    mod->draw = draw;
    snprintf(line, sizeof line, "нативный пример загружен (%s, API %u)", mod->platform, (unsigned)api->api_version);
    api->log(mod, line);
    return CELLFAP_OK;
}
