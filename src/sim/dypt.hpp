#pragma once
// C++ обёртка над C ABI встраиваемого dypt (dypt_embed.dll / libdypt_embed.so).
// Соответствует embed/src/lib.rs из репозитория dypt.
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <functional>
#include <memory>

// Типы, пересекающие границу C ABI (те же имена, что в embed/src/lib.rs).
extern "C" {
struct DyStr {
    const uint8_t* ptr;
    size_t len;
};
struct DyValue {
    uint32_t kind;
    uint32_t b;
    int64_t i;
    double f;
    DyStr s;
    const DyValue* items;
    size_t count;
};
struct DyHost {
    void* user;
    int (*call)(void* user, DyStr name, const DyValue* args, size_t argc, DyValue* out);
    void (*print)(void* user, DyStr text);
};

uint32_t dypt_abi_version(void);
DyStr dypt_version(void);
int dypt_run(uint32_t kind, DyStr input, DyStr base_dir, const DyStr* names, size_t name_count,
             const DyHost* host, char* err, size_t err_cap, size_t* err_len);
void* dypt_open(DyStr path, const DyStr* names, size_t name_count, const DyHost* host,
                char* err, size_t err_cap, size_t* err_len);
int dypt_has(void* script, DyStr name);
int dypt_call(void* script, DyStr name, const DyValue* args, size_t argc, DyValue* out,
              char* err, size_t err_cap, size_t* err_len);
void dypt_close(void* script);
}  // extern "C"

// Значения dypt
constexpr uint32_t DY_NULL = 0, DY_BOOL = 1, DY_INT = 2, DY_FLOAT = 3, DY_STRING = 4, DY_LIST = 5, DY_MAP = 6;

namespace dypt {

// Удобная обёртка над DyValue
struct Value {
    uint32_t kind = DY_NULL;
    bool b = false;
    int64_t i = 0;
    double f = 0;
    std::string s;
    std::vector<Value> items;

    static Value null() { return Value{}; }
    static Value boolean(bool v) { Value r; r.kind = DY_BOOL; r.b = v; return r; }
    static Value integer(int64_t v) { Value r; r.kind = DY_INT; r.i = v; return r; }
    static Value floating(double v) { Value r; r.kind = DY_FLOAT; r.f = v; return r; }
    static Value string(const std::string& v) { Value r; r.kind = DY_STRING; r.s = v; return r; }
    static Value list(const std::vector<Value>& v) { Value r; r.kind = DY_LIST; r.items = v; return r; }

    bool isNull() const { return kind == DY_NULL; }
    bool asBool() const { return b || i != 0 || f != 0; }
    double asFloat() const {
        if (kind == DY_INT) return (double)i;
        if (kind == DY_FLOAT) return f;
        if (kind == DY_BOOL) return b ? 1 : 0;
        return 0;
    }
    int64_t asInt() const {
        if (kind == DY_FLOAT) return (int64_t)f;
        if (kind == DY_BOOL) return b ? 1 : 0;
        return i;
    }
};

// Загрузчик динамической библиотеки dypt_embed
class Library {
public:
    Library() = default;
    ~Library() { unload(); }
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;

    bool load(const std::string& path);  // возвращает false, если библиотека не найдена
    void unload();
    void release() { handle_ = nullptr; }  // forget the library without unloading it (a thread still runs in it)
    bool loaded() const { return handle_ != nullptr; }

    uint32_t abiVersion();
    std::string version();
    // Запуск скрипта один раз (без сохранения окружения)
    bool run(const std::string& path, const Value& input, Value& out, std::string& error);
    // Открыть живой скрипт (сохраняет окружение между вызовами)
    void* open(const std::string& path, const std::vector<std::string>& hostNames,
               const DyHost* host, std::string& error);
    bool has(void* script, const std::string& name);
    bool call(void* script, const std::string& name, const std::vector<Value>& args, Value& out, std::string& error);
    void close(void* script);

private:
    void* handle_ = nullptr;
    // Указатели на функции (получаются при загрузке)
    uint32_t (*abiVersion_)() = nullptr;
    DyStr (*version_)() = nullptr;
    int (*run_)(uint32_t, DyStr, DyStr, const DyStr*, size_t, const DyHost*, char*, size_t, size_t*) = nullptr;
    void* (*open_)(DyStr, const DyStr*, size_t, const DyHost*, char*, size_t, size_t*) = nullptr;
    int (*has_)(void*, DyStr) = nullptr;
    int (*call_)(void*, DyStr, const DyValue*, size_t, DyValue*, char*, size_t, size_t*) = nullptr;
    void (*close_)(void*) = nullptr;
};

// Хост-окружение: скрипт вызывает функции игры по имени через DyHost.call.
// CellSandbox реализует безопасный набор функций для клеток.
class HostContext {
public:
    explicit HostContext(Library* lib) : lib_(lib) {}

    // Регистрация функции хоста (доступна скрипту по имени)
    void setHandler(std::function<int(const std::string& name, const std::vector<Value>& args, Value& out)> handler) {
        handler_ = std::move(handler);
    }
    void setPrinter(std::function<void(const std::string& text)> printer) { printer_ = std::move(printer); }

    DyHost* host() { return &host_; }
    const std::vector<std::string>& hostNames() const { return hostNames_; }
    void setHostNames(std::vector<std::string> names) { hostNames_ = std::move(names); }
    void init();  // заполнить указатели мостов (вызывается один раз перед открытием скриптов)

    Library* library() { return lib_; }

private:
    Library* lib_;
    std::function<int(const std::string&, const std::vector<Value>&, Value&)> handler_;
    std::function<void(const std::string&)> printer_;
    std::vector<std::string> hostNames_;
    DyHost host_{};

    static int callBridge(void* user, DyStr name, const DyValue* args, size_t argc, DyValue* out);
    static void printBridge(void* user, DyStr text);

    friend class Library;
};

// Песочница для одного пользовательского скрипта с лимитами
struct SandboxConfig {
    size_t maxSourceBytes = 64 * 1024;       // максимальный размер исходного кода
    size_t maxHostCallsPerTick = 256;         // лимит вызовов функций хоста за один on_tick
    size_t maxOutputFloats = 64;             // максимум чисел в выходном списке
    double maxSecondsPerTick = 0.002;        // мягкий лимит времени на один вызов (для информации)
};

class ScriptedCell {
public:
    explicit ScriptedCell(HostContext* host);
    ~ScriptedCell();
    ScriptedCell(const ScriptedCell&) = delete;
    ScriptedCell& operator=(const ScriptedCell&) = delete;

    // Загрузить и открыть скрипт. Возвращает false с сообщением об ошибке при неудаче.
    bool load(const std::string& source, std::string& error);
    void unload();
    bool loaded() const { return script_ != nullptr; }
    const std::string& source() const { return source_; }
    size_t ticksExecuted() const { return ticksExecuted_; }

    // Any function of the script (mods call their own entry points).
    bool has(const std::string& name);
    bool call(const std::string& name, const std::vector<Value>& args, Value& out, std::string& error);
    // Проверка, что в скрипте есть точка входа on_tick.
    bool hasOnTick();
    // Вызвать on_tick(inputs) -> outputs. Возвращает false при ошибке выполнения.
    // inputs: плоский список float (входные сигналы, масса, возраст, ...).
    // outputs: плоский список float (значения выходных сигналов).
    bool onTick(const std::vector<float>& inputs, std::vector<float>& outputs, std::string& error);

    // Состояние скрипта между вызовами хранится в окружении dypt (переменные верхнего уровня).

    HostContext* host() const { return host_; }

private:
    HostContext* host_;
    void* script_ = nullptr;
    std::string source_;
    std::string path_;  // временный файл скрипта
    size_t ticksExecuted_ = 0;
    SandboxConfig config_;
    int hostCallsThisTick_ = 0;
};

}  // namespace dypt
