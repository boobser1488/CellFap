#pragma once
// The game's side of sdk/cellfap_mod.h: the table of functions native mods call, the handles they get, and the
// dish parameters by name (shared with the dypt functions узнать / задать).
//
// Every world function checks that its handle belongs to the callback running right now on this thread
// (ModCallScope): a library that keeps the handle, or calls from a thread of its own, gets CELLFAP_E_STATE instead of
// touching a dish the simulation thread is changing.
#include "cellfap_mod.h"
#include "genome.hpp"
#include "native_mods.hpp"

#include <memory>
#include <string>
#include <thread>
#include <vector>

class ModManager;
class World;
struct Settings;

// ---------------------------------------------------------------- dish parameters by name
struct DishParam {
    const char* name;    // English (native API)
    const char* ru;      // Russian (dypt), may be null
    enum Kind { Float, Int, Bool } kind;
    size_t offset;       // in Settings
    double lo, hi;
    bool writable;
};
const DishParam* findDishParam(const std::string& name);  // either name
const DishParam* dishParamAt(int i);                      // null past the end
double getDishParam(const Settings& s, const DishParam& p);
void setDishParam(Settings& s, const DishParam& p, double v);  // clamped

// ---------------------------------------------------------------- the panel and the overlay
// Drawn by the game's interface (Dear ImGui, src/ui_mods.cpp); the simulation library only forwards to them, so no
// ImGui state ever crosses into a mod's library.
class ModUi {
public:
    virtual ~ModUi() = default;
    virtual void text(const char* s) = 0;
    virtual void textColored(uint32_t rgba, const char* s) = 0;
    virtual void heading(const char* s) = 0;
    virtual void separator() = 0;
    virtual void sameLine() = 0;
    virtual bool button(const char* label) = 0;
    virtual bool checkbox(const char* label, int32_t* v) = 0;
    virtual bool sliderFloat(const char* label, float* v, float lo, float hi) = 0;
    virtual bool sliderInt(const char* label, int32_t* v, int32_t lo, int32_t hi) = 0;
    virtual bool inputText(const char* label, char* buf, size_t cap) = 0;
};
class ModDraw {
public:
    virtual ~ModDraw() = default;
    virtual void line(float x0, float y0, float x1, float y1, uint32_t rgba, float width) = 0;
    virtual void circle(float x, float y, float r, uint32_t rgba, float width) = 0;
    virtual void text(float x, float y, uint32_t rgba, const char* s) = 0;
};

// ---------------------------------------------------------------- one loaded library
struct NativeMod {
    ModManager* mgr = nullptr;
    int index = -1;  // in mgr->mods (stable while the mod runs: a rescan stops every mod first)
    std::vector<std::unique_ptr<SharedLibrary>> libs;  // dependencies first, the entry library last
    CellFapMod mod{};
    std::string id, libDir, dataDir;  // what mod.id / lib_dir / data_dir point at
    std::string error;                // set_error
    std::vector<Genome> genomes;      // register_genome handles
    bool loaded = false;              // cellfap_mod_load went through (unload is due)
};

const CellFapApi* modApi();

// ---------------------------------------------------------------- handles of one call
struct CellFapWorld {
    World* world = nullptr;
    NativeMod* mod = nullptr;
    std::thread::id thread;
    bool active = false;
};
struct CellFapUi {
    ModUi* ui = nullptr;
    std::thread::id thread;
    bool active = false;
};
struct CellFapDraw {
    ModDraw* draw = nullptr;
    std::thread::id thread;
    bool active = false;
};

// Makes a handle valid for the length of a callback (and restores what was there: calls may nest, e.g. world_reset
// right after load).
template <class H>
class ModCallScope {
public:
    explicit ModCallScope(H& h) : h_(h), saved_(h) { h_.thread = std::this_thread::get_id(); h_.active = true; }
    ~ModCallScope() { h_ = saved_; }
    ModCallScope(const ModCallScope&) = delete;
    ModCallScope& operator=(const ModCallScope&) = delete;

private:
    H& h_;
    H saved_;
};

// Converts between the game's structures and the SDK's (also used by tests).
void cellToApi(const World& w, int index, CellFapCell& out);
void modeToApi(const Mode& m, CellFapMode& out);
bool modeFromApi(const CellFapMode& in, Mode& m);  // false: a value out of range or not a number
