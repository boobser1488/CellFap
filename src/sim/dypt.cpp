#include "dypt.hpp"

#include <cmath>
#include "serialize.hpp"  // dataDir(): the program's own writable folder

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#ifndef _WIN32
#include <unistd.h>  // getpid
#endif

#ifdef _WIN32
#include <windows.h>
static void* loadDll(const std::string& path) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), (int)path.size(), nullptr, 0);
    if (wlen <= 0) return nullptr;
    std::wstring wpath((size_t)wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), (int)path.size(), wpath.data(), wlen);
    return LoadLibraryW(wpath.c_str());
}
static void* dllSym(void* h, const char* name) { return (void*)GetProcAddress((HMODULE)h, name); }
static void freeDll(void* h) { if (h) FreeLibrary((HMODULE)h); }
#else
#include <dlfcn.h>
static void* loadDll(const std::string& path) { return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL); }
static void* dllSym(void* h, const char* name) { return dlsym(h, name); }
static void freeDll(void* h) { if (h) dlclose(h); }
#endif

namespace dypt {

// Превращаем std::string в DyStr (строка должна жить до конца вызова)
static inline DyStr dyStr(const std::string& s) {
    return DyStr{reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

// Конвертация DyValue (из C ABI) в Value (C++ обёртку)
static Value fromC(const DyValue& v) {
    Value r;
    r.kind = v.kind;
    r.b = v.b != 0;
    r.i = v.i;
    r.f = v.f;
    r.s = v.s.ptr ? std::string(reinterpret_cast<const char*>(v.s.ptr), v.s.len) : std::string();
    if ((v.kind == DY_LIST || v.kind == DY_MAP) && v.items && v.count) {
        r.items.reserve(v.count);
        for (size_t k = 0; k < v.count; ++k) r.items.push_back(fromC(v.items[k]));
    }
    return r;
}

// Конвертация Value в DyValue (C ABI). Вложенные списки размещаются в storage,
// который должен жить до конца вызова DyValue.
static void toC(const Value& v, DyValue& out, std::vector<DyValue>& storage) {
    out.kind = v.kind;
    out.b = v.b ? 1u : 0u;
    out.i = v.i;
    out.f = v.f;
    out.s = v.s.empty() ? DyStr{nullptr, 0} : dyStr(v.s);
    out.items = nullptr;
    out.count = 0;
    if (v.kind == DY_LIST || v.kind == DY_MAP) {
        size_t base = storage.size();
        storage.resize(base + v.items.size());
        for (size_t k = 0; k < v.items.size(); ++k) {
            storage[base + k].kind = DY_NULL;
            toC(v.items[k], storage[base + k], storage);
        }
        out.items = storage.data() + base;
        out.count = v.items.size();
    }
}

bool Library::load(const std::string& path) {
    unload();
    handle_ = loadDll(path);
    if (!handle_) return false;
    auto sym = [&](const char* name) -> void* { return dllSym(handle_, name); };
    abiVersion_ = (decltype(abiVersion_))sym("dypt_abi_version");
    version_ = (decltype(version_))sym("dypt_version");
    run_ = (decltype(run_))sym("dypt_run");
    open_ = (decltype(open_))sym("dypt_open");
    has_ = (decltype(has_))sym("dypt_has");
    call_ = (decltype(call_))sym("dypt_call");
    close_ = (decltype(close_))sym("dypt_close");
    if (!abiVersion_ || !version_ || !run_ || !open_ || !has_ || !call_ || !close_) {
        unload();
        return false;
    }
    return true;
}

void Library::unload() {
    if (handle_) {
        freeDll(handle_);
        handle_ = nullptr;
    }
    abiVersion_ = nullptr;
    version_ = nullptr;
    run_ = nullptr;
    open_ = nullptr;
    has_ = nullptr;
    call_ = nullptr;
    close_ = nullptr;
}

uint32_t Library::abiVersion() { return abiVersion_ ? abiVersion_() : 0; }

std::string Library::version() {
    if (!version_) return "";
    DyStr s = version_();
    return s.ptr ? std::string(reinterpret_cast<const char*>(s.ptr), s.len) : std::string();
}

bool Library::run(const std::string& path, const Value& input, Value& out, std::string& error) {
    char errBuf[2048] = {};
    size_t errLen = 0;
    DyValue inC;
    std::vector<DyValue> inStorage;
    toC(input, inC, inStorage);
    DyValue outC;
    DyStr emptyStr{nullptr, 0};
    int rc = run_(0, dyStr(path), emptyStr, nullptr, 0, nullptr /*host*/, errBuf, sizeof(errBuf), &errLen);
    if (rc != 0) {
        error = errLen > 0 ? std::string(errBuf, errLen) : std::string(errBuf);
        return false;
    }
    // При host=nullptr результат выполнения теряется (скрипт только печатает).
    error = "dypt_run: результат недоступен без host; используйте dypt_open+dypt_call";
    return false;
}

void* Library::open(const std::string& path, const std::vector<std::string>& hostNames,
                    const DyHost* host, std::string& error) {
    char errBuf[2048] = {};
    size_t errLen = 0;
    std::vector<DyStr> names;
    for (const auto& n : hostNames) names.push_back(dyStr(n));
    void* s = open_(dyStr(path), names.data(), names.size(), host, errBuf, sizeof(errBuf), &errLen);
    if (!s) {
        error = errLen > 0 ? std::string(errBuf, errLen) : std::string(errBuf);
    }
    return s;
}

bool Library::has(void* script, const std::string& name) {
    return has_(script, dyStr(name)) != 0;
}

bool Library::call(void* script, const std::string& name, const std::vector<Value>& args, Value& out, std::string& error) {
    char errBuf[2048] = {};
    size_t errLen = 0;
    std::vector<DyValue> storage;
    storage.reserve(args.size());
    std::vector<DyValue> argsC(args.size());
    for (size_t k = 0; k < args.size(); ++k) {
        argsC[k].kind = DY_NULL;
        toC(args[k], argsC[k], storage);
    }
    DyValue outC;
    int rc = call_(script, dyStr(name), argsC.data(), args.size(), &outC, errBuf, sizeof(errBuf), &errLen);
    if (rc != 0) {
        error = errLen > 0 ? std::string(errBuf, errLen) : std::string(errBuf);
        return false;
    }
    out = fromC(outC);
    return true;
}

void Library::close(void* script) {
    if (script && close_) close_(script);
}

// ---- HostContext ----

int HostContext::callBridge(void* user, DyStr name, const DyValue* args, size_t argc, DyValue* out) {
    auto* self = static_cast<HostContext*>(user);
    std::string n = name.ptr ? std::string(reinterpret_cast<const char*>(name.ptr), name.len) : std::string();
    std::vector<Value> vargs;
    vargs.reserve(argc);
    for (size_t k = 0; k < argc; ++k) vargs.push_back(fromC(args[k]));
    Value outVal;
    int rc = self->handler_ ? self->handler_(n, vargs, outVal) : 1;
    if (rc != 0) {
        *out = DyValue{};
        out->kind = DY_STRING;
        static thread_local std::string msg;
        msg = "ошибка вызова '" + n + "'";
        out->s = dyStr(msg);
        return 1;
    }
    static thread_local std::vector<DyValue> storage;
    storage.clear();
    toC(outVal, *out, storage);
    return 0;
}

void HostContext::printBridge(void* user, DyStr text) {
    auto* self = static_cast<HostContext*>(user);
    if (self->printer_) {
        std::string s = text.ptr ? std::string(reinterpret_cast<const char*>(text.ptr), text.len) : std::string();
        self->printer_(s);
    }
}

// Инициализация хоста: заполняет указатели мостов один раз.
void HostContext::init() {
    host_.user = this;
    host_.call = &HostContext::callBridge;
    host_.print = &HostContext::printBridge;
}

// ---- ScriptedCell ----

ScriptedCell::ScriptedCell(HostContext* host) : host_(host) { host_->init(); }

ScriptedCell::~ScriptedCell() { unload(); }

bool ScriptedCell::load(const std::string& source, std::string& error) {
    unload();
    if (source.size() > config_.maxSourceBytes) {
        error = "превышен максимальный размер скрипта (" + std::to_string(config_.maxSourceBytes) + " байт)";
        return false;
    }
    source_ = source;
    // Сохраняем скрипт во временный файл (dypt_open читает с диска)
    char tmpDir[512];
    char tmpPath[512];
#ifdef _WIN32
    if (!GetTempPathA(sizeof(tmpDir), tmpDir)) {
        error = "не удалось получить временный каталог";
        return false;
    }
    if (!GetTempFileNameA(tmpDir, "dypt", 0, tmpPath)) {
        error = "не удалось создать временный файл";
        return false;
    }
    remove(tmpPath);  // GetTempFileName created it; the script goes to the same name with .dpt
    path_ = std::string(tmpPath) + ".dpt";
#else
    // On Android and other POSIX systems: the app's own cache directory. /tmp may not exist or may not be
    // writable in the app sandbox, so the caller-supplied data directory is the reliable place.
    snprintf(tmpPath, sizeof(tmpPath), "%s/dypt_%d.dpt", dataDir("shaders").c_str(), (int)getpid());
    path_ = tmpPath;
#endif
    FILE* f = fopen(path_.c_str(), "wb");
    if (!f) {
        error = "не удалось записать временный файл скрипта";
        return false;
    }
    fwrite(source.data(), 1, source.size(), f);
    fclose(f);

    host_->init();
    script_ = host_->library()->open(path_, host_->hostNames(), host_->host(), error);
    if (!script_) {
        remove(path_.c_str());
        path_.clear();
        return false;
    }
    return true;
}

void ScriptedCell::unload() {
    if (script_) {
        host_->library()->close(script_);
        script_ = nullptr;
    }
    if (!path_.empty()) {
        remove(path_.c_str());
        path_.clear();
    }
    source_.clear();
    ticksExecuted_ = 0;
}

bool ScriptedCell::hasOnTick() {
    if (!script_) return false;
    return host_->library()->has(script_, "on_tick");
}

bool ScriptedCell::onTick(const std::vector<float>& inputs, std::vector<float>& outputs, std::string& error) {
    if (!script_) {
        error = "скрипт не загружен";
        return false;
    }
    hostCallsThisTick_ = 0;
    // Входы: список float
    std::vector<Value> args;
    args.reserve(inputs.size());
    for (float v : inputs) args.push_back(Value::floating(v));
    Value argList = Value::list(args);
    Value outVal;
    if (!host_->library()->call(script_, "on_tick", {argList}, outVal, error)) {
        return false;
    }
    outputs.clear();
    if (outVal.kind == DY_LIST) {
        outputs.reserve(outVal.items.size());
        for (const auto& v : outVal.items) {
            outputs.push_back(v.isNull() ? std::nanf("") : (float)v.asFloat());  // null: "leave that place as it was"
        }
    } else if (outVal.kind == DY_NULL) {
        // Nothing returned: nothing changes.
    } else {
        static const char* kKinds[] = {"null", "логическое значение", "целое число", "число", "строку", "список", "словарь"};
        error = std::string("on_tick должна вернуть список чисел, а вернула ") + (outVal.kind < 7 ? kKinds[outVal.kind] : "что-то другое");
        return false;
    }
    for (const Value& v : outVal.items)
        if (!v.isNull() && v.kind != DY_INT && v.kind != DY_FLOAT && v.kind != DY_BOOL) {
            error = "в списке, который вернула on_tick, есть не число (строка или вложенный список)";
            return false;
        }
    if (outputs.size() > config_.maxOutputFloats) {
        outputs.resize(config_.maxOutputFloats);
    }
    ++ticksExecuted_;
    return true;
}

}  // namespace dypt
