#include "genome.hpp"

#include <cmath>
#include <cstring>

// ---------------------------------------------------------------- cell types
// costPerSec here is the relative energy-usage coefficient from the original game (Phagocyte = 1).
static const CellTypeInfo kTypes[kCellTypeCount] = {
    {"Фагоцит", "Phagocyte", "Поглощает непокрытые частицы пищи при касании.", 1.0f, 0.62f, 0.78f, 0.42f},
    {"Флагеллоцит", "Flagellocyte", "Плывёт вперёд жгутиком. Расход растёт с силой.", 0.1f, 0.45f, 0.70f, 0.90f},
    {"Фотоцит", "Photocyte", "Получает массу из света пропорционально площади.", 2.5f, 0.42f, 0.80f, 0.38f},
    {"Деворoцит", "Devorocyte", "Высасывает массу из незащищённых клеток при касании.", 0.1f, 0.88f, 0.36f, 0.38f},
    {"Липоцит", "Lipocyte", "Запасает до 18 нг жира, очень мало тратит.", 0.03f, 0.95f, 0.85f, 0.52f},
    {"Кератиноцит", "Keratinocyte", "Защищает себя и соседей по адгезину от хищников и вирусов.", 0.01f, 0.78f, 0.70f, 0.58f},
    {"Буйоцит", "Buoyocyte", "Регулирует плотность — всплывает или тонет при гравитации.", 0.02f, 0.62f, 0.86f, 0.92f},
    {"Глюеоцит", "Glueocyte", "Приклеивается к клеткам и стенке при касании (до 5 связей).", 0.01f, 0.86f, 0.80f, 0.40f},
    {"Вироцит", "Virocyte", "Заражает касанием: вписывает свои моды в геном жертвы.", 2.0f, 0.70f, 0.42f, 0.86f},
    {"Нитроцит", "Nitrocyte", "Всегда 100% азота, делится им через адгезины.", 0.2f, 0.36f, 0.72f, 0.72f},
    {"Стереоцит", "Stereocyte", "Сенсор со знаком: + слева, − справа.", 0.1f, 0.82f, 0.62f, 0.86f},
    {"Сенсоцит", "Senseocyte", "Выделяет сигнал пропорционально силе раздражителя.", 0.1f, 0.94f, 0.66f, 0.44f},
    {"Миоцит", "Myocyte", "Сокращает, изгибает и сдвигает связи с соседями. Расход растёт с силой сокращения.", 0.2f, 0.86f, 0.46f, 0.52f},
    {"Нейроцит", "Neurocyte", "Производит сигналы S1–S8 по программе.", 0.1f, 0.96f, 0.86f, 0.62f},
    {"Секроцит", "Secrocyte", "Выделяет вещество на касающиеся клетки.", 0.1f, 0.58f, 0.88f, 0.70f},
    {"Стемоцит", "Stemocyte", "Меняет моду, когда сигнал пути > 0.5.", 0.01f, 0.80f, 0.80f, 0.86f},
    {"Гамета", "Gamete", "Сливается с другой гаметой, смешивая геномы.", 0.0036f, 0.96f, 0.62f, 0.78f},
    {"Цилиоцит", "Ciliocyte", "Ресничками тянет касающиеся клетки; высокое трение.", 0.05f, 0.56f, 0.64f, 0.94f},
    {"Электроцит", "Electrocyte", "Копит заряд и разряжает ток в окружающую воду, когда программа «Разряд» > 0.5.", 0.15f, 0.40f, 0.62f, 0.98f},
    {"Книдоцит", "Cnidocyte", "Стрекательная клетка: выстреливает ядовитую нить в чужую клетку при касании.", 0.12f, 0.92f, 0.52f, 0.30f},
    {"Хроматоцит", "Chromatocyte", "Пигментная клетка: цвет смещается к цвету пигмента по программе (маскировка, сигналы).", 0.03f, 0.55f, 0.40f, 0.30f},
    {"Люминоцит", "Luminocyte", "Светится цветом свечения по программе. Глаза «на свет» чужих организмов плывут к нему: приманка.", 0.02f, 0.70f, 0.90f, 0.85f},
    {"Цистоцит", "Cystocyte", "Спящая циста: броня, почти не тратит еду, не делится. Просыпается в моду «пути 1», когда сигнал > 0.5.", 0.004f, 0.72f, 0.62f, 0.42f},
    {"Хватоцит", "Grappler", "Хватает чужие клетки, пока программа «Хватка» ≥ 0.3 (при ≥ 0.8 — ещё и стенку чашки), и отпускает, когда меньше.", 0.05f, 0.82f, 0.56f, 0.70f},
    {"Фибробласт", "Fibroblast", "Создаёт очень прочные и длинные адгезионные связи между клетками.", 0.03f, 0.88f, 0.65f, 0.45f},
    {"Барорецептор", "Baroreceptor", "Обнаруживает контакт со стенками поля и выделяет сигнальное вещество.", 0.05f, 0.60f, 0.75f, 0.90f},
    {"Эритроцит", "Erythrocyte", "Не имеет ядра, запасает меньше энергии, низкий метаболизм, ослабленные связи.", 0.01f, 0.90f, 0.35f, 0.35f},
    {"Хроноцит", "Chronocyte", "Выделяет сигнал каждые N секунд (настраиваемый интервал).", 0.04f, 0.70f, 0.85f, 0.50f},
    {"Циклоспориноцит", "Cyclosporinocyte", "Защищён от пенициллина, не распространяя защиту на соседей.", 0.02f, 0.65f, 0.80f, 0.60f},
    {"Гранулоцит", "Granulocyte", "Накапливает вредные вещества; при превышении порога погибает, оставляя отравленную точку.", 0.08f, 0.75f, 0.55f, 0.40f},
    {"Гриффоцит", "Griffocyte", "Цепляется за дно супа, сила сцепления зависит от сигнального вещества.", 0.02f, 0.55f, 0.70f, 0.55f},
    {"Спироцит", "Spirocyte", "Вращается вокруг собственной оси в обоих направлениях.", 0.06f, 0.50f, 0.65f, 0.75f},
    {"Хромоцит", "Chromocyte", "Настраиваемый цветовой градиент (-1..1): чёрный→цвет, радуга, по концентрации сигнала.", 0.03f, 0.45f, 0.55f, 0.65f},
    {"Сигнальная клетка", "SignalCell", "Программируемая клетка: скрипт Dypt обрабатывает входные сигналы и формирует выходные.", 0.05f, 0.40f, 0.50f, 0.60f},
};

// One-liners in the spirit of the original game's descriptions.
static const char* kQuips[kCellTypeCount] = {
    "Основная клетка питания в чашках, где есть пища.",
    "Двигает организм. Направление задаёт угол клетки, скорость — программа «Сила плавания».",
    "Основная клетка питания в освещённых чашках. Тратит больше других клеток, поэтому нужен яркий свет.",
    "Клетка хищника: питается чужими клетками вместо пищи.",
    "Служит запасом массы на время нехватки пищи или света.",
    "Броневая клетка. Сама не питается и получает массу от соседей по адгезину.",
    "Плотность задаёт программа «Плотность»: отрицательная — клетка всплывает, положительная — тонет.",
    "Позволяет организму закрепляться на стенке и удерживать другие клетки.",
    "Распространяет ваши моды в чужих организмах.",
    "Нужен, когда в среде мало нитратов: без азота клетки не делятся и погибают.",
    "Помогает организму поворачивать к цели или от неё, если подать сигнал на жгутики или реснички.",
    "Что ощущать, задаёт настройка цели: клетки, пища, свет, стенка и другие.",
    "Позволяет организму ползать и изгибаться, если менять сокращение по сигналу.",
    "Управляет поведением организма: его сигналы считывают другие клетки.",
    "Вещество задаётся настройкой: яд, фермент, запах или сигнал.",
    "Переключает клетку в другую моду по сигналу, например на следующую стадию развития.",
    "Нужна для полового размножения. Сливается только с совместимой модой.",
    "Двигает касающиеся клетки или ползёт по ним. Направление задаёт программа.",
    "Разряд бьёт током клетки других геномов поблизости. Несколько электроцитов вместе бьют сильнее.",
    "Яд нити парализует чужую клетку.",
    "Используется для маскировки и как видимый сигнал.",
    "Используется как приманка для организмов, которые плывут на свет.",
    "Помогает организму пережить голод и неблагоприятные условия.",
    "Используется для ловли добычи и закрепления организма на стенке.",
    "Создаёт прочные волокна соединительной ткани.",
    "Сигнализирует о контакте с твёрдой поверхностью.",
    "Переносит вещества, не имея ядра.",
    "Отсчитывает время и подаёт сигнал по расписанию.",
    "Переносит пенициллин без вреда для себя.",
    "Очищает ткань от токсинов, погибая при переполнении.",
    "Прикрепляет организм к субстрату.",
    "Вращается, создавая движение.",
    "Меняет цвет по заданному закону.",
    "Исполняет программу, написанную игроком.",
};

const char* cellTypeQuip(CellType t) { return kQuips[clampv((int)t, 0, kCellTypeCount - 1)]; }

const CellTypeInfo& cellTypeInfo(CellType t) {
    int i = (int)t;
    if (i < 0 || i >= kCellTypeCount) i = 0;
    return kTypes[i];
}

// ---------------------------------------------------------------- programmable values
static const ProgRange kProgRanges[PF_Count] = {
    {"Сила плавания", 0.0f, 1.0f, 0.5f, 0.5f},
    {"Плотность", -3.0f, 3.0f, 3.0f, 0.0f},
    {"Сокращение", -0.5f, 0.5f, 0.5f, 0.0f},
    {"Изгиб / левые реснички", -1.0f, 1.0f, 1.0f, 0.0f},
    {"Подъём / правые реснички", -1.0f, 1.0f, 1.0f, 0.0f},
    {"Сигнал пути 1", -1.0f, 1.0f, 1.0f, 0.0f},
    {"Сигнал пути 2", -1.0f, 1.0f, 1.0f, 0.0f},
    {"Выход 1", -20.0f, 20.0f, 20.0f, 0.0f},
    {"Выход 2", -20.0f, 20.0f, 20.0f, 0.0f},
    {"Выход 3", -20.0f, 20.0f, 20.0f, 0.0f},
    {"Выход 4", -20.0f, 20.0f, 20.0f, 0.0f},
};

const ProgRange& progRange(int field) { return kProgRanges[clampv(field, 0, PF_Count - 1)]; }

float evalProg(const Prog& p, const ProgInputs& in, int field) {
    const ProgRange& r = progRange(field);
    float v;
    if (p.input == ProgInput::Fixed) {
        v = p.b;
    } else {
        float x = in.v[(int)p.input];
        switch (p.kind) {
            case ProgKind::Step: v = x < p.c ? p.a : p.b; break;
            case ProgKind::Wave: {
                if (p.input == ProgInput::Age) x = in.ageHours;
                float period = std::fabs(p.c) > 1e-3f ? p.c : 1e-3f;
                v = p.a * std::sin(6.2831853f * x / period) + p.b;
                break;
            }
            default: v = p.a * x + p.b; break;
        }
    }
    return clampv(v, r.lo, r.hi);
}

const char* senseTargetName(SenseTarget s) {
    static const char* n[] = {"Клетки", "Пища", "Покрытая пища", "Свет", "Скорость", "Стенка", "Кровь (сок клеток)"};
    return n[clampv((int)s, 0, (int)SenseTarget::Count - 1)];
}

const char* secretionName(Secretion s) {
    static const char* n[] = {"Цианид (HCN)", "Липаза",      "Протеаза",    "Запах пищи",  "Запах покрытой пищи",
                              "Активатор света", "Активатор стенки", "−S1",   "−S2",         "−S3",
                              "−S4",         "+S1",         "+S2",         "+S3",         "+S4",
                              "−S5",         "−S6",         "−S7",         "−S8",         "+S5",
                              "+S6",         "+S7",         "+S8"};
    return n[clampv((int)s, 0, (int)Secretion::Count - 1)];
}

const char* progInputName(ProgInput p) {
    static const char* n[] = {"Константа", "S1", "S2", "S3", "S4", "Масса", "Возраст", "Азот", "Связи", "S5", "S6", "S7", "S8",
                              "Свет", "Еда рядом", "Ранение"};
    return n[clampv((int)p, 0, (int)ProgInput::Count - 1)];
}

// ---------------------------------------------------------------- genome
Mode defaultModeFor(int index) {
    Mode m;
    // Distinct, soft colours around the hue wheel so new modes are easy to tell apart.
    Color c = hsv(0.28f + index * 0.137f, 0.45f, 0.85f);
    m.color = c;
    m.childMode[0] = m.childMode[1] = (uint8_t)index;
    m.stemMode[0] = m.stemMode[1] = (uint8_t)index;
    m.virusCopyFrom = (uint8_t)index;
    m.gameteCompatibleMode = m.gameteModeAfter = (uint8_t)index;
    m.prog[PF_Swim].b = 0.4f;
    for (int i = 0; i < 4; ++i) m.neuroChannel[i] = (uint8_t)i;
    return m;
}

// ---------------------------------------------------------------- copy-on-write tables
static const std::array<std::shared_ptr<Mode>, kMaxModes>& defaultModes() {
    static const std::array<std::shared_ptr<Mode>, kMaxModes> d = [] {
        std::array<std::shared_ptr<Mode>, kMaxModes> a;
        for (int i = 0; i < kMaxModes; ++i) a[i] = std::make_shared<Mode>(defaultModeFor(i));
        return a;
    }();
    return d;
}

ModeTable::ModeTable() : p_(defaultModes()) {}

void ModeTable::copyFrom(const ModeTable& o) {
    for (int i = 0; i < kMaxModes; ++i) {
        // A mode someone may still be editing through a reference gets its own copy.
        p_[i] = o.leaked_[i] ? std::make_shared<Mode>(*o.p_[i]) : o.p_[i];
        leaked_[i] = false;
    }
}

Mode& ModeTable::edit(int i) {
    std::shared_ptr<Mode>& s = p_[i];
    if (s.use_count() != 1) s = std::make_shared<Mode>(*s);
    leaked_[i] = true;
    return *s;
}

bool ModeTable::isDefault(int i) const { return p_[i] == defaultModes()[i] || modeEquals(*p_[i], *defaultModes()[i]); }

const std::string& LabelTable::operator[](int i) const {
    static const std::string empty;
    return p_ ? (*p_)[i] : empty;
}

void LabelTable::set(int i, const std::string& s) {
    if (!p_) {
        if (s.empty()) return;
        p_ = std::make_shared<std::array<std::string, kMaxModes>>();
    } else if (p_.use_count() != 1) {
        p_ = std::make_shared<std::array<std::string, kMaxModes>>(*p_);
    }
    (*p_)[i] = s;
}

static bool progEq(const Prog& a, const Prog& b) {
    return a.input == b.input && a.kind == b.kind && a.a == b.a && a.b == b.b && a.c == b.c;
}
static bool colorEq(const Color& a, const Color& b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

// Field-wise (memcmp would also compare indeterminate padding bytes).
bool modeEquals(const Mode& a, const Mode& b) {
    if (a.type != b.type || !colorEq(a.color, b.color) || a.makeAdhesin != b.makeAdhesin || a.prioritize != b.prioritize ||
        a.splitMass != b.splitMass || a.splitRatio != b.splitRatio || a.nutrientPriority != b.nutrientPriority ||
        a.splitAngle != b.splitAngle || a.adhesinStiffness != b.adhesinStiffness || a.cytoskeleton != b.cytoskeleton ||
        a.adhesinLength != b.adhesinLength || a.maxConnections != b.maxConnections)
        return false;
    for (int c = 0; c < 2; ++c)
        if (a.childAngle[c] != b.childAngle[c] || a.childMode[c] != b.childMode[c] || a.keepAdhesin[c] != b.keepAdhesin[c] ||
            a.mirror[c] != b.mirror[c] || a.stemMode[c] != b.stemMode[c])
            return false;
    for (int f = 0; f < PF_Count; ++f)
        if (!progEq(a.prog[f], b.prog[f])) return false;
    for (int i = 0; i < 4; ++i)
        if (a.neuroChannel[i] != b.neuroChannel[i]) return false;
    return a.virusCopyFrom == b.virusCopyFrom && a.senseTarget == b.senseTarget && a.senseChannel == b.senseChannel &&
           a.senseOutput == b.senseOutput && colorEq(a.senseColor, b.senseColor) && a.senseThreshold == b.senseThreshold &&
           a.secretion == b.secretion && a.gameteCompatibleMode == b.gameteCompatibleMode &&
           a.gameteModeAfter == b.gameteModeAfter && a.gameteTelomeres == b.gameteTelomeres;
}

// ---------------------------------------------------------------- scripts (dypt programs of signal cells)
const std::string& ScriptTable::operator[](int i) const {
    static const std::string kEmpty;
    return p_ && i >= 0 && i < kMaxModes ? p_->v[i] : kEmpty;
}
void ScriptTable::set(int i, const std::string& s) {
    if (i < 0 || i >= kMaxModes) return;
    // Copy on write: genomes that share their programs must not see each other's edits.
    Store copy = p_ ? *p_ : Store{};
    if (s.empty()) copy.v[i].clear();
    else copy.v[i] = s;
    copy.nonEmpty = false;
    for (int k = 0; k < kMaxModes; ++k)
        if (!copy.v[k].empty()) {
            copy.nonEmpty = true;
            break;
        }
    p_ = std::make_shared<const Store>(std::move(copy));
}

bool Genome::sameModes(const Genome& o) const {
    for (int i = 0; i < kMaxModes; ++i)
        if (!modes.sameAs(i, o.modes, i) && !modeEquals(modes[i], o.modes[i])) return false;
    return true;
}

static CellType randomType(Rng& rng, const bool* allowed) {
    for (int tries = 0; tries < 64; ++tries) {
        int t = rng.irange(0, kCellTypeCount - 1);
        if (!allowed || allowed[t]) return (CellType)t;
    }
    return CellType::Phagocyte;
}

static void mutateProg(Prog& p, int field, Rng& rng, bool pointOnly) {
    const ProgRange& r = progRange(field);
    int which = rng.irange(0, pointOnly ? 2 : 4);
    switch (which) {
        case 0: p.a = clampv(p.a + rng.normal() * r.scale * 0.5f, -8 * r.scale, 8 * r.scale); break;
        case 1: p.b = clampv(p.b + rng.normal() * r.scale * 0.25f, r.lo - 8 * r.scale, r.hi + 8 * r.scale); break;
        case 2:
            if (p.kind == ProgKind::Wave) p.c = clampv(p.c * std::exp(rng.normal() * 0.2f), 0.05f, 24.0f);
            else p.c = clampv(p.c + rng.normal() * 0.2f, -1.0f, 1.0f);
            break;
        case 3: p.input = (ProgInput)rng.irange(0, (int)ProgInput::Count - 1); break;
        default: p.kind = (ProgKind)rng.irange(0, (int)ProgKind::Count - 1); break;
    }
}

int mutateGenome(Genome& g, int focusMode, Rng& rng, const MutationOptions& opt) {
    // Mutations hit the active mode or its children most of the time, so they are actually expressed.
    int mi;
    float pick = rng.uniform();
    const Mode& focus = g.modes[clampv(focusMode, 0, kMaxModes - 1)];
    if (pick < 0.5f) mi = focusMode;
    else if (pick < 0.8f) mi = focus.childMode[rng.irange(0, 1)];
    else mi = rng.irange(0, kMaxModes - 1);
    mi = clampv(mi, 0, kMaxModes - 1);
    Mode& m = g.modes.edit(mi);

    if (!opt.pointOnly && rng.chance(0.06f)) return rng.irange(0, kMaxModes - 1);  // active-mode switch

    const int kPoint = 13, kStructural = 9;
    int n = opt.pointOnly ? kPoint : kPoint + kStructural;
    int k = rng.irange(0, n - 1);
    auto jitterColor = [&](float& c) { c = clampv(c + rng.normal() * 0.09f, 0.0f, 1.0f); };
    switch (k) {
        case 0:
            if (m.splitMass > kSplitAtAge && m.splitMass < kNeverSplit) {
                // Move 1..3 slider snaps up or down.
                float steps = std::round((m.splitMass - kSplitMassMin) / kSplitMassStep);
                steps += (rng.chance(0.5f) ? 1.0f : -1.0f) * (float)rng.irange(1, 3);
                m.splitMass = kSplitMassMin + clampv(steps, 0.0f, 22.0f) * kSplitMassStep;
            }
            break;
        case 1: m.splitRatio = clampv(m.splitRatio + (rng.chance(0.5f) ? 1.f : -1.f) / 30.0f, 0.1f, 0.9f); break;
        case 2: m.nutrientPriority = clampv(m.nutrientPriority * std::pow(10.0f, (rng.chance(0.5f) ? 1.f : -1.f) / 12.0f), 0.1f, 10.0f); break;
        case 3: m.splitAngle = wrapAngle(m.splitAngle + deg2rad(15.0f) * (float)rng.irange(-2, 2)); break;
        case 4: m.childAngle[0] = wrapAngle(m.childAngle[0] + deg2rad(15.0f) * (float)rng.irange(-2, 2)); break;
        case 5: m.childAngle[1] = wrapAngle(m.childAngle[1] + deg2rad(15.0f) * (float)rng.irange(-2, 2)); break;
        case 6: jitterColor(m.color.r); break;
        case 7: jitterColor(m.color.g); break;
        case 8: jitterColor(m.color.b); break;
        case 9: m.adhesinStiffness = clampv(m.adhesinStiffness + rng.normal() * 0.1f, 0.0f, 1.0f); break;
        case 10: m.cytoskeleton = clampv(m.cytoskeleton + rng.normal() * 0.125f, 0.5f, 2.0f); break;
        case 11: m.adhesinLength = clampv(m.adhesinLength + rng.normal() * 0.1f, 0.0f, 1.0f); break;
        case 12: {
            // A type-relevant continuous parameter.
            switch (m.type) {
                case CellType::Flagellocyte:
                case CellType::Electrocyte:
                case CellType::Cnidocyte:
                case CellType::Chromatocyte:
                case CellType::Luminocyte:
                case CellType::Grappler: mutateProg(m.prog[PF_Swim], PF_Swim, rng, true); break;
                case CellType::Cystocyte: mutateProg(m.prog[PF_Stem1], PF_Stem1, rng, true); break;
                case CellType::Buoyocyte: mutateProg(m.prog[PF_Density], PF_Density, rng, true); break;
                case CellType::Myocyte: {
                    int f = rng.irange(PF_Contraction, PF_Lift);
                    mutateProg(m.prog[f], f, rng, true);
                    break;
                }
                case CellType::Ciliocyte: {
                    int f = rng.irange(PF_Bend, PF_Lift);
                    mutateProg(m.prog[f], f, rng, true);
                    break;
                }
                case CellType::Stemocyte: {
                    int f = rng.irange(PF_Stem1, PF_Stem2);
                    mutateProg(m.prog[f], f, rng, true);
                    break;
                }
                case CellType::Neurocyte: {
                    int f = rng.irange(PF_Neuro1, PF_Neuro4);
                    mutateProg(m.prog[f], f, rng, true);
                    break;
                }
                case CellType::Senseocyte:
                case CellType::Stereocyte:
                    if (rng.chance(0.5f)) m.senseOutput = clampv(m.senseOutput + 5.0f * (float)rng.irange(-2, 2), -60.0f, 60.0f);
                    else m.senseThreshold = clampv(m.senseThreshold + rng.normal() * 0.15f, 0.0f, 1.7320508f);
                    break;
                default: m.maxConnections = clampv(m.maxConnections + rng.irange(-1, 1), 1, kMaxLinks); break;
            }
            break;
        }
        // ---- structural (not allowed with "only point mutations")
        case 13: m.type = randomType(rng, opt.allowedTypes); break;
        case 14: m.childMode[0] = (uint8_t)rng.irange(0, kMaxModes - 1); break;
        case 15: m.childMode[1] = (uint8_t)rng.irange(0, kMaxModes - 1); break;
        case 16: m.makeAdhesin = !m.makeAdhesin; break;
        case 17: {
            int c = rng.irange(0, 1);
            m.keepAdhesin[c] = !m.keepAdhesin[c];
            break;
        }
        case 18:
            if (rng.chance(0.3f)) m.splitMass = rng.chance(0.5f) ? kNeverSplit : kSplitAtAge;
            else m.splitMass = kSplitMassMin + kSplitMassStep * (float)rng.irange(0, 22);
            break;
        case 19: m.prioritize = !m.prioritize; break;
        case 20: {
            int c = rng.irange(0, 1);
            m.mirror[c] = !m.mirror[c];
            break;
        }
        default: {
            switch (m.type) {
                case CellType::Senseocyte:
                case CellType::Stereocyte:
                    if (rng.chance(0.5f)) m.senseTarget = (SenseTarget)rng.irange(0, (int)SenseTarget::Count - 1);
                    else m.senseChannel = (uint8_t)rng.irange(0, kSignalCount - 1);
                    break;
                case CellType::Secrocyte: m.secretion = (Secretion)rng.irange(0, (int)Secretion::Count - 1); break;
                case CellType::Virocyte: m.virusCopyFrom = (uint8_t)rng.irange(0, kMaxModes - 1); break;
                case CellType::Stemocyte: m.stemMode[rng.irange(0, 1)] = (uint8_t)rng.irange(0, kMaxModes - 1); break;
                case CellType::Cystocyte:
                    if (rng.chance(0.4f)) m.stemMode[0] = (uint8_t)rng.irange(0, kMaxModes - 1);
                    else mutateProg(m.prog[PF_Stem1], PF_Stem1, rng, false);
                    break;
                case CellType::Luminocyte:
                    if (rng.chance(0.5f)) m.senseColor = hsv(rng.uniform(), rng.range(0.3f, 0.8f), rng.range(0.7f, 1.0f));
                    else mutateProg(m.prog[PF_Swim], PF_Swim, rng, false);
                    break;
                case CellType::Neurocyte: {
                    int f = rng.irange(PF_Neuro1, PF_Neuro4);
                    if (rng.chance(0.3f)) m.neuroChannel[f - PF_Neuro1] = (uint8_t)rng.irange(0, kSignalCount - 1);
                    else mutateProg(m.prog[f], f, rng, false);
                    break;
                }
                case CellType::Chromatocyte:
                    if (rng.chance(0.5f)) m.senseColor = hsv(rng.uniform(), rng.range(0.3f, 0.8f), rng.range(0.2f, 0.95f));
                    else mutateProg(m.prog[PF_Swim], PF_Swim, rng, false);
                    break;
                default: {
                    int f = rng.irange(0, PF_Count - 1);
                    mutateProg(m.prog[f], f, rng, false);
                    break;
                }
            }
            break;
        }
    }
    return -1;
}

Genome randomGenome(Rng& rng, const bool* allowedTypes) {
    Genome g;
    g.name = "Случайный";
    int used = rng.irange(1, 4);
    for (int i = 0; i < used; ++i) {
        Mode& m = g.modes.edit(i);
        m.type = randomType(rng, allowedTypes);
        m.color = hsv(rng.uniform(), rng.range(0.35f, 0.7f), rng.range(0.6f, 0.95f));
        m.makeAdhesin = rng.chance(0.5f);
        m.splitMass = kSplitMassMin + kSplitMassStep * (float)rng.irange(6, 20);
        m.splitAngle = deg2rad(15.0f * (float)rng.irange(0, 23));
        m.childMode[0] = (uint8_t)rng.irange(0, used - 1);
        m.childMode[1] = (uint8_t)rng.irange(0, used - 1);
        m.prog[PF_Swim].b = rng.range(0.2f, 0.8f);
    }
    return g;
}
