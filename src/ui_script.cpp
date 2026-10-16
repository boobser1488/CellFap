// The editor of a program cell's code (dypt), in the mode panel of the genome editor.
//
// The text is edited in a buffer of its own and goes into the genome only through «Применить», after it has compiled
// and run: the cells in the dish never pick up half-typed code (every keystroke used to recompile the live program).
// Errors are shown in Russian, with the line they are on: dypt names the line of a syntax error; for the errors of a
// running program the line is looked up in the source when it can be (a variable that does not exist, ...).
#include "app.hpp"
#include "sim/dypt.hpp"
#include "sim/signal_cells.hpp"
#include "ui_widgets.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------- errors

struct ScriptError {
    std::string message;  // what to tell the player (Russian)
    std::string raw;      // dypt's own words
    int line = 0;         // 1-based; 0 = unknown
    int column = 0;
};

bool startsWith(const std::string& s, const char* p) { return s.compare(0, std::strlen(p), p) == 0; }

// The line on which `word` first appears as a whole word (outside comments), 0 when it does not.
int lineOfWord(const std::string& src, const std::string& word) {
    int line = 1;
    for (size_t i = 0; i < src.size(); ++i) {
        if (src[i] == '\n') {
            ++line;
            continue;
        }
        if (src.compare(i, 2, "//") == 0) {
            while (i < src.size() && src[i] != '\n') ++i;
            --i;
            continue;
        }
        if (src.compare(i, word.size(), word) != 0) continue;
        auto isId = [](unsigned char c) { return std::isalnum(c) || c == '_' || c >= 0x80; };
        const bool before = i > 0 && isId((unsigned char)src[i - 1]);
        const bool after = i + word.size() < src.size() && isId((unsigned char)src[i + word.size()]);
        if (!before && !after) return line;
    }
    return 0;
}

// The line before `line` that is not blank (where an unfinished expression usually is).
int previousCodeLine(const std::string& src, int line) {
    std::vector<std::string> lines;
    size_t a = 0;
    for (size_t b; (b = src.find('\n', a)) != std::string::npos; a = b + 1) lines.push_back(src.substr(a, b - a));
    lines.push_back(src.substr(a));
    for (int l = line - 1; l >= 1; --l)
        if (lines[l - 1].find_first_not_of(" \t\r") != std::string::npos) return l;
    return line;
}

ScriptError describeError(std::string raw, const std::string& src) {
    ScriptError e;
    if (startsWith(raw, "Dypt error: ")) raw = raw.substr(12);
    e.raw = raw;
    std::string msg = raw;
    // "Line 3, column 5: ..."
    int line = 0, col = 0, used = 0;
    if (std::sscanf(msg.c_str(), "Line %d, column %d: %n", &line, &col, &used) >= 2 && used > 0) {
        e.line = line;
        e.column = col;
        msg = msg.substr((size_t)used);
    }
    // The usual messages, in Russian; the rest stays in dypt's words.
    struct Rule {
        const char* en;
        const char* ru;
    };
    static const Rule kRules[] = {
        {"Expected ',' or ']' in array literal", "в списке пропущена запятая или закрывающая скобка «]»"},
        {"Index out of bounds", "номер элемента за пределами списка (входы — это 19 чисел: 0..18)"},
        {"Division by zero or invalid operands", "деление на ноль или деление не чисел"},
        {"Invalid operands for +", "«+» нельзя применить к этим значениям (например, к списку и числу)"},
        {"Invalid operands for -", "«-» нельзя применить к этим значениям"},
        {"Invalid operands for *", "«*» нельзя применить к этим значениям"},
        {"Invalid operands for /", "«/» нельзя применить к этим значениям"},
    };
    for (const Rule& r : kRules)
        if (msg == r.en) msg = r.ru;
    char name[256];
    if (std::sscanf(msg.c_str(), "Variable '%255[^']' not found", name) == 1) {
        const std::string v = name;
        static const char* kKeywords[] = {"return", "if", "else", "for", "while", "function", "and", "or", "not"};
        bool keyword = false;
        for (const char* k : kKeywords) keyword |= v == k;
        if (keyword) {
            // dypt read the keyword as a value: the expression before it was left unfinished.
            msg = "незаконченное выражение перед «" + v + "» (что-то пропущено в конце строки?)";
            if (!e.line) {
                const int at = lineOfWord(src, v);
                e.line = at ? previousCodeLine(src, at) : 0;
            }
        } else {
            msg = "переменная «" + v + "» не задана (опечатка? ей не присвоено значение?)";
            if (!e.line) e.line = lineOfWord(src, v);
        }
    }
    char want[64], found[128];
    if (startsWith(msg, "expected '}', found")) {
        msg = "не хватает закрывающей скобки «}»";
    } else if (std::sscanf(msg.c_str(), "expected '%63[^']', found %127[^\n]", want, found) == 2) {
        std::string f = found;
        if (f.size() >= 2 && f.front() == '\'' && f.back() == '\'') f = f.substr(1, f.size() - 2);
        msg = std::string("ожидалось «") + want + "», а стоит «" + f + "»";
    }
    // A syntax error at the start of a line whose previous line ends in an operator: the expression went on to
    // the next line (dypt reads on), and the mistake is the dangling operator.
    if (e.line > 1 && !src.empty()) {
        std::vector<std::string> lines;
        size_t a = 0;
        for (size_t b; (b = src.find('\n', a)) != std::string::npos; a = b + 1) lines.push_back(src.substr(a, b - a));
        lines.push_back(src.substr(a));
        const int prev = previousCodeLine(src, e.line);
        if (prev >= 1 && prev < e.line && prev <= (int)lines.size()) {
            std::string t = lines[prev - 1];
            if (size_t c = t.find("//"); c != std::string::npos) t.resize(c);
            while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '\r')) t.pop_back();
            static const char* kOps[] = {"+", "-", "*", "/", "%", "=", "<", ">", " and", " or", " not"};  // (not "," or "(": lists may span lines)
            for (const char* op : kOps) {
                const size_t n = std::strlen(op);
                if (t.size() >= n && t.compare(t.size() - n, n, op) == 0) {
                    std::string o = op;
                    if (o[0] == ' ') o = o.substr(1);
                    msg = "строка " + std::to_string(prev) + " обрывается на «" + o +
                          "»: выражение продолжилось на следующей строке. (" + msg + ")";
                    e.line = prev;
                    e.column = 0;
                    break;
                }
            }
        }
    }
    e.message = msg;
    return e;
}

// ---------------------------------------------------------------- editor state

struct EditorState {
    std::string text;       // what is being edited
    std::string applied;    // what the genome had when the text was loaded or last applied
    const void* genome = nullptr;
    int mode = -1;
    bool hasError = false;
    ScriptError error;
    bool tested = false;    // the current text compiled and ran
    std::string testSummary;
    int scrollToLine = 0;   // pending: scroll the code view to this line
    bool codeDetached = false;  // the code is in a window of its own
    float codeH = 0;            // height of the code in the editor (0: not set yet)
    // presets
    int preset = 0, logic = 0;
    float threshold = 0.5f, period = 200.0f;
    // test bench
    float in[SignalCellInputs::kCount] = {};
    int steps = 100;
    std::vector<std::vector<float>> trace;
    std::string traceError;
    int traceFailed = -1;
    // what the programs printed
    std::string log;
};
EditorState g_ed;

int textCallback(ImGuiInputTextCallbackData* d) {
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* s = static_cast<std::string*>(d->UserData);
        s->resize((size_t)d->BufTextLen);
        d->Buf = s->data();
    }
    return 0;
}

int countLines(const std::string& s) { return 1 + (int)std::count(s.begin(), s.end(), '\n'); }

}  // namespace

void App::drawScriptEditor() {
    const float u = uiScale_;
    auto note = [&](const char* text) {
        ImGui::PushFont(fontSmall_);
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", text);
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
    };
    Genome& g = ui_.editGenome;
    const int modeIdx = ui_.editMode;
    SignalCellManager& mgr = world_->signalCells();

    // Another genome or mode: load its program. The same one changed elsewhere (undo, a loaded file) while there are
    // no edits of ours: follow it.
    const std::string& stored = g.scripts[modeIdx];
    const bool dirty = g_ed.text != g_ed.applied;
    if (g_ed.genome != &g || g_ed.mode != modeIdx || (!dirty && stored != g_ed.applied)) {
        g_ed.genome = &g;
        g_ed.mode = modeIdx;
        g_ed.text = g_ed.applied = stored;
        g_ed.hasError = false;
        g_ed.tested = false;
        g_ed.trace.clear();
        g_ed.traceError.clear();
    }

    if (opt_.codeWindow) {  // test captures: the code in its own window
        opt_.codeWindow = false;
        g_ed.codeDetached = true;
    }
    if (opt_.scriptCheck) {  // test captures: press «Проверить» once
        opt_.scriptCheck = false;
        scriptTest();
    }
    uiw::sectionHeader("Программа клетки (dypt)");
    if (!mgr.ready()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.45f, 1.0f));
        ImGui::TextWrapped("Язык dypt не подключён: %s. Программу можно писать и сохранять в геноме, но клетка "
                           "не будет её выполнять.",
                           mgr.lastError().empty() ? "библиотека dypt_embed не найдена" : mgr.lastError().c_str());
        ImGui::PopStyleColor();
    }

    // ---- examples
    if (ImGui::CollapsingHeader("Вставить пример")) {
        static const SignalCellPreset kPresets[] = {SignalCellPreset::LogicGate, SignalCellPreset::Switch, SignalCellPreset::Timer,
                                                    SignalCellPreset::Conditional};
        // The built-in examples, then the programs the running modifications added.
        std::vector<std::string> labels;
        for (SignalCellPreset k : kPresets) labels.push_back(signalCellPresetName(k));
        for (const ModProgram& mp : mods_.programs) labels.push_back(mp.name + "  (модификация)");
        std::vector<const char*> names;
        for (const std::string& l : labels) names.push_back(l.c_str());
        if (g_ed.preset >= (int)names.size()) g_ed.preset = 0;  // a modification was switched off meanwhile
        uiw::rowLabel("Пример");
        ImGui::Combo("##preset", &g_ed.preset, names.data(), (int)names.size());
        if (g_ed.preset >= 4) {
            const ModProgram& mp = mods_.programs[g_ed.preset - 4];
            std::string from = "Программа из модификации";
            for (const ModInfo& m : mods_.mods)
                if (m.id == mp.mod) from += " «" + m.name + "»";
            note((from + ".").c_str());
            if (ImGui::Button("Заменить код примером", {-FLT_MIN, 0})) {
                g_ed.text = mp.source;
                g_ed.hasError = false;
                g_ed.tested = false;
            }
        } else {
            const SignalCellPreset p = kPresets[g_ed.preset];
            if (p == SignalCellPreset::LogicGate) {
                static const char* kOps[] = {"И (AND)", "ИЛИ (OR)", "Исключающее ИЛИ (XOR)", "НЕ (NOT)"};
                uiw::rowLabel("Логика");
                ImGui::Combo("##logic", &g_ed.logic, kOps, 4);
            }
            if (p == SignalCellPreset::Switch || p == SignalCellPreset::Conditional)
                uiw::sliderF("Порог S1", &g_ed.threshold, 0.0f, 5.0f, "%.2f");
            if (p == SignalCellPreset::Timer) {
                uiw::sliderF("Период, тиков", &g_ed.period, 2.0f, 4000.0f, "%.0f", 0, false, ImGuiSliderFlags_Logarithmic);
                ImGui::SameLine();
                ImGui::TextDisabled("= %.2f ч", g_ed.period / 400.0f);
            }
            static const char* kWhat[] = {
                "Входы S1 и S2 (единица — выше 0.5), результат в S3.",
                "Каждый раз, когда S1 поднимается выше порога, S3 включается или выключается.",
                "Импульс на S3 каждые N тиков; счёт у каждой клетки свой.",
                "Пока S1 выше порога, S2 передаётся в S3.",
            };
            note(kWhat[g_ed.preset]);
            if (ImGui::Button("Заменить код примером", {-FLT_MIN, 0})) {
                g_ed.text = signalCellPresetApply(p, {(float)g_ed.logic, g_ed.threshold, g_ed.period});
                g_ed.hasError = false;
                g_ed.tested = false;
            }
        }
    }

    // ---- the code, with line numbers; the line of an error is marked. It sits in the editor (its height dragged
    // by the grip under it) or in a window of its own that is moved and sized like any other.
    ImFont* mono = fontMono_ ? fontMono_ : fontSmall_;
    auto codeView = [&](float height) {  // height < 0: all the room there is
        ImGui::PushFont(mono);
        const float lineH = ImGui::GetTextLineHeight();
        const ImGuiStyle& st = ImGui::GetStyle();
        const int lines = countLines(g_ed.text);
        char digits[16];
        std::snprintf(digits, sizeof(digits), "%d", std::max(99, lines));
        const float gutter = ImGui::CalcTextSize(digits).x + 12 * u;
        float longest = 0;
        for (size_t a = 0; a <= g_ed.text.size();) {
            size_t b = g_ed.text.find('\n', a);
            if (b == std::string::npos) b = g_ed.text.size();
            longest = std::max(longest, ImGui::CalcTextSize(g_ed.text.data() + a, g_ed.text.data() + b).x);
            a = b + 1;
        }
        ImGui::PopFont();
        const float viewH = height < 0 ? std::max(ImGui::GetContentRegionAvail().y, 60 * u) : height;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.07f, 0.075f, 0.09f, 1.0f));
        ImGui::BeginChild("code", {-FLT_MIN, viewH}, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::PushFont(mono);
        const ImVec2 o = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float width = std::max(ImGui::GetContentRegionAvail().x - gutter, longest + 40 * u);
        // As tall as the text, and at least the view: the frame around it scrolls, so the numbers stay beside their
        // lines, and a tap anywhere below the last line still lands in the text.
        const float textH = std::max(lineH * (lines + 2) + 2 * st.FramePadding.y, viewH - 2 * st.WindowPadding.y);
        dl->AddRectFilled(o, {o.x + gutter - 4 * u, o.y + textH}, IM_COL32(255, 255, 255, 8));
        for (int i = 0; i < lines; ++i) {
            const float y = o.y + st.FramePadding.y + i * lineH;
            const bool bad = g_ed.hasError && g_ed.error.line == i + 1;
            if (bad) dl->AddRectFilled({o.x, y}, {o.x + gutter + width, y + lineH}, IM_COL32(230, 70, 60, 60));
            char n[12];
            std::snprintf(n, sizeof(n), "%d", i + 1);
            const float w = ImGui::CalcTextSize(n).x;
            dl->AddText({o.x + gutter - 10 * u - w, y}, bad ? IM_COL32(255, 130, 110, 255) : IM_COL32(150, 155, 165, 160), n);
        }
        if (g_ed.scrollToLine > 0) {
            const float y = (g_ed.scrollToLine - 1) * lineH;  // only when the line is out of sight
            if (y < ImGui::GetScrollY() || y + 2 * lineH > ImGui::GetScrollY() + ImGui::GetWindowHeight())
                ImGui::SetScrollY(std::max(0.0f, y - 2 * lineH));
            g_ed.scrollToLine = 0;
        }
        ImGui::SetCursorScreenPos({o.x + gutter, o.y});
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        if (ImGui::InputTextMultiline("##code", g_ed.text.data(), g_ed.text.capacity() + 1, {width, textH},
                                      ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackResize |
                                          ImGuiInputTextFlags_NoHorizontalScroll,
                                      textCallback, &g_ed.text)) {
            g_ed.tested = false;  // (the error stays marked until the next check)
        }
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::EndChild();
        ImGui::PopStyleColor();
    };
    if (!g_ed.codeDetached) {
        if (ImGui::SmallButton("Код в отдельное окно")) g_ed.codeDetached = true;
        if (g_ed.codeH <= 0) {  // first time: as tall as the text, within reason
            ImGui::PushFont(mono);
            const float lineH = ImGui::GetTextLineHeight();
            ImGui::PopFont();
            g_ed.codeH = std::clamp(lineH * (countLines(g_ed.text) + 2) + 12 * u, 140 * u, ImGui::GetIO().DisplaySize.y * 0.45f);
        }
        codeView(g_ed.codeH);
        // The grip: drag it to make the code taller or shorter.
        const ImVec2 gp = ImGui::GetCursorScreenPos();
        const float gw = ImGui::GetContentRegionAvail().x, gh = 12 * u;
        ImGui::InvisibleButton("##codegrip", {gw, gh});
        uiw::keepTouchDrag();
        const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if (ImGui::IsItemActive())
            g_ed.codeH = std::clamp(g_ed.codeH + ImGui::GetIO().MouseDelta.y, 80 * u, 3000 * u);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float cx = gp.x + gw * 0.5f, cy = gp.y + gh * 0.5f;
        dl->AddRectFilled({cx - 22 * u, cy - 2 * u}, {cx + 22 * u, cy + 2 * u}, hot ? IM_COL32(255, 158, 51, 220) : IM_COL32(150, 155, 165, 120),
                          2 * u);
    } else {
        ImGui::TextDisabled("Код открыт в отдельном окне.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Вернуть сюда")) g_ed.codeDetached = false;
        // A window of its own: moved, sized and closed like the others (closing puts the code back).
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowSize({std::min(620 * u, io.DisplaySize.x * 0.9f), std::min(460 * u, io.DisplaySize.y * 0.6f)},
                                 ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.45f}, ImGuiCond_FirstUseEver, {0.5f, 0.5f});
        ImGui::SetNextWindowSizeConstraints({240 * u, 140 * u}, {FLT_MAX, FLT_MAX});
        bool open = true;
        char title[80];
        std::snprintf(title, sizeof(title), "Код — моды M%d###progcode", modeIdx + 1);
        if (ImGui::Begin(title, &open, ImGuiWindowFlags_NoCollapse)) {
            if (mobile_) ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());  // over the docked sheets
            if (g_ed.text != g_ed.applied) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.70f, 0.30f, 1.0f));
                ImGui::TextUnformatted("● есть неприменённые правки");
                ImGui::PopStyleColor();
            }
            codeView(-1.0f);
        }
        ImGui::End();
        if (!open) g_ed.codeDetached = false;
    }

    // ---- check / apply / revert
    const bool edited = g_ed.text != g_ed.applied;
    {
        // Three in a row when they fit, one under the other on a narrow phone sheet.
        const ImGuiStyle& st = ImGui::GetStyle();
        const float avail = ImGui::GetContentRegionAvail().x;
        const float need = ImGui::CalcTextSize("Отменить правки").x + 2 * st.FramePadding.x;
        const bool row = (avail - 2 * st.ItemSpacing.x) / 3.0f >= need;
        const float w = row ? (avail - 2 * st.ItemSpacing.x) / 3.0f : -FLT_MIN;
        auto next = [&] {
            if (row) ImGui::SameLine();
        };
        if (ImGui::Button("Проверить", {w, 0})) scriptTest();
        next();
        ImGui::BeginDisabled(!edited);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(uiw::kAccent.x, uiw::kAccent.y, uiw::kAccent.z, 0.55f));
        if (ImGui::Button("Применить", {w, 0})) {
            // Only code that compiles and runs reaches the cells; an empty program switches the cell's code off.
            if (g_ed.text.empty() || scriptTest()) {
                g.scripts.set(modeIdx, g_ed.text);
                g_ed.applied = g_ed.text;
                ui_.previewDirty = true;
                toast(g_ed.text.empty() ? "Программа убрана из моды" : "Программа применена: клетки этой моды выполняют её");
            }
        }
        ImGui::PopStyleColor();
        next();
        if (ImGui::Button("Отменить правки", {w, 0})) {
            g_ed.text = g_ed.applied;
            g_ed.hasError = false;
            g_ed.tested = false;
        }
        ImGui::EndDisabled();
    }
    // status of the text
    if (edited) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.70f, 0.30f, 1.0f));
        ImGui::TextUnformatted("● Есть неприменённые правки: клетки выполняют прежнюю версию.");
        ImGui::PopStyleColor();
    }
    if (g_ed.hasError) {
        const ScriptError& e = g_ed.error;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.35f, 0.10f, 0.09f, 0.55f));
        ImGui::BeginChild("err", {-FLT_MIN, 0}, ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.72f, 0.65f, 1.0f));
        if (e.line > 0) {
            if (e.column > 0) ImGui::Text("Строка %d, позиция %d", e.line, e.column);
            else ImGui::Text("Строка %d", e.line);
            if (ImGui::SmallButton("показать")) g_ed.scrollToLine = e.line;
        } else {
            ImGui::TextUnformatted("Ошибка");
        }
        ImGui::PopStyleColor();
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(e.message.c_str());
        if (e.raw != e.message) {
            ImGui::PushFont(fontSmall_);
            ImGui::TextDisabled("dypt: %s", e.raw.c_str());
            ImGui::PopFont();
        }
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        ImGui::PopStyleColor();
    } else if (g_ed.tested) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.88f, 0.55f, 1.0f));
        ImGui::TextWrapped("✓ Собирается и выполняется. %s", g_ed.testSummary.c_str());
        ImGui::PopStyleColor();
    }

    // ---- how it runs in the dish
    {
        const LoadedProgram* lp = g_ed.applied.empty() ? nullptr : mgr.findBySource(g_ed.applied);
        uiw::rowLabel("В чашке");
        if (g_ed.applied.empty()) ImGui::TextDisabled("программы нет — клетка ведёт себя как обычная");
        else if (!lp) ImGui::TextDisabled("ещё не запускалась (вставьте клетку в чашку)");
        else if (lp->disabled) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.45f, 1.0f));
            ImGui::TextWrapped("отключена: %s", lp->lastError.c_str());
            ImGui::PopStyleColor();
        } else if (!lp->lastError.empty()) {
            ImGui::TextWrapped("работает (%llu вызовов), последняя ошибка: %s", (unsigned long long)lp->calls, lp->lastError.c_str());
        } else {
            ImGui::Text("работает, %llu вызовов", (unsigned long long)lp->calls);
        }
        if (lp && lp->disabled && ImGui::Button("Включить снова", {-FLT_MIN, 0})) mgr.retry(g_ed.applied);  // compiled afresh
    }

    // ---- test bench: run it for a while, as the dish would
    if (ImGui::CollapsingHeader("Испытание")) {
        note("Задайте входы и прогоните программу несколько тиков подряд. Как в чашке, её ответ задаёт сигналы и память, "
             "которые она увидит на следующем тике. Входы, которые программа не трогает, остаются как заданы.");
        ImGui::PushItemWidth(-FLT_MIN);
        if (ImGui::BeginTable("in", 4, ImGuiTableFlags_SizingStretchSame)) {
            for (int i = 0; i < SignalCellInputs::kCount; ++i) {
                if (i >= kSignalCount && i < SignalCellInputs::kMemory && i != SignalCellInputs::kLight) continue;
                ImGui::TableNextColumn();
                ImGui::PushID(i);
                ImGui::TextDisabled("%s", SignalCellInputs::name(i));
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::DragFloat("##v", &g_ed.in[i], 0.02f, -20.0f, 20.0f, "%.2f");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::PopItemWidth();
        uiw::rowLabel("Тиков");
        uiw::cellLabSlider("##steps", ImGuiDataType_S32, &g_ed.steps, 1, 1000, "%d", ImGuiSliderFlags_Logarithmic);
        if (ImGui::Button("Прогнать", {-FLT_MIN, 0})) {
            std::vector<float> in(g_ed.in, g_ed.in + SignalCellInputs::kCount);
            mgr.testSequence(g_ed.text, in, g_ed.steps, g_ed.trace, g_ed.traceError, g_ed.traceFailed);
            if (!g_ed.traceError.empty()) {
                g_ed.hasError = true;
                g_ed.error = describeError(g_ed.traceError, g_ed.text);
            } else {
                g_ed.hasError = false;
            }
        }
        if (!g_ed.traceError.empty() && g_ed.traceFailed >= 0)
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.5f, 1.0f), "Ошибка на тике %d (см. выше)", g_ed.traceFailed + 1);
        if (!g_ed.trace.empty()) {
            ImGui::Text("Прогнано тиков: %zu. Что менялось:", g_ed.trace.size());
            const int outs[] = {0, 1, 2, 3, 4, 5, 6, 7, SignalCellInputs::kMemory, SignalCellInputs::kMemory + 1,
                                SignalCellInputs::kMemory + 2, SignalCellInputs::kMemory + 3};
            std::vector<float> series(g_ed.trace.size());
            bool any = false;
            for (int ch : outs) {
                float lo = 1e30f, hi = -1e30f;
                for (size_t k = 0; k < g_ed.trace.size(); ++k) {
                    series[k] = g_ed.trace[k][ch];
                    lo = std::min(lo, series[k]);
                    hi = std::max(hi, series[k]);
                }
                if (hi - lo < 1e-6f && std::fabs(hi - g_ed.in[ch]) < 1e-6f) continue;  // untouched
                any = true;
                char label[48];
                std::snprintf(label, sizeof(label), "%s: %.2f", SignalCellInputs::name(ch), series.back());
                ImGui::PlotLines("##p", series.data(), (int)series.size(), 0, label, lo - 0.05f, hi + 0.05f,
                                 {-FLT_MIN, 38 * u});
            }
            if (!any) ImGui::TextDisabled("ничего: программа не меняет ни сигналов, ни памяти");
        }
    }

    // ---- what the programs printed (журнал(...))
    {
        const std::string fresh = mgr.takeLog();
        if (!fresh.empty()) {
            g_ed.log += fresh;
            if (g_ed.log.size() > 6000) g_ed.log.erase(0, g_ed.log.size() - 6000);
        }
        if (!g_ed.log.empty() && ImGui::CollapsingHeader("Журнал программ")) {
            ImGui::PushFont(fontMono_ ? fontMono_ : fontSmall_);
            ImGui::BeginChild("log", {-FLT_MIN, 110 * u}, ImGuiChildFlags_Borders);
            ImGui::TextUnformatted(g_ed.log.c_str());
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
            ImGui::PopFont();
            if (ImGui::SmallButton("Очистить журнал")) g_ed.log.clear();
        }
    }

    if (ImGui::CollapsingHeader("Как писать программу")) {
        note("Клетка вызывает функцию on_tick(входы) на каждом тике симуляции (тик — 1/400 часа).");
        note("входы — список из 19 чисел: 0..7 — сигналы S1..S8; 8 масса, 9 возраст, 10 азот, 11 связи, 12 свет, "
             "13 пища рядом, 14 травма (всё 0..1); 15..18 — память клетки.");
        note("Ответ — список: места 0..7 задают уровни сигналов S1..S8, места 8..11 — новую память. null на месте или "
             "более короткий список оставляют значение как было. Память у каждой клетки своя.");
        note("журнал(значения...) пишет строку в «Журнал программ».");
        note("Нельзя: import, extern, работа с файлами, памятью процесса и потоками, sleep, exit. Программа, которая "
             "думает дольше сторожевого лимита (меню «Разработчик»), отключается; ошибки времени выполнения тоже "
             "отключают её после нескольких повторов.");
    }
}

bool App::scriptTest() {
    SignalCellManager& mgr = world_->signalCells();
    std::string err;
    std::vector<float> out;
    std::vector<float> in(g_ed.in, g_ed.in + SignalCellInputs::kCount);
    const bool ok = mgr.testProgram(g_ed.text, err, &out, &in);
    g_ed.tested = ok;
    g_ed.hasError = !ok;
    if (!ok) {
        g_ed.error = describeError(err, g_ed.text);
        if (g_ed.error.line > 0) g_ed.scrollToLine = g_ed.error.line;
        return false;
    }
    // What it answered to the bench's inputs.
    std::string s;
    for (size_t i = 0; i < out.size() && i < (size_t)kSignalCellResults; ++i) {
        if (!std::isfinite(out[i])) continue;
        char b[40];
        std::snprintf(b, sizeof(b), "%s%s = %.2f", s.empty() ? "" : ", ",
                      SignalCellInputs::name(i < (size_t)kSignalCount ? (int)i : SignalCellInputs::kMemory + (int)(i - kSignalCount)),
                      out[i]);
        s += b;
    }
    g_ed.testSummary = s.empty() ? "Ответ на входы испытания: ничего не меняет." : "Ответ на входы испытания: " + s + ".";
    return true;
}
