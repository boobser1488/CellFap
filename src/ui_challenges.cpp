// Levels ("ЗАДАНИЯ"): the list of challenges, the goal panel in the microscope and the win / lose window.
#include "app.hpp"
#include "ui_widgets.hpp"

using namespace uiw;

namespace {
const ImGuiWindowFlags kFixed = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus;

const char* toolName(int t) {
    static const char* n[] = {"диагностика", "синтезатор", "пинцет", "бустер", "удаление", "электрод",
                              "генная игла", "игла веществ", "карта", "адгезин", "фонарик"};
    return n[clampv(t, 0, 10)];
}
}  // namespace

bool App::toolAllowed(Tool t) const {
    if (ui_.challenge < 0) return true;
    return (challenges()[ui_.challenge].tools >> (int)t) & 1u;
}

void App::startChallenge(int index) {
    const auto& list = challenges();
    if (index < 0 || index >= (int)list.size()) return;
    ui_.challenge = index;
    ui_.chState = ChallengeState{};
    ui_.chProgress = ChallengeProgress{};
    ui_.chFinished = false;
    ui_.chShowHint = false;
    Rng rng((uint64_t)(time_ * 1000.0) | 1);
    list[index].setup(*world_, rng, ui_.chState);
    ui_.chState.insertsLeft = list[index].inserts;
    track(-1);
    fitCamera(true);
    if (!toolAllowed(ui_.tool)) ui_.tool = Tool::Inspect;
    if (toolAllowed(Tool::Synth)) ui_.tool = Tool::Synth;
    ui_.speed = Speed::Observe;
    ui_.screen = Screen::Microscope;
    toast(std::string("Задание: ") + list[index].name);
}

void App::updateChallenge() {
    if (ui_.challenge < 0 || ui_.chFinished) return;
    ui_.chProgress = challenges()[ui_.challenge].check(*world_, ui_.chState);
    if (ui_.chProgress.won) {
        ui_.chFinished = true;
        cfg_.challengesDone |= 1 << ui_.challenge;
        markConfigDirty();
        ImGui::OpenPopup("Задание выполнено###chresult");
    } else if (ui_.chProgress.lost) {
        ui_.chFinished = true;
        ImGui::OpenPopup("Задание провалено###chresult");
    }
}

void App::drawChallengesScreen() {
    ImGuiIO& io = ImGui::GetIO();
    float top = navBottom();
    ImGui::GetBackgroundDrawList()->AddRectFilled({0, top}, io.DisplaySize, IM_COL32(16, 17, 19, mobile_ ? 240 : 170));
    float maxW = std::min(io.DisplaySize.x - (mobile_ ? 12 * uiScale_ + safe_.x + safe_.z : 40 * uiScale_), 1100.0f * uiScale_);
    ImGui::SetNextWindowPos({(io.DisplaySize.x - maxW + (mobile_ ? safe_.x - safe_.z : 0.0f)) * 0.5f, top + (mobile_ ? 6 : 16) * uiScale_});
    ImGui::SetNextWindowSize({maxW, io.DisplaySize.y - top - (mobile_ ? 12 : 32) * uiScale_});
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("##challenges", nullptr, kFixed);
    const auto& list = challenges();
    int done = 0;
    for (int i = 0; i < (int)list.size(); ++i) done += (cfg_.challengesDone >> i) & 1;

    ImGui::PushFont(fontBold_);
    ImGui::TextUnformatted("Задания");
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("  выполнено %d из %zu · следующее открывается, когда пройдено предыдущее", done, list.size());
    ImGui::PopTextWrapPos();
    ImGui::Dummy({0, 6 * uiScale_});

    // Left: the list of level cards. Right: the briefing of the selected one. A narrow screen stacks them.
    const bool stack = ImGui::GetContentRegionAvail().x < 680 * uiScale_;
    float listW = 330 * uiScale_;
    ImGui::BeginChild("chlist", {stack ? -FLT_MIN : listW, stack ? ImGui::GetContentRegionAvail().y * 0.45f : 0}, ImGuiChildFlags_None);
    for (int i = 0; i < (int)list.size(); ++i) {
        bool completed = (cfg_.challengesDone >> i) & 1;
        bool open = i < 3 || ((cfg_.challengesDone >> (i - 1)) & 1) || completed;
        ImGui::PushID(i);
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x, h = 52 * uiScale_;
        bool sel = ui_.chSelected == i;
        if (ImGui::InvisibleButton("card", {w, h})) ui_.chSelected = i;
        bool hov = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, {p.x + w, p.y + h}, sel ? IM_COL32(255, 158, 51, 45) : (hov ? IM_COL32(255, 255, 255, 18) : IM_COL32(30, 31, 34, 235)),
                          8 * uiScale_);
        if (sel) dl->AddRect(p, {p.x + w, p.y + h}, IM_COL32(255, 158, 51, 160), 8 * uiScale_);
        // number badge
        ImVec2 c{p.x + 24 * uiScale_, p.y + h * 0.5f};
        dl->AddCircleFilled(c, 15 * uiScale_, completed ? kGreenU32 : (open ? kAccentU32 : IM_COL32(90, 92, 98, 255)));
        char num[8];
        std::snprintf(num, sizeof(num), "%d", i + 1);
        ImVec2 ns = ImGui::CalcTextSize(num);
        dl->AddText({c.x - ns.x * 0.5f, c.y - ns.y * 0.5f}, IM_COL32(25, 22, 18, 255), num);
        dl->AddText(fontBold_, ImGui::GetFontSize(), {p.x + 48 * uiScale_, p.y + 8 * uiScale_},
                    open ? IM_COL32(235, 240, 245, 255) : IM_COL32(130, 134, 140, 255), list[i].name);
        // As in the original's list: a completed level says so in green; a long goal ends with "…" at the card's edge.
        const char* sub = completed ? "Задание выполнено." : (open ? list[i].goal : "закрыто — пройдите предыдущее");
        ImGui::PushFont(fontSmall_);
        ImGui::PushStyleColor(ImGuiCol_Text, completed ? ImVec4(0.43f, 0.80f, 0.40f, 1) : ImVec4(0.63f, 0.65f, 0.67f, 1));
        const ImVec2 a{p.x + 48 * uiScale_, p.y + 30 * uiScale_}, b{p.x + w - 10 * uiScale_, p.y + h - 2 * uiScale_};
        ImGui::RenderTextEllipsis(dl, a, b, b.x, b.x, sub, nullptr, nullptr);
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::PopID();
        ImGui::Dummy({0, 4 * uiScale_});
    }
    ImGui::EndChild();
    if (!stack) ImGui::SameLine(0, 18 * uiScale_);

    ImGui::BeginChild("chinfo", {0, 0}, ImGuiChildFlags_None);
    int i = clampv(ui_.chSelected, 0, (int)list.size() - 1);
    const Challenge& ch = list[i];
    bool completed = (cfg_.challengesDone >> i) & 1;
    bool open = i < 3 || ((cfg_.challengesDone >> (i - 1)) & 1) || completed;
    ImGui::PushFont(fontBold_);
    ImGui::Text("%d. %s", i + 1, ch.name);
    ImGui::PopFont();
    ImGui::Dummy({0, 4 * uiScale_});
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(ch.story);
    ImGui::Dummy({0, 8 * uiScale_});
    sectionHeader("Цель");
    ImGui::TextUnformatted(ch.goal);
    sectionHeader("Разрешённые инструменты");
    std::string tools;
    for (int t = 0; t < (int)Tool::Count; ++t)
        if ((ch.tools >> t) & 1) tools += (tools.empty() ? "" : ", ") + std::string(toolName(t));
    ImGui::TextDisabled("%s. Редактор генома доступен всегда.", tools.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Dummy({0, 12 * uiScale_});
    if (!open) {
        ImGui::BeginDisabled();
        ImGui::Button("Закрыто", {200 * uiScale_, 36 * uiScale_});
        ImGui::EndDisabled();
    } else if (ImGui::Button(ui_.challenge == i ? "Начать заново" : (completed ? "Пройти ещё раз" : "Начать"), {200 * uiScale_, 36 * uiScale_})) {
        startChallenge(i);
    }
    if (ui_.challenge >= 0) {
        ImGui::SameLine();
        if (ImGui::Button("Свободная игра", {200 * uiScale_, 36 * uiScale_})) {
            ui_.challenge = -1;
            toast("Задание отложено. Чашка доступна для свободной игры.");
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

// Goal panel in the microscope plus the result window.
void App::drawChallengePanel() {
    if (ui_.challenge < 0) return;
    updateChallenge();  // (opens the result popup at the top level of the ID stack)
    const Challenge& ch = challenges()[ui_.challenge];
    ImGuiIO& io = ImGui::GetIO();
    if (!mobile_) {  // on the phone the goal lives in a tab of the dock (ui_mobile.cpp)
        bool panelOpen = true;
        if (beginPanel("Задание###chpanel2", &panelOpen, {io.DisplaySize.x - 12 * uiScale_, io.DisplaySize.y - 12 * uiScale_},
                       {460 * uiScale_, 0}, {1.0f, 1.0f}, ImGuiWindowFlags_AlwaysAutoResize))
            drawChallengeBody(440 * uiScale_);
        ImGui::End();
        if (!panelOpen) {
            ui_.challenge = -1;
            toast("Задание закрыто. Чашка доступна для свободной игры.");
            return;
        }
    }

    // Result window.
    const float rw = std::min(460 * uiScale_, io.DisplaySize.x - 24 * uiScale_);
    ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.45f}, ImGuiCond_Appearing, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({rw, 0});
    if (ImGui::BeginPopupModal("Задание выполнено###chresult", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        bool won = ui_.chProgress.won;
        ImGui::PushFont(fontBold_);
        ImGui::TextColored(won ? ImVec4(0.55f, 0.85f, 0.55f, 1) : ImVec4(1.0f, 0.5f, 0.45f, 1), won ? "Задание выполнено!" : "Задание не выполнено");
        ImGui::PopFont();
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(won ? ch.win : (ch.lose[0] ? ch.lose : "Условие задания не выполнено. Попробуйте ещё раз."));
        ImGui::PopTextWrapPos();
        ImGui::Dummy({0, 8 * uiScale_});
        int next = ui_.challenge + 1;
        if (won && next < (int)challenges().size()) {
            if (ImGui::Button("Следующее задание", {0, 0})) {
                ImGui::CloseCurrentPopup();
                startChallenge(next);
            }
            ImGui::SameLine();
        }
        if (!won && ImGui::Button("Ещё раз", {0, 0})) {
            ImGui::CloseCurrentPopup();
            startChallenge(ui_.challenge);
        } else if (!won) {
            ImGui::SameLine();
        }
        if (ImGui::Button("Свободная игра", {0, 0})) {
            ui_.challenge = -1;  // the level is over: its panel goes away, the dish stays
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void App::drawChallengeBody(float w) {
    const Challenge& ch = challenges()[ui_.challenge];
    ImGui::PushFont(fontBold_);
    ImGui::Text("%d. %s", ui_.challenge + 1, ch.name);
    ImGui::PopFont();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    ImGui::TextUnformatted(ch.goal);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = 8 * uiScale_;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, {p.x + w, p.y + h}, IM_COL32(255, 255, 255, 20), 4);
    dl->AddRectFilled(p, {p.x + w * clampv(ui_.chProgress.progress, 0.0f, 1.0f), p.y + h}, ui_.chProgress.won ? kGreenU32 : kAccentU32, 4);
    ImGui::Dummy({w, h + 2 * uiScale_});
    ImGui::PushFont(fontSmall_);
    ImGui::TextDisabled("%s", ui_.chProgress.status.c_str());
    if (ui_.chState.insertsLeft >= 0) ImGui::TextDisabled("Вставок синтезатором осталось: %d", ui_.chState.insertsLeft);
    if (ui_.chShowHint) ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.5f, 1.0f), "Подсказка: %s", ch.hint);
    ImGui::PopFont();
    ImGui::PopTextWrapPos();
    auto small = [&](const char* label) { return mobile_ ? ImGui::Button(label) : ImGui::SmallButton(label); };
    if (small(ui_.chShowHint ? "Скрыть подсказку" : "Подсказка")) ui_.chShowHint = !ui_.chShowHint;
    ImGui::SameLine();
    if (small("Заново")) startChallenge(ui_.challenge);
    ImGui::SameLine();
    if (small("Все задания")) ui_.screen = Screen::Challenges;
    if (mobile_ && ImGui::Button("Закрыть задание")) {
        ui_.challenge = -1;
        toast("Задание закрыто. Чашка доступна для свободной игры.");
    }
}
