#include "analyzer.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QLibrary>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const std::string &message)
{
    if (!condition) throw std::runtime_error(message);
}

ac::Frame solid(uint8_t r, uint8_t g, uint8_t b, uint64_t time = 1)
{
    ac::Frame frame;
    frame.width = 128;
    frame.height = 96;
    frame.timestamp = time;
    frame.rgba.resize(size_t(frame.width) * frame.height * 4);
    for (size_t i = 0; i < frame.rgba.size(); i += 4) {
        frame.rgba[i] = r;
        frame.rgba[i + 1] = g;
        frame.rgba[i + 2] = b;
        frame.rgba[i + 3] = 255;
    }
    return frame;
}

// Independent reference invocation: original modules only, no analysis.lua adapter.
// Tests both the adapter's averaging and every numeric/string tuning output.
std::map<std::string, std::string> reference(const QString &directory, const QString &library)
{
    QLibrary dll(library);
    require(dll.load(), "reference LuaJIT load failed");
    using State = void;
    auto newState = reinterpret_cast<State *(*)()>(dll.resolve("luaL_newstate"));
    auto openLibs = reinterpret_cast<void (*)(State *)>(dll.resolve("luaL_openlibs"));
    auto close = reinterpret_cast<void (*)(State *)>(dll.resolve("lua_close"));
    auto load = reinterpret_cast<int (*)(State *, const char *, size_t, const char *)>(dll.resolve("luaL_loadbuffer"));
    auto call = reinterpret_cast<int (*)(State *, int, int, int)>(dll.resolve("lua_pcall"));
    auto string = reinterpret_cast<const char *(*)(State *, int, size_t *)>(dll.resolve("lua_tolstring"));
    require(newState && openLibs && close && load && call && string, "reference API missing");
    QByteArray script;
    const QStringList modules = {"ac_roi", "ac_tune", "ac_tune_math", "ac_tune_despill", "ac_screen",
        "ac_diag", "ac_person", "ac_plate_math", "ac_plate_warn", "ac_ui", "ac_autoplate", "ac_autoplate_img"};
    for (const QString &name : modules) {
        QFile file(QDir(directory).filePath(name + ".lua"));
        require(file.open(QIODevice::ReadOnly), "reference module read failed");
        const QByteArray source = file.readAll();
        QByteArray delimiter("====");
        while (source.contains("]" + delimiter + "]")) delimiter += '=';
        script += "package.preload['" + name.toUtf8() + "']=assert(loadstring([" + delimiter + "[" + source + "]" + delimiter + "]))\n";
    }
    script += R"lua(
local ffi=require('ffi')
local n=128*96*4
local sum=ffi.new('uint16_t[?]',n)
for f=0,4 do
  for i=0,n-1,4 do
    sum[i]=sum[i]+35+f
    sum[i+1]=sum[i+1]+190
    sum[i+2]=sum[i+2]+60
    sum[i+3]=sum[i+3]+255
  end
end
local average=ffi.new('uint8_t[?]',n)
for i=0,n-1 do average[i]=math.floor((sum[i]+2)/5) end
local params,info=require('ac_tune').autotune_hybrid(average,128,96,0)
assert(params,info)
local lines={}
for key,value in pairs(params) do lines[#lines+1]=key..'='..tostring(value) end
table.sort(lines)
return table.concat(lines,'\n')
)lua";
    State *state = newState();
    require(state != nullptr, "reference VM allocation failed");
    openLibs(state);
    const int loadStatus = load(state, script.constData(), size_t(script.size()), "reference-original");
    const int status = loadStatus ? loadStatus : call(state, 0, 1, 0);
    size_t length = 0;
    const char *result = string(state, -1, &length);
    const QByteArray output(result ? result : "", int(length));
    close(state);
    require(status == 0, "reference calculation failed: " + output.toStdString());
    std::map<std::string, std::string> values;
    for (const QByteArray &line : output.split('\n')) {
        const int separator = line.indexOf('=');
        require(separator > 0, "reference output malformed");
        values[line.left(separator).toStdString()] = line.mid(separator + 1).toStdString();
    }
    return values;
}
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    try {
        require(application.arguments().size() == 3, "usage: analyzer-test <absolute lua module directory> <absolute lua51.dll>");
        const QString directory = QDir(application.arguments()[1]).absolutePath();
        const QString library = application.arguments()[2];
        const std::string modules = directory.toUtf8().toStdString();
        const std::string runtime = library.toUtf8().toStdString();
        auto flag = std::make_shared<std::atomic_bool>(false);
        std::vector<ac::Frame> green;
        for (int i = 0; i < 5; ++i) green.push_back(solid(uint8_t(35 + i), 190, 60, uint64_t(i + 1)));
        const auto tuned = ac::analyze(green, {}, ac::AnalysisMode::Auto, flag, modules, runtime);
        require(tuned.success && tuned.method == "color", "green tuning failed: " + tuned.message);
        require(tuned.plate.rgba.empty(), "color analysis unexpectedly returned a plate");
        require(tuned.strings.at("key_color_type") == "custom", "key type missing");
        const auto expected = reference(directory, library);
        require(expected.size() == tuned.numbers.size() + tuned.strings.size(), "reference setting count differs");
        for (const auto &[key, value] : tuned.numbers) {
            const double original = std::stod(expected.at(key));
            require(std::abs(value - original) <= 1e-12 * std::max(1.0, std::abs(original)), "original Lua numeric parity failed: " + key);
        }
        for (const auto &[key, value] : tuned.strings)
            require(value == expected.at(key), "original Lua string parity failed: " + key);

        for (const auto color : {solid(220, 30, 200), solid(245, 245, 245)}) {
            const auto paper = ac::analyze(std::vector<ac::Frame>(5, color), {}, ac::AnalysisMode::Auto, flag, modules, runtime);
            require(paper.success, "paper analysis failed: " + paper.message);
            if (paper.method == "plate") require(paper.plate.valid(), "paper plate missing");
            else require(paper.method == "color", "unexpected paper method");
        }
        const auto white = ac::analyze(std::vector<ac::Frame>(5, solid(245,245,245)), {}, ac::AnalysisMode::Auto, flag, modules, runtime);
        require(white.method == "plate", "white background must use background-plate method");
        const auto cropped = ac::analyze(std::vector<ac::Frame>(5, solid(245,245,245)), {16,16,16,16}, ac::AnalysisMode::Auto, flag, modules, runtime);
        require(cropped.success && cropped.plate.valid(), "cropped automatic plate failed: " + cropped.message);
        require(cropped.plate.rgba[3] == 0, "excluded automatic-plate border must remain protected");

        auto invalid = ac::analyze(green, {96,0,0,0}, ac::AnalysisMode::Auto, flag, modules, runtime);
        require(!invalid.success && !invalid.cancelled, "invalid ROI accepted");
        invalid = ac::analyze(green, {-1,0,0,0}, ac::AnalysisMode::Auto, flag, modules, runtime);
        require(!invalid.success, "negative ROI accepted");
        auto mismatched = green;
        mismatched.back().rgba.pop_back();
        require(!ac::analyze(mismatched, {}, ac::AnalysisMode::Auto, flag, modules, runtime).success, "malformed frame accepted");
        require(!ac::analyze({}, {}, ac::AnalysisMode::Auto, flag, modules, runtime).success, "empty frame input accepted");

        flag->store(true);
        const auto cancelled = ac::analyze(green, {}, ac::AnalysisMode::Auto, flag, modules, runtime);
        require(cancelled.cancelled && !cancelled.success, "pre-cancelled job executed");
        flag->store(false);
        const auto manual = ac::analyze(std::vector<ac::Frame>(30, solid(120,160,190)), {}, ac::AnalysisMode::Plate, flag, modules, runtime);
        require(manual.success && manual.plate.valid(), "manual plate failed: " + manual.message);
        require(manual.strings.at("plate_kind") == "manual", "manual plate kind lost");
        for (size_t i = 0; i < manual.plate.rgba.size(); i += 4) {
            require(manual.plate.rgba[i] == 120 && manual.plate.rgba[i+1] == 160 && manual.plate.rgba[i+2] == 190, "manual plate changed constant RGB");
            require(manual.plate.rgba[i+3] == 32, "sigma alpha was not preserved as raw noise byte");
        }
        require(manual.numbers.at("sigma_scale") == 16.0 && manual.numbers.at("tau_cd") == 3.0, "plate uniforms differ from original constants");
        require(!ac::analyze(green, {}, ac::AnalysisMode::Auto, flag, modules, "relative-lua51.dll").success, "relative DLL lookup accepted");
        std::cout << "PASS: original Lua parity, green/paper paths, ROI protection/rejection, cancellation, raw sigma, malformed input\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
