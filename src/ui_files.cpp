// Genome browser (built-in organisms, the player's bank, Cell Lab files) and sharing dishes and genomes.
//
// Sharing writes the file in our own format (.genome / .substrate with a signature, so a renamed file is still
// recognised by importPath) and hands it on: on Android through the system share sheet with a short message (the
// messengers show it next to the file), on the desktop by showing the file in Explorer and copying the message.
#include "app.hpp"
#include "sim/cellab_import.hpp"
#include "sim/layout.hpp"
#include "sim/presets.hpp"
#include "sim/serialize.hpp"
#include "ui_widgets.hpp"

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include <filesystem>

using namespace uiw;

namespace {
constexpr const char* kShareNote =
    "Образец из лаборатории CellFap. Поместите его под микроскоп и посмотрите, что из него вырастет.";
constexpr const char* kShareLink = "";  // download page of the Android app: appended to the message when set

bool toggle(const char* label, bool on, ImVec2 size = {0, 0}) {
    if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.38f));
    bool r = ImGui::Button(label, size);
    if (on) ImGui::PopStyleColor();
    return r;
}
}  // namespace

// ---------------------------------------------------------------------------- sharing
void App::shareFile(const std::string& path, const std::string& fileName, const std::string& what) {
    std::string msg = what + "\n\n" + kShareNote + "\nСкачайте приложение CellFap для Android";
    msg += kShareLink[0] ? std::string(" на ") + kShareLink : std::string(".");
#ifdef __ANDROID__
    glfwAndroidShareFile(path.c_str(), fileName.c_str(), msg.c_str());
#else
    (void)fileName;
    ImGui::SetClipboardText(msg.c_str());
#ifdef _WIN32
    std::wstring wp;
    if (int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0); n > 0) {
        wp.resize((size_t)n - 1);
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wp.data(), n);
    }
    for (wchar_t& c : wp)
        if (c == L'/') c = L'\\';
    std::wstring args = L"/select,\"" + wp + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    toast("Файл показан в проводнике, текст сообщения скопирован — приложите файл в мессенджере");
#else
    toast("Файл: " + path + " (текст сообщения скопирован)");
#endif
#endif
}

// Ways to pass the genome on: Auto = the Cell Lab format when nothing is lost (it opens in Cell Lab, MinLab and here),
// else ours; CellLab = the Cell Lab format whatever is lost (the losses are told); CellFap = our text format, lossless,
// named .genome.txt so that nobody tries to open it in Cell Lab.
void App::shareGenome(const Genome& g, ShareAs how) {
    const std::string name = safeFileName(g.name, "геном");
    const std::string title = g.name.empty() ? name : g.name;
    const std::string dir = dataDir("share");
    std::string err;
    // Cell Lab / MinLab list only files named "<name>.genome" and take the title from the name.
    if (how != ShareAs::CellFap && exportCellLabGenome(g, dir + "/" + name + ".genome", err) && (err.empty() || how == ShareAs::CellLab)) {
        shareFile(dir + "/" + name + ".genome", name + ".genome", "Геном «" + title + "» для Cell Lab, MinLab и CellFap");
        if (!err.empty()) toast("Для Cell Lab не поместилось: " + err);  // after the share hint: this one matters more
        return;
    }
    if (how == ShareAs::CellLab) {
        toast("Не удалось подготовить геном для Cell Lab: " + err);
        return;
    }
    std::string path = dir + "/" + name + ".genome.txt";
    if (!saveGenome(g, path)) {
        toast("Не удалось сохранить геном");
        return;
    }
    shareFile(path, name + ".genome.txt", "Геном «" + title + "» для CellFap");
}

// The share choices as menu items (inside a popup or a menu section).
void App::genomeShareItems(const Genome& g) {
    if (ImGui::MenuItem("Лучшим форматом")) shareGenome(g, ShareAs::Auto);
    if (!mobile_ && ImGui::IsItemHovered()) ImGui::SetTooltip("Формат Cell Lab, если в нём ничего не теряется; иначе текст CellFap");
    if (ImGui::MenuItem("Для Cell Lab / MinLab")) shareGenome(g, ShareAs::CellLab);
    if (!mobile_ && ImGui::IsItemHovered()) ImGui::SetTooltip("Формат Cell Lab, даже если что-то не поместится (игра скажет, что именно)");
    if (ImGui::MenuItem("Текстом CellFap (без потерь)")) shareGenome(g, ShareAs::CellFap);
    if (mobile_ && ImGui::MenuItem("Открыть в MinLab / Cell Lab")) openInCellLab(&g);
}

void App::dishShareItems() {
    if (ImGui::MenuItem("Чашкой CellFap (без потерь)")) shareDish(false);
    if (ImGui::MenuItem("Для Cell Lab / MinLab")) shareDish(true);
    if (!mobile_ && ImGui::IsItemHovered())
        ImGui::SetTooltip("Формат Cell Lab: свои типы клеток CellFap заменяются ближайшими, в геноме не больше 40 мод, "
                          "скорости не сохраняются");
    if (mobile_ && ImGui::MenuItem("Открыть в MinLab / Cell Lab")) openInCellLab(nullptr);
}
void App::shareDish(bool forCellLab) {
    std::string name = safeFileName(ui_.substrateFile, "чашка");
    std::string path = dataDir("share") + "/" + name + ".substrate";
    char what[256];
    std::string lossNote;
    if (forCellLab) {
        CellLabExportInfo info;
        std::string err;
        if (!exportCellLabSubstrate(*world_, path, info, err)) {
            toast("Не удалось подготовить чашку для Cell Lab: " + err);
            return;
        }
        std::string lost;
        auto add = [&](const std::string& s) { lost += (lost.empty() ? "" : "; ") + s; };
        if (info.genomesCut) add("геномов больше 40 мод: " + std::to_string(info.genomesCut));
        if (info.ownTypes) add("типов CellFap заменено ближайшими: " + std::to_string(info.ownTypes));
        if (info.otherLosses) add("сенсоров, программ и сигналов без аналога: " + std::to_string(info.otherLosses));
        lossNote = lost.empty() ? "" : "Чашка для Cell Lab / MinLab. Не поместилось: " + lost;
        std::snprintf(what, sizeof(what), "Чашка Петри «%s» для Cell Lab и MinLab: %d клеток, %.1f ч", name.c_str(), info.cells,
                      world_->timeHours());
    } else {
        if (!saveWorld(*world_, path.c_str())) {
            toast("Не удалось сохранить чашку");
            return;
        }
        std::snprintf(what, sizeof(what), "Чашка Петри «%s» для CellFap: %d клеток, %.1f ч", name.c_str(), world_->cellCount(),
                      world_->timeHours());
    }
    shareFile(path, name + ".substrate", what);
    if (!lossNote.empty()) toast(lossNote);  // after the share hint: this one matters more
}

// The file goes straight into Cell Lab / MinLab (phone): Cell Lab format, losses told in a toast.
void App::openInCellLab(const Genome* genomeToSend) {
    const bool genome = genomeToSend != nullptr;
    const std::string dir = dataDir("share");
    std::string name, err;
    if (genome) {
        const Genome& g = *genomeToSend;
        name = safeFileName(g.name, "геном") + ".genome";
        if (!exportCellLabGenome(g, dir + "/" + name, err)) {
            toast("Не удалось подготовить геном: " + err);
            return;
        }
    } else {
        name = safeFileName(ui_.substrateFile, "чашка") + ".substrate";
        CellLabExportInfo info;
        if (!exportCellLabSubstrate(*world_, dir + "/" + name, info, err)) {
            toast("Не удалось подготовить чашку: " + err);
            return;
        }
        if (info.genomesCut) err += (err.empty() ? "" : "; ") + std::string("геномов больше 40 мод: ") + std::to_string(info.genomesCut);
        if (info.ownTypes) err += (err.empty() ? "" : "; ") + std::string("типов CellFap заменено: ") + std::to_string(info.ownTypes);
        if (info.otherLosses) err += (err.empty() ? "" : "; ") + std::string("без аналога: ") + std::to_string(info.otherLosses);
    }
#ifdef __ANDROID__
    glfwAndroidOpenInApp((dir + "/" + name).c_str(), name.c_str(), genome ? 1 : 0);
    if (!err.empty()) toast("Для Cell Lab не поместилось: " + err);
#else
    toast("Файл для Cell Lab: " + dir + "/" + name + (err.empty() ? "" : " (не поместилось: " + err + ")"));
#endif
}

// ---------------------------------------------------------------------------- genome browser
void App::loadGenomeIntoEditor(const Genome& g) {
    commitGenome();
    ui_.editGenome = g;
    std::snprintf(ui_.genomeFile, sizeof(ui_.genomeFile), "%s", safeFileName(g.name, "геном").c_str());  // Save keeps this name
    ui_.editMode = g.initialMode;
    selCell_ = -1;
    editorTick_ = 1 << 20;  // show the grown organism
    previewAutoFrame_ = true;
    ui_.previewDirty = true;
    commitGenome();
}

void App::openGenomeBrowser() {
    genomeBrowser_ = true;
    expBrowser_ = false;
    gbDirty_ = true;
    gbSel_ = -1;
    gbAfterOpen_ = Screen::Genome;
    gbFocus_ = true;
}

void App::rebuildGenomeList() {
    gbEntries_.clear();
    for (const GenomePreset& p : genomePresets()) {
        GenomeEntry e;
        e.name = p.name;
        e.description = p.description;
        e.source = 0;
        e.genome = p.genome;
        gbEntries_.push_back(std::move(e));
    }
    // The bank: our genomes (.genome.txt from older versions, .genome) and Cell Lab files people put there.
    auto addDir = [&](const char* sub) {
        const std::string dir = dataDir(sub);
        for (const char* ext : {".genome.txt", ".genome"})
            for (const std::string& f : listFiles(dir, ext)) {
                if (std::strcmp(ext, ".genome") == 0 && f.size() > 11 && f.compare(f.size() - 11, 11, ".genome.txt") == 0) continue;
                GenomeEntry e;
                e.path = dir + "/" + f;
                std::string err;
                char head[16] = {};
                if (FILE* fp = openFileUtf8(e.path, "rb")) {
                    std::fread(head, 1, 15, fp);
                    std::fclose(fp);
                }
                if (std::strncmp(head, "CellFapGenome", 13) == 0) {
                    if (!loadGenome(e.genome, e.path)) continue;
                    e.source = 1;
                } else {
                    if (!importCellLabGenome(e.genome, e.path, err)) continue;
                    e.source = 2;
                }
                e.name = e.genome.name.empty() ? f : e.genome.name;
                e.description = f;
                gbEntries_.push_back(std::move(e));
            }
    };
    addDir("genomes");
    addDir("import");
    gbDirty_ = false;
    ++gbGeneration_;
}

// Small picture of the grown organism: the last stage of its layout, normalised to a unit square.
void App::buildGenomeThumb(GenomeEntry& e) {
    e.previewReady = true;  // ordered: the picture comes from the worker thread a moment later
    struct Out {
        std::vector<Thumb> thumb;
        int cells = 0;
    };
    auto out = std::make_shared<Out>();
    const Genome g = e.genome;
    const std::string key = e.path + "\n" + e.name;
    const int gen = gbGeneration_;
    runInBackground(
        [out, g] {
            GenomeLayout L = buildGenomeLayout(g, 72, 700);
            if (L.stages.empty()) return;
            const LayoutStage& st = L.stages.back();
            out->cells = (int)st.cells.size();
            float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
            for (const LayoutCell& c : st.cells) {
                float r = radiusForMass(c.mass, CellType::Phagocyte);
                lo[0] = std::min(lo[0], c.pos.x - r), lo[1] = std::min(lo[1], c.pos.y - r);
                hi[0] = std::max(hi[0], c.pos.x + r), hi[1] = std::max(hi[1], c.pos.y + r);
                if (g.modes[c.mode].type == CellType::Flagellocyte) {  // the tail trails ~2 radii behind
                    Vec2 tip = c.pos - fromAngle(c.angle) * (r * 2.2f);
                    lo[0] = std::min(lo[0], tip.x), lo[1] = std::min(lo[1], tip.y);
                    hi[0] = std::max(hi[0], tip.x), hi[1] = std::max(hi[1], tip.y);
                }
            }
            float span = std::max({hi[0] - lo[0], hi[1] - lo[1], 30.0f});
            float cx = (lo[0] + hi[0]) * 0.5f, cy = (lo[1] + hi[1]) * 0.5f;
            for (const LayoutCell& c : st.cells) {
                const Mode& m = g.modes[c.mode];
                Thumb t;
                t.x = 0.5f + (c.pos.x - cx) / span;
                t.y = 0.5f - (c.pos.y - cy) / span;
                t.r = radiusForMass(c.mass, CellType::Phagocyte) / span;
                t.col = colU32(m.color);
                t.type = (int)m.type;
                t.angle = c.angle;
                out->thumb.push_back(t);
            }
        },
        [this, out, key, gen] {
            if (gen != gbGeneration_) return;  // the list was rebuilt meanwhile
            for (GenomeEntry& x : gbEntries_)
                if (x.path + "\n" + x.name == key) {
                    x.thumb = std::move(out->thumb);
                    x.cells = out->cells;
                    break;
                }
        });
}

namespace {
constexpr uint32_t kThumbAtlas = 2048;  // px; tiles of the card's size (36 of them at the largest)
}

bool App::thumbTile(const std::string& key, const std::vector<Thumb>& cells, float px, ImTextureID& tex, ImVec2& uv0, ImVec2& uv1) {
    if (cells.empty()) return false;
    px = std::min(px, 320.0f);  // bigger cards stretch the tile a little
    const int tile = std::clamp(((int)std::ceil(px) + 15) / 16 * 16, 64, 320);
    if (tile != thumbTilePx_ || std::fabs(px - thumbDrawPx_) > 0.5f) {  // the cards changed size: every tile is redrawn
        thumbTilePx_ = tile;
        thumbDrawPx_ = px;
        thumbShown_.clear();
        thumbReq_.clear();
    }
    const int cols = (int)kThumbAtlas / tile;
    const int i = (int)thumbReq_.size();
    if (i >= cols * cols) return false;
    ThumbTile t;
    t.key = key;
    if (i >= (int)thumbShown_.size() || thumbShown_[i].key != key) t.cells = cells;
    thumbReq_.push_back(std::move(t));
    tex = (ImTextureID)renderer_.thumbs(kThumbAtlas, kThumbAtlas).texture;
    const float s = (float)tile / kThumbAtlas;
    uv0 = {(float)(i % cols) * s, (float)(i / cols) * s};
    uv1 = {uv0.x + s, uv0.y + s};
    return true;
}

// Called from renderFrame before the main pass: redraws the atlas when the tiles asked for this frame differ from
// the ones it shows.
void App::renderThumbAtlas() {
    std::vector<ThumbTile> req;
    req.swap(thumbReq_);
    if (req.empty()) return;
    bool same = req.size() == thumbShown_.size();
    for (size_t i = 0; same && i < req.size(); ++i) same = req[i].key == thumbShown_[i].key;
    if (same) return;
    for (size_t i = 0; i < req.size(); ++i)
        if (req[i].cells.empty() && i < thumbShown_.size()) req[i].cells = std::move(thumbShown_[i].cells);
    thumbShown_ = std::move(req);

    // World units are atlas pixels, y up: tile (col, row) covers x in [col, col+1) * tile, y in (-(row+1), -row] * tile.
    const float tile = (float)thumbTilePx_, shown = thumbDrawPx_;
    const int cols = (int)kThumbAtlas / thumbTilePx_;
    RenderLists lists;
    uint32_t round = 0;  // the outline spokes of an undisturbed cell: radius 1
    for (int b = 0; b < 4; ++b) round |= (uint32_t)((1.0f - 0.4f) * 255.0f + 0.5f) << (b * 8);
    for (size_t i = 0; i < thumbShown_.size(); ++i) {
        const float ox = (float)(i % cols) * tile, oy = (float)(i / cols) * tile;
        uint32_t seed = 0x9E3779B9u * (uint32_t)(i + 1);
        for (const Thumb& t : thumbShown_[i].cells) {
            CellInstance ci{};
            ci.x = ox + (0.07f + t.x * 0.86f) * shown;
            ci.y = -(oy + (0.07f + t.y * 0.86f) * shown);
            ci.radius = t.r * 0.86f * shown;
            ci.angle = t.angle;
            ci.color = t.col;
            seed = seed * 1664525u + 1013904223u;
            ci.typeFlags = (uint32_t)t.type | (seed & 0xFFFF0000u);
            ci.p0 = t.type == (int)CellType::Lipocyte ? 0.5f : 0.0f;  // a half-full droplet
            ci.p1 = t.type == (int)CellType::Chromocyte ? 0.0f : 0.5f;
            for (uint32_t& w : ci.shape) w = round;
            lists.cells.push_back(ci);
        }
    }
    SceneParams sc;
    sc.camX = kThumbAtlas * 0.5f;
    sc.camY = -(float)kThumbAtlas * 0.5f;
    sc.zoom = 1.0f;
    sc.dishRadius = 1e9f;  // no glass to clip at
    sc.theme = (float)cfg_.theme;
    sc.gridAlpha = 0.0f;
    sc.dish = false;
    const ImVec4 bg = ImGui::ColorConvertU32ToFloat4(thumbBackground());
    const float clear[4] = {bg.x, bg.y, bg.z, 1.0f};
    renderer_.thumbs(kThumbAtlas, kThumbAtlas);
    renderer_.beginThumbsPass(clear);
    renderer_.draw(lists, sc, 2, {kThumbAtlas, kThumbAtlas});
    renderer_.endPreviewPass();
}

void App::openExperimentBrowser() {
    expBrowser_ = true;
    ebFocus_ = true;
    ebSel_ = -1;
    genomeBrowser_ = false;
}

// Picture of an experiment: set it up in a scratch world (no steps) and keep cells, walls and some food, normalised to
// the dish.
void App::buildExperimentThumb(int i) {
    ebCards_[i].ready = true;  // ordered: filled in by the worker thread
    auto out = std::make_shared<ExpCard>();
    runInBackground(
        [out, i] {
            static JobPool pool(1);  // the worker's own (the dish's pool belongs to the simulation thread)
            World tw(&pool);
            Rng rng(7);
            experiments()[i].setup(tw, rng);
            const float D = 2.0f * std::max(tw.settings.dishRadius, 1.0f);
            for (const Cell& c : tw.cells()) {
                if (!c.alive) continue;
                Thumb t;
                t.x = 0.5f + c.pos.x / D;
                t.y = 0.5f - c.pos.y / D;
                t.r = c.radius / D;
                t.col = colU32(tw.cellColor(c));
                t.type = (int)c.type;
                t.angle = c.angle;
                out->dots.push_back(t);
                ++out->cells;
            }
            for (const World::Barrier& b : tw.barriers())
                out->walls.push_back({0.5f + b.a.x / D, 0.5f - b.a.y / D, 0.5f + b.b.x / D, 0.5f - b.b.y / D});
            const auto& food = tw.food();
            const size_t step = std::max<size_t>(1, food.size() / 700);
            for (size_t k = 0; k < food.size(); k += step) out->food.push_back({0.5f + food[k].pos.x / D, 0.5f - food[k].pos.y / D});
        },
        [this, out, i] {
            if (i < (int)ebCards_.size()) {
                out->ready = true;
                ebCards_[i] = std::move(*out);
            }
        });
}

// The same cards as the genome browser: picture, name, what is in it; the selected card opens the dish.
void App::drawExperimentBrowser() {
    if (!expBrowser_) return;
    const auto& exps = experiments();
    if (ebCards_.size() != exps.size()) ebCards_.assign(exps.size(), ExpCard{});
    ImGuiIO& io = ImGui::GetIO();
    const float u = uiScale_;
    ImVec2 pos, size;
    if (mobile_) {
        pos = {safe_.x, navBottom()};
        size = {io.DisplaySize.x - safe_.x - safe_.z, io.DisplaySize.y - navBottom()};
    } else {
        size = {std::min(1100 * u, io.DisplaySize.x * 0.86f), io.DisplaySize.y * 0.8f};
        pos = {(io.DisplaySize.x - size.x) * 0.5f, (io.DisplaySize.y - size.y) * 0.55f};
    }
    ImGui::SetNextWindowPos(pos, mobile_ ? ImGuiCond_Always : ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(size, mobile_ ? ImGuiCond_Always : ImGuiCond_Appearing);
    if (ebFocus_) ImGui::SetNextWindowFocus();
    ebFocus_ = false;
    ImGui::SetNextWindowBgAlpha(1.0f);
    ImGuiWindowFlags fl = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar;
    if (mobile_) fl |= ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove;
    bool open = true;
    if (mobile_) ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::Begin("Эксперименты###ebrowser", mobile_ ? nullptr : &open, fl);
    if (mobile_) ImGui::PopStyleVar();

    ImGui::PushFont(fontBold_);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Новая чашка");
    ImGui::PopFont();
    {
        const ImGuiStyle& sty = ImGui::GetStyle();
        const char* b2 = "Закрыть";
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 8 * u,
                                 ImGui::GetWindowWidth() - sty.WindowPadding.x - ImGui::CalcTextSize(b2).x - 2 * sty.FramePadding.x));
        if (ImGui::Button(b2)) expBrowser_ = false;
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##ebfilter", "поиск", ebFilter_, sizeof(ebFilter_));
    const std::string filter = lowerUtf8(ebFilter_);
    ImGui::Separator();

    ImGui::BeginChild("##ebgrid", {0, 0});
    const float gap = 10 * u;
    const float avail = ImGui::GetContentRegionAvail().x;
    const int cols = std::max(1, (int)((avail + gap) / (330 * u + gap)));
    const float cardW = (avail - (cols - 1) * gap) / cols, cardH = 132 * u;
    int shown = 0, built = 0, openIdx = -1;
    for (int i = 0; i < (int)exps.size(); ++i) {
        if (!filter.empty() && lowerUtf8(std::string(exps[i].name) + " " + exps[i].description).find(filter) == std::string::npos)
            continue;
        ExpCard& card = ebCards_[i];
        if (shown % cols) ImGui::SameLine(0, gap);
        ++shown;
        ImGui::PushID(i);
        ImVec2 p = ImGui::GetCursorScreenPos();
        const bool sel = ebSel_ == i;
        ImGui::SetNextItemAllowOverlap();
        if (ImGui::InvisibleButton("card", {cardW, cardH})) {
            if (sel && mobile_) openIdx = i;  // a second tap on the selected card opens it
            ebSel_ = i;
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) openIdx = i;
        const bool visible = ImGui::IsItemVisible();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, {p.x + cardW, p.y + cardH}, sel ? IM_COL32(255, 158, 51, 40) : IM_COL32(255, 255, 255, 12), 10 * u);
        if (sel) dl->AddRect(p, {p.x + cardW, p.y + cardH}, IM_COL32(255, 158, 51, 170), 10 * u, 0, 1.5f);
        // The dish (built lazily, one per frame, only for cards on screen).
        const float pad = 8 * u, ts = cardH - 2 * pad;
        ImVec2 tp{p.x + pad, p.y + pad};
        if (!card.ready && visible && built < 4) {
            buildExperimentThumb(i);
            ++built;
        }
        const ImVec2 dc{tp.x + ts * 0.5f, tp.y + ts * 0.5f};
        const bool light = cfg_.theme != 0;
        dl->AddCircleFilled(dc, ts * 0.5f, light ? IM_COL32(214, 212, 244, 255) : IM_COL32(30, 32, 38, 255), 48);
        dl->AddCircle(dc, ts * 0.5f, light ? IM_COL32(60, 60, 90, 200) : IM_COL32(150, 155, 165, 120), 48, 1.2f);
        for (const ImVec2& f : card.food)
            dl->AddRectFilled({tp.x + f.x * ts - 0.5f, tp.y + f.y * ts - 0.5f}, {tp.x + f.x * ts + 0.5f, tp.y + f.y * ts + 0.5f},
                              light ? IM_COL32(110, 90, 80, 150) : IM_COL32(150, 130, 110, 120));
        for (const ImVec4& wl : card.walls)
            dl->AddLine({tp.x + wl.x * ts, tp.y + wl.y * ts}, {tp.x + wl.z * ts, tp.y + wl.w * ts},
                        light ? IM_COL32(70, 70, 100, 255) : IM_COL32(170, 175, 190, 230), 1.5f);
        for (const Thumb& t : card.dots)
            dl->AddCircleFilled({tp.x + t.x * ts, tp.y + t.y * ts}, std::max(1.6f * u, t.r * ts), t.col);
        if (!card.ready) {
            const char* wait = "…";
            ImVec2 ws = ImGui::CalcTextSize(wait);
            dl->AddText({dc.x - ws.x * 0.5f, dc.y - ws.y * 0.5f}, IM_COL32(200, 200, 210, 160), wait);
        }
        // Text.
        const float tx = tp.x + ts + pad, tw = p.x + cardW - pad - tx;
        dl->PushClipRect({tx, p.y}, {p.x + cardW - pad, p.y + cardH}, true);
        dl->AddText(fontBold_, fontBold_->FontSize, {tx, p.y + pad}, IM_COL32(240, 242, 245, 255), exps[i].name);
        float y = p.y + pad + fontBold_->FontSize + 2 * u;
        if (card.ready) {
            char sub[64];
            std::snprintf(sub, sizeof(sub), "%d клеток", card.cells);
            dl->AddText(fontSmall_, fontSmall_->FontSize, {tx, y}, kAccentU32, sub);
        }
        y += fontSmall_->FontSize + 3 * u;
        const float textBottom = sel ? p.y + cardH - ImGui::GetFrameHeight() - pad - 2 * u : p.y + cardH - pad;
        dl->PushClipRect({tx, y}, {p.x + cardW - pad, textBottom}, true);
        dl->AddText(fontSmall_, fontSmall_->FontSize, {tx, y}, IM_COL32(170, 175, 182, 255), exps[i].description, nullptr, tw);
        dl->PopClipRect();
        dl->PopClipRect();
        if (sel) {
            ImVec2 back = ImGui::GetCursorScreenPos();
            ImGui::SetCursorScreenPos({tx, p.y + cardH - ImGui::GetFrameHeight() - pad});
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 0.62f, 0.20f, 0.6f));
            if (ImGui::Button("Открыть")) openIdx = i;
            ImGui::PopStyleColor();
            ImGui::SetCursorScreenPos(back);
            ImGui::Dummy({0, 0});
        }
        ImGui::PopID();
    }
    if (shown == 0) ImGui::TextDisabled("Ничего не найдено.");
    ImGui::EndChild();
    ImGui::End();
    if (!open) expBrowser_ = false;
    if (openIdx >= 0) {
        loadExperiment(openIdx);
        ui_.screen = Screen::Microscope;
        expBrowser_ = false;
        toast(std::string("Эксперимент: ") + exps[openIdx].name);
    }
}

void App::drawGenomeBrowser() {
    if (!genomeBrowser_) return;
    if (gbDirty_) rebuildGenomeList();
    ImGuiIO& io = ImGui::GetIO();
    const float u = uiScale_;
    ImVec2 pos, size;
    if (mobile_) {
        pos = {safe_.x, navBottom()};
        size = {io.DisplaySize.x - safe_.x - safe_.z, io.DisplaySize.y - navBottom()};
    } else {
        size = {std::min(1100 * u, io.DisplaySize.x * 0.86f), io.DisplaySize.y * 0.8f};
        pos = {(io.DisplaySize.x - size.x) * 0.5f, (io.DisplaySize.y - size.y) * 0.55f};
    }
    ImGui::SetNextWindowPos(pos, mobile_ ? ImGuiCond_Always : ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(size, mobile_ ? ImGuiCond_Always : ImGuiCond_Appearing);
    if (gbFocus_) ImGui::SetNextWindowFocus();  // once, when opened (every frame would fight its own popups)
    gbFocus_ = false;
    ImGui::SetNextWindowBgAlpha(1.0f);  // opaque: nothing of the editor shows through the cards
    ImGuiWindowFlags fl = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar;
    if (mobile_) fl |= ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove;
    bool open = true;
    if (mobile_) ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::Begin("Геномы###gbrowser", mobile_ ? nullptr : &open, fl);
    if (mobile_) ImGui::PopStyleVar();

    // Header: title, open file, close.
    ImGui::PushFont(fontBold_);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Выбор генома");
    ImGui::PopFont();
    {
        const ImGuiStyle& sty = ImGui::GetStyle();
        const char* b1 = "Открыть файл…";
        const char* b2 = "Закрыть";
        float w = ImGui::CalcTextSize(b1).x + ImGui::CalcTextSize(b2).x + 4 * sty.FramePadding.x + sty.ItemSpacing.x;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 8 * u, ImGui::GetWindowWidth() - sty.WindowPadding.x - w));
        if (ImGui::Button(b1)) {
            std::string p = pickFile(L"Геномы (*.genome, *.genome.txt)\0*.genome;*.genome.txt\0Все файлы\0*.*\0", L"Открыть геном");
            if (!p.empty() && importPath(p)) genomeBrowser_ = false;
            else if (mobile_) genomeBrowser_ = false;  // the picker answers later through importPath
        }
        ImGui::SameLine();
        if (ImGui::Button(b2)) genomeBrowser_ = false;
    }
    // Source tabs and search.
    const char* tabs[] = {"Встроенные", "Мои", "Cell Lab"};
    for (int t = 0; t < 3; ++t) {
        if (t) ImGui::SameLine();
        if (toggle(tabs[t], gbTab_ == t)) {
            gbTab_ = t;
            gbSel_ = -1;
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##gbfilter", "поиск", gbFilter_, sizeof(gbFilter_));
    const std::string filter = lowerUtf8(gbFilter_);
    ImGui::Separator();

    ImGui::BeginChild("##gbgrid", {0, 0});
    const float gap = 10 * u;
    const float avail = ImGui::GetContentRegionAvail().x;
    const int cols = std::max(1, (int)((avail + gap) / (330 * u + gap)));
    const float cardW = (avail - (cols - 1) * gap) / cols, cardH = 132 * u;
    int shown = 0, thumbsBuilt = 0;
    std::string pendingDelete;
    int openIdx = -1;
    for (int i = 0; i < (int)gbEntries_.size(); ++i) {
        GenomeEntry& e = gbEntries_[i];
        if (e.source != gbTab_) continue;
        if (!filter.empty() && lowerUtf8(e.name + " " + e.description).find(filter) == std::string::npos) continue;
        if (shown % cols) ImGui::SameLine(0, gap);
        ++shown;
        ImGui::PushID(i);
        ImVec2 p = ImGui::GetCursorScreenPos();
        const bool sel = gbSel_ == i;
        ImGui::SetNextItemAllowOverlap();
        if (ImGui::InvisibleButton("card", {cardW, cardH})) gbSel_ = i;
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) openIdx = i;
        const bool visible = ImGui::IsItemVisible();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, {p.x + cardW, p.y + cardH}, sel ? IM_COL32(255, 158, 51, 40) : IM_COL32(255, 255, 255, 12), 10 * u);
        if (sel) dl->AddRect(p, {p.x + cardW, p.y + cardH}, IM_COL32(255, 158, 51, 170), 10 * u, 0, 1.5f);
        // Thumbnail (built lazily: a couple per frame, only for cards on screen).
        const float pad = 8 * u, ts = cardH - 2 * pad;
        ImVec2 tp{p.x + pad, p.y + pad};
        dl->AddRectFilled(tp, {tp.x + ts, tp.y + ts}, thumbBackground(), 8 * u);
        if (!e.previewReady && visible && thumbsBuilt < 6) {
            buildGenomeThumb(e);
            ++thumbsBuilt;
        }
        ImTextureID tex{};
        ImVec2 uv0, uv1;
        char key[32];
        std::snprintf(key, sizeof key, "\n%zu\n%d", e.thumb.size(), cfg_.theme);
        if (visible && thumbTile(e.path + "\n" + e.name + key, e.thumb, ts, tex, uv0, uv1)) {
            // The tile is the card's size (rounded up to 16 px): crop it to the square shown.
            const float k = thumbDrawPx_ / (float)thumbTilePx_;
            ImVec2 uvEnd{uv0.x + (uv1.x - uv0.x) * k, uv0.y + (uv1.y - uv0.y) * k};
            dl->AddImageRounded(tex, tp, {tp.x + ts, tp.y + ts}, uv0, uvEnd, IM_COL32_WHITE, 8 * u);
        } else {  // no room in the atlas: vector icons
            dl->PushClipRect(tp, {tp.x + ts, tp.y + ts}, true);
            const float inner = ts * 0.86f, ox = tp.x + ts * 0.07f, oy = tp.y + ts * 0.07f;
            for (const Thumb& t : e.thumb) {
                ImVec2 c{ox + t.x * inner, oy + t.y * inner};
                float r = std::max(1.0f, t.r * inner);
                if (r >= 7.0f) drawCellIcon(dl, c, r, t.type, t.col, 1.0f, t.angle);
                else {
                    dl->AddCircleFilled(c, r, t.col);
                    if (r >= 2.5f) dl->AddCircle(c, r, IM_COL32(0, 0, 0, 70), 0, 1.0f);
                }
            }
            dl->PopClipRect();
        }
        // Text.
        const float tx = tp.x + ts + pad, tw = p.x + cardW - pad - tx;
        dl->PushClipRect({tx, p.y}, {p.x + cardW - pad, p.y + cardH}, true);
        dl->AddText(fontBold_, fontBold_->FontSize, {tx, p.y + pad}, IM_COL32(240, 242, 245, 255), e.name.c_str());
        char sub[64];
        const char* src = e.source == 0 ? "встроенный" : e.source == 1 ? "мой" : "Cell Lab";
        if (e.previewReady) std::snprintf(sub, sizeof(sub), "%s · %d клеток", src, e.cells);
        else std::snprintf(sub, sizeof(sub), "%s", src);
        float y = p.y + pad + fontBold_->FontSize + 2 * u;
        dl->AddText(fontSmall_, fontSmall_->FontSize, {tx, y}, kAccentU32, sub);
        y += fontSmall_->FontSize + 3 * u;
        const float textBottom = sel ? p.y + cardH - ImGui::GetFrameHeight() - pad - 2 * u : p.y + cardH - pad;
        dl->PushClipRect({tx, y}, {p.x + cardW - pad, textBottom}, true);
        dl->AddText(fontSmall_, fontSmall_->FontSize, {tx, y}, IM_COL32(170, 175, 182, 255), e.description.c_str(), nullptr, tw);
        dl->PopClipRect();
        dl->PopClipRect();
        // Actions of the selected card.
        if (sel) {
            ImVec2 back = ImGui::GetCursorScreenPos();
            ImGui::SetCursorScreenPos({tx, p.y + cardH - ImGui::GetFrameHeight() - pad});
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 0.62f, 0.20f, 0.6f));
            if (ImGui::Button("Открыть")) openIdx = i;
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (dropButton("Поделиться")) ImGui::OpenPopup("gbshare");
            if (mobile_) ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {10 * u, 18 * u});  // finger-sized rows
            if (ImGui::BeginPopup("gbshare")) {
                Genome g = e.genome;
                if (g.name.empty()) g.name = e.name;
                genomeShareItems(g);
                ImGui::EndPopup();
            }
            if (mobile_) ImGui::PopStyleVar();
            if (e.source != 0) {
                ImGui::SameLine();
                if (ImGui::Button("Удалить")) ImGui::OpenPopup("gbdel");
            }
            if (ImGui::BeginPopup("gbdel")) {
                ImGui::Text("Удалить файл «%s»?", e.name.c_str());
                if (ImGui::Button("Удалить", {160 * u, 0})) {
                    pendingDelete = e.path;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            ImGui::SetCursorScreenPos(back);
            ImGui::Dummy({0, 0});
        }
        ImGui::PopID();
    }
    if (shown == 0) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled(gbTab_ == 1   ? "Здесь появятся геномы, которые вы сохраните («Сохранить» в редакторе) или откроете из файла."
                            : gbTab_ == 2 ? "Положите файлы .genome из Cell Lab в папку genomes или import — или нажмите «Открыть файл…»."
                                          : "Ничего не найдено.");
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
    ImGui::End();
    if (!open) genomeBrowser_ = false;

    if (openIdx >= 0) {
        loadGenomeIntoEditor(gbEntries_[openIdx].genome);
        ui_.screen = gbAfterOpen_;
        genomeBrowser_ = false;
        toast((gbAfterOpen_ == Screen::Microscope ? "Синтезатор вставляет: " : "Геном: ") + gbEntries_[openIdx].name);
    }
    if (!pendingDelete.empty()) {
        std::error_code ec;
        std::filesystem::remove(std::filesystem::path((const char8_t*)pendingDelete.c_str()), ec);
        toast(ec ? "Не удалось удалить файл" : "Файл удалён");
        gbDirty_ = true;
        gbSel_ = -1;
    }
}
