#include "analyzer.hpp"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLibrary>
#include <QString>
#include <cstddef>
#include <cmath>
#include <exception>
#include <limits>
#include <set>

namespace ac {
namespace {
struct lua_State;
using LuaInteger = ptrdiff_t;
using LuaFunction = int (*)(lua_State *);
constexpr int GlobalsIndex = -10002;
constexpr int TypeNumber = 3, TypeString = 4, TypeTable = 5;

// Only public Lua 5.1 C API symbols are resolved. No OBS-owned VM is accessed.
struct LuaApi {
    QLibrary library;
    lua_State *(*newState)() = nullptr;
    void (*openLibs)(lua_State *) = nullptr;
    void (*close)(lua_State *) = nullptr;
    int (*loadBuffer)(lua_State *, const char *, size_t, const char *) = nullptr;
    int (*pcall)(lua_State *, int, int, int) = nullptr;
    void (*setTop)(lua_State *, int) = nullptr;
    int (*type)(lua_State *, int) = nullptr;
    const char *(*toString)(lua_State *, int, size_t *) = nullptr;
    double (*toNumber)(lua_State *, int) = nullptr;
    int (*toBoolean)(lua_State *, int) = nullptr;
    void (*pushString)(lua_State *, const char *, size_t) = nullptr;
    void (*pushInteger)(lua_State *, LuaInteger) = nullptr;
    void (*pushBoolean)(lua_State *, int) = nullptr;
    void (*pushNil)(lua_State *) = nullptr;
    void (*pushPointer)(lua_State *, void *) = nullptr;
    void (*pushClosure)(lua_State *, LuaFunction, int) = nullptr;
    void (*createTable)(lua_State *, int, int) = nullptr;
    void (*rawSetI)(lua_State *, int, int) = nullptr;
    void (*getField)(lua_State *, int, const char *) = nullptr;
    void (*setField)(lua_State *, int, const char *) = nullptr;
    int (*next)(lua_State *, int) = nullptr;

    template<typename T> bool bind(T &target, const char *name)
    {
        target = reinterpret_cast<T>(library.resolve(name));
        return target != nullptr;
    }

    bool load(const QString &path)
    {
        // An explicit absolute path prevents DLL lookup via the current directory.
        if (!QFileInfo(path).isAbsolute() || !QFileInfo(path).isFile()) return false;
        library.setFileName(path);
        if (!library.load()) return false;
        return bind(newState, "luaL_newstate") && bind(openLibs, "luaL_openlibs") &&
            bind(close, "lua_close") && bind(loadBuffer, "luaL_loadbuffer") &&
            bind(pcall, "lua_pcall") && bind(setTop, "lua_settop") &&
            bind(type, "lua_type") && bind(toString, "lua_tolstring") &&
            bind(toNumber, "lua_tonumber") && bind(toBoolean, "lua_toboolean") &&
            bind(pushString, "lua_pushlstring") && bind(pushInteger, "lua_pushinteger") &&
            bind(pushBoolean, "lua_pushboolean") && bind(pushNil, "lua_pushnil") &&
            bind(pushPointer, "lua_pushlightuserdata") && bind(pushClosure, "lua_pushcclosure") &&
            bind(createTable, "lua_createtable") && bind(rawSetI, "lua_rawseti") &&
            bind(getField, "lua_getfield") && bind(setField, "lua_setfield") && bind(next, "lua_next");
    }
    std::string string(lua_State *state, int index)
    {
        size_t length = 0;
        const char *value = toString(state, index, &length);
        return value ? std::string(value, length) : std::string();
    }
    void pop(lua_State *state) { setTop(state, -2); }
};

struct Vm;
thread_local Vm *activeVm = nullptr;

struct Vm {
    LuaApi api;
    lua_State *state = nullptr;
    QString moduleDirectory;
    CancelFlag cancel;
    Vm *previous = nullptr;
    Vm() : previous(activeVm) { activeVm = this; }
    ~Vm()
    {
        if (state) api.close(state);
        activeVm = previous;
    }
    bool cancelled() const { return cancel && cancel->load(std::memory_order_relaxed); }

    static int readModule(lua_State *state)
    {
        auto &vm = *activeVm;
        const std::string name = vm.api.string(state, 1);
        static const std::set<std::string> allowed = {
            "ac_roi", "ac_tune", "ac_tune_math", "ac_tune_despill", "ac_screen",
            "ac_diag", "ac_person", "ac_plate_math", "ac_plate_warn", "ac_ui",
            "ac_autoplate", "ac_autoplate_img"
        };
        QByteArray content;
        QString error;
        if (!allowed.count(name)) {
            error = QStringLiteral("허용되지 않은 분석 모듈: ") + QString::fromStdString(name);
        } else {
            QFile file(QDir(vm.moduleDirectory).filePath(QString::fromStdString(name) + ".lua"));
            if (!file.open(QIODevice::ReadOnly)) error = file.errorString();
            else {
                content = file.readAll();
                if (content.startsWith("\xEF\xBB\xBF")) content.remove(0, 3);
                if (file.error() != QFileDevice::NoError) error = file.errorString();
            }
        }
        if (error.isEmpty()) {
            vm.api.pushString(state, content.constData(), size_t(content.size()));
            return 1;
        }
        const QByteArray detail = error.toUtf8();
        vm.api.pushNil(state);
        vm.api.pushString(state, detail.constData(), size_t(detail.size()));
        return 2;
    }
    static int isCancelled(lua_State *state)
    {
        activeVm->api.pushBoolean(state, activeVm->cancelled());
        return 1;
    }
    void callback(const char *name, LuaFunction function)
    {
        api.pushClosure(state, function, 0);
        api.setField(state, GlobalsIndex, name);
    }
    std::string fieldString(const char *name)
    {
        api.getField(state, -1, name);
        const auto value = api.string(state, -1);
        api.pop(state);
        return value;
    }
};

AnalysisResult failure(const std::string &message, bool cancelled = false)
{
    AnalysisResult result;
    result.cancelled = cancelled;
    result.message = cancelled ? "분석을 취소했습니다" : message;
    return result;
}

AnalysisResult runAnalysis(const std::vector<Frame> &frames, Roi roi, AnalysisMode mode,
                           const CancelFlag &cancel, const std::string &directory,
                           const std::string &libraryPath)
{
    if (cancel && cancel->load(std::memory_order_relaxed)) return failure({}, true);
    const size_t expected = mode == AnalysisMode::Auto ? 5 : 30;
    if (frames.size() != expected)
        return failure(mode == AnalysisMode::Auto ? "자동 맞춤에는 프레임 5장이 필요합니다" : "빈 배경 학습에는 프레임 30장이 필요합니다");
    const auto &first = frames.front();
    if (!first.valid() || first.width > uint32_t(std::numeric_limits<int>::max()) ||
        first.height > uint32_t(std::numeric_limits<int>::max()))
        return failure("올바른 RGBA8 프레임이 아닙니다");
    for (const auto &frame : frames)
        if (!frame.valid() || frame.width != first.width || frame.height != first.height)
            return failure("분석 프레임의 크기 또는 형식이 서로 다릅니다");
    if (roi.top < 0 || roi.bottom < 0 || roi.left < 0 || roi.right < 0 ||
        roi.top > 10000 || roi.bottom > 10000 || roi.left > 10000 || roi.right > 10000)
        return failure("분석 제외 값은 0~10000 사이 정수여야 합니다");
    if (int64_t(roi.top) + roi.bottom >= first.height || int64_t(roi.left) + roi.right >= first.width)
        return failure("분석 제외 값이 화면보다 큽니다");

    Vm vm;
    vm.cancel = cancel;
    vm.moduleDirectory = QString::fromUtf8(directory.data(), int(directory.size()));
    if (!QFileInfo(vm.moduleDirectory).isAbsolute()) return failure("분석 모듈 폴더는 절대 경로여야 합니다");
    if (!vm.api.load(QString::fromUtf8(libraryPath.data(), int(libraryPath.size()))))
        return failure("OBS의 LuaJIT 런타임(lua51.dll)을 불러오지 못했습니다: " + vm.api.library.errorString().toStdString());
    vm.state = vm.api.newState();
    if (!vm.state) return failure("분석용 LuaJIT 메모리를 할당하지 못했습니다");
    vm.api.openLibs(vm.state);
    vm.callback("_ac_read_module", Vm::readModule);
    vm.callback("_ac_cancelled", Vm::isCancelled);
    QFile adapter(QDir(vm.moduleDirectory).filePath("../analysis.lua"));
    if (!adapter.open(QIODevice::ReadOnly)) return failure("분석 어댑터 analysis.lua를 읽지 못했습니다");
    QByteArray source = adapter.readAll();
    if (source.startsWith("\xEF\xBB\xBF")) source.remove(0, 3);
    if (adapter.error() != QFileDevice::NoError) return failure("분석 어댑터 analysis.lua 읽기 오류");
    if (vm.api.loadBuffer(vm.state, source.constData(), size_t(source.size()), "@analysis.lua") ||
        vm.api.pcall(vm.state, 0, 1, 0))
        return failure("분석기를 준비하지 못했습니다: " + vm.api.string(vm.state, -1));

    vm.api.createTable(vm.state, int(frames.size()), 0);
    int index = 0;
    for (const auto &frame : frames) {
        // The synchronous private VM finishes before these C++ buffers are released.
        vm.api.pushPointer(vm.state, const_cast<uint8_t *>(frame.rgba.data()));
        vm.api.rawSetI(vm.state, -2, ++index);
    }
    vm.api.pushInteger(vm.state, first.width);
    vm.api.pushInteger(vm.state, first.height);
    vm.api.createTable(vm.state, 4, 0);
    index = 0;
    for (const int value : {roi.top, roi.bottom, roi.left, roi.right}) {
        vm.api.pushInteger(vm.state, value);
        vm.api.rawSetI(vm.state, -2, ++index);
    }
    const char *modeName = mode == AnalysisMode::Auto ? "auto" : "plate";
    vm.api.pushString(vm.state, modeName, mode == AnalysisMode::Auto ? 4 : 5);
    if (vm.api.pcall(vm.state, 5, 1, 0)) {
        const auto error = vm.api.string(vm.state, -1);
        return failure("분석 중 오류: " + error, vm.cancelled() || error.find("__AC_CANCELLED__") != std::string::npos);
    }
    if (vm.cancelled()) return failure({}, true);
    if (vm.api.type(vm.state, -1) != TypeTable) return failure("분석 결과 형식이 올바르지 않습니다");
    AnalysisResult result;
    vm.api.getField(vm.state, -1, "success");
    result.success = vm.api.toBoolean(vm.state, -1) != 0;
    vm.api.pop(vm.state);
    result.message = vm.fieldString("message");
    if (!result.success) return result;
    result.method = vm.fieldString("method");
    if (result.method != "color" && result.method != "plate") return failure("지원하지 않는 분석 결과입니다");
    for (const char *field : {"numbers", "strings"}) {
        vm.api.getField(vm.state, -1, field);
        if (vm.api.type(vm.state, -1) != TypeTable) return failure("필터 설정 결과가 누락되었습니다");
        vm.api.pushNil(vm.state);
        while (vm.api.next(vm.state, -2)) {
            if (vm.api.type(vm.state, -2) != TypeString) return failure("필터 설정 이름이 올바르지 않습니다");
            const auto key = vm.api.string(vm.state, -2);
            if (field[0] == 'n') {
                if (vm.api.type(vm.state, -1) != TypeNumber) return failure("필터 설정 숫자가 올바르지 않습니다");
                const double value = vm.api.toNumber(vm.state, -1);
                if (!std::isfinite(value)) return failure("필터 설정에 유효하지 않은 숫자가 있습니다");
                result.numbers[key] = value;
            } else {
                if (vm.api.type(vm.state, -1) != TypeString) return failure("필터 설정 문자열이 올바르지 않습니다");
                result.strings[key] = vm.api.string(vm.state, -1);
            }
            vm.api.pop(vm.state);
        }
        vm.api.pop(vm.state);
    }
    if (result.method == "plate") {
        vm.api.getField(vm.state, -1, "rgba");
        size_t length = 0;
        const char *bytes = vm.api.toString(vm.state, -1, &length);
        if (!bytes || length != first.rgba.size()) return failure("배경판 RGBA8 크기가 올바르지 않습니다");
        result.plate.width = first.width;
        result.plate.height = first.height;
        result.plate.rgba.assign(reinterpret_cast<const uint8_t *>(bytes), reinterpret_cast<const uint8_t *>(bytes) + length);
    }
    if (vm.cancelled()) return failure({}, true);
    return result;
}
} // namespace

AnalysisResult analyze(const std::vector<Frame> &frames, Roi roi, AnalysisMode mode,
                       const CancelFlag &cancel, const std::string &moduleDirectory,
                       const std::string &luaLibraryPath)
{
    try { return runAnalysis(frames, roi, mode, cancel, moduleDirectory, luaLibraryPath); }
    catch (const std::exception &error) { return failure(std::string("분석용 자원 준비 실패: ") + error.what()); }
}
} // namespace ac
