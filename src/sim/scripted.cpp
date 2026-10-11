#include "scripted.hpp"

#include "serialize.hpp"

#include <cstdint>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

struct State {
    std::vector<ScriptedOrganism> organisms;
    std::string status;
};

State load() {
    State st;
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring path(buf, n);
    path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"organisms.dll";
    HMODULE lib = LoadLibraryW(path.c_str());
    if (!lib) {
        st.status = "organisms.dll не найдена — организмы на dypt недоступны";
        return st;
    }
    // The library stays loaded for the lifetime of the process.
    using InitFn = int (*)();
    using ErrFn = const char* (*)();
    using FreeFn = void (*)(void*);
    using CountFn = int64_t (*)();
    using TextFn = char* (*)(int64_t);
    auto init = (InitFn)(void*)GetProcAddress(lib, "organisms_init");
    auto lastError = (ErrFn)(void*)GetProcAddress(lib, "organisms_last_error");
    auto release = (FreeFn)(void*)GetProcAddress(lib, "organisms_free");
    auto count = (CountFn)(void*)GetProcAddress(lib, "organisms_count");
    auto name = (TextFn)(void*)GetProcAddress(lib, "organisms_name");
    auto description = (TextFn)(void*)GetProcAddress(lib, "organisms_description");
    auto genome = (TextFn)(void*)GetProcAddress(lib, "organisms_genome");
    if (!init || !lastError || !release || !count || !name || !description || !genome) {
        st.status = "organisms.dll: нет нужных функций (count/name/description/genome)";
        return st;
    }
    auto failure = [&](const char* what) {
        const char* e = lastError();
        st.status = std::string("organisms.dll: ошибка в ") + what + ": " + (e ? e : "?");
    };
    if (!init()) {
        failure("init");
        return st;
    }
    auto take = [&](char* s) {
        std::string r = s ? s : "";
        if (s) release(s);
        return r;
    };
    int64_t total = count();
    for (int64_t i = 0; i < total; ++i) {
        ScriptedOrganism o;
        o.name = take(name(i));
        o.description = take(description(i));
        std::string text = take(genome(i));
        if (text.empty()) {
            failure("genome");
            continue;
        }
        if (!genomeFromText(o.genome, text)) {
            st.status = "organisms.dll: не разобран геном «" + o.name + "»";
            continue;
        }
        if (o.genome.name.empty()) o.genome.name = o.name;
        st.organisms.push_back(std::move(o));
    }
    if (st.status.empty()) st.status = "organisms.dll: организмов на dypt — " + std::to_string(st.organisms.size());
#elif defined(CELLFAP_EMBEDDED_ORGANISMS)
#include "organisms_data.inc"
    for (const auto& e : kEmbeddedOrganisms) {
        ScriptedOrganism o;
        o.name = e[0];
        o.description = e[1];
        if (!genomeFromText(o.genome, e[2])) continue;
        if (o.genome.name.empty()) o.genome.name = o.name;
        st.organisms.push_back(std::move(o));
    }
    st.status = "организмов на dypt (встроены) — " + std::to_string(st.organisms.size());
#else
    st.status = "организмы на dypt поддержаны только в Windows";
#endif
    return st;
}

const State& state() {
    static const State s = load();
    return s;
}

}  // namespace

const std::vector<ScriptedOrganism>& scriptedOrganisms() { return state().organisms; }
const std::string& scriptedStatus() { return state().status; }

const Genome* scriptedGenome(const char* name) {
    for (const ScriptedOrganism& o : scriptedOrganisms())
        if (o.name == name) return &o.genome;
    return nullptr;
}
