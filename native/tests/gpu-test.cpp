// Headless integration test of the actual filter on a real D3D11 device.
// SunjooAn — OBS Auto Chroma 0.4.0-dev, GPL-2.0-or-later.
#include "filter.hpp"
#include <obs-module.h>
#include <graphics/vec4.h>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

static obs_module_t *testModule = nullptr;
// filter.cpp uses the loaded module only for its data path, with no OBS frontend or dock.
extern "C" obs_module_t *obs_current_module(void) { return testModule; }

namespace {
constexpr uint32_t W = 128, H = 96;
int failures = 0;

void check(bool passed, const char *name)
{
    std::cout << (passed ? "PASS " : "FAIL ") << name << std::endl;
    if (!passed) ++failures;
}

void logHandler(int level, const char *format, va_list args, void *)
{
    if (level > LOG_WARNING) return;
    char text[4096];
    vsnprintf(text, sizeof(text), format, args);
    std::cerr << "OBS: " << text << std::endl;
}

struct Synthetic {
    gs_texture_t *texture = nullptr;
};

void *createSynthetic(obs_data_t *settings, obs_source_t *)
{
    std::vector<uint8_t> pixels(W * H * 4);
    const bool translucent = obs_data_get_bool(settings, "translucent");
    const bool precisionPattern = obs_data_get_bool(settings, "precision_pattern");
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            auto *p = pixels.data() + (y * W + x) * 4;
            p[0] = x < W / 2 ? 30 : 180;
            p[1] = x < W / 2 ? 180 : 70;
            p[2] = x < W / 2 ? 60 : 70;
            if (precisionPattern && x < W / 2 && y >= H / 2) {
                p[0] = 140; p[1] = 230; p[2] = 130;
            }
            // An opaque neutral foreground patch with green spill, distinct from
            // both background samples, makes the Despill test observable in RGB.
            if (precisionPattern && x >= 88 && x < 104 && y >= 24 && y < 40) {
                p[0] = 160; p[1] = 200; p[2] = 160;
            }
            p[3] = translucent ? 128 : 255;
        }
    auto *source = new Synthetic;
    const uint8_t *data = pixels.data();
    obs_enter_graphics();
    source->texture = gs_texture_create(W, H, GS_RGBA, 1, &data, 0);
    obs_leave_graphics();
    return source;
}

void registerSynthetic()
{
    obs_source_info info{};
    info.id = "ac-gpu-test-pattern";
    info.type = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
    info.get_name = [](void *) { return "GPU test pattern · SunjooAn 0.4.0-dev"; };
    info.create = createSynthetic;
    info.destroy = [](void *data) {
        auto *s = static_cast<Synthetic *>(data);
        obs_enter_graphics();
        if (s->texture) gs_texture_destroy(s->texture);
        obs_leave_graphics();
        delete s;
    };
    info.get_width = [](void *) { return W; };
    info.get_height = [](void *) { return H; };
    info.video_render = [](void *data, gs_effect_t *) {
        obs_source_draw(static_cast<Synthetic *>(data)->texture, 0, 0, W, H, false);
    };
    obs_register_source(&info);
}

class Harness {
public:
    obs_source_t *source = nullptr, *filter = nullptr;
    gs_texrender_t *target = nullptr;
    gs_stagesurf_t *stage = nullptr;
    ac::Frame output;
    Harness(bool translucent = false, bool precisionPattern = false)
    {
        auto *settings = obs_data_create();
        obs_data_set_bool(settings, "translucent", translucent);
        obs_data_set_bool(settings, "precision_pattern", precisionPattern);
        source = obs_source_create_private("ac-gpu-test-pattern", "AC GPU test source", settings);
        obs_data_release(settings);
        if (!source) throw std::runtime_error("Synthetic source creation failed");
        obs_source_inc_showing(source);
        obs_enter_graphics();
        target = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
        stage = gs_stagesurface_create(W, H, GS_RGBA);
        obs_leave_graphics();
        if (!target || !stage) throw std::runtime_error("GPU readback creation failed");
    }
    ~Harness()
    {
        if (filter) {
            ac::requestCapture(filter, false);
            obs_source_filter_remove(source, filter);
            obs_source_release(filter);
        }
        obs_source_dec_showing(source);
        obs_source_release(source);
        obs_enter_graphics();
        if (stage) gs_stagesurface_destroy(stage);
        if (target) gs_texrender_destroy(target);
        obs_leave_graphics();
    }
    void addFilter()
    {
        filter = obs_source_create_private(ac::FilterId, "AC GPU test filter", nullptr);
        if (!filter || !obs_obj_get_data(filter)) throw std::runtime_error("Native filter creation failed");
        obs_source_filter_add(source, filter);
        ac::requestCapture(filter, true);
    }
    void draw()
    {
        obs_enter_graphics();
        const bool previousLinear = gs_set_linear_srgb(true);
        const bool previousSrgb = gs_framebuffer_srgb_enabled();
        gs_texrender_reset(target);
        if (!gs_texrender_begin(target, W, H)) {
            obs_leave_graphics();
            throw std::runtime_error("Test render begin failed");
        }
        vec4 clear{};
        gs_clear(GS_CLEAR_COLOR, &clear, 0, 0);
        gs_ortho(0, W, 0, H, -100, 100);
        gs_blend_state_push();
        gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA,
                                   GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
        obs_source_video_render(source);
        gs_blend_state_pop();
        gs_texrender_end(target);
        gs_stage_texture(stage, gs_texrender_get_texture(target));
        uint8_t *p = nullptr;
        uint32_t pitch = 0;
        const bool mapped = gs_stagesurface_map(stage, &p, &pitch);
        if (mapped) {
            output.width = W; output.height = H; output.timestamp = obs_get_video_frame_time();
            output.rgba.resize(W * H * 4);
            for (uint32_t y = 0; y < H; ++y)
                std::memcpy(output.rgba.data() + size_t(y) * W * 4, p + size_t(y) * pitch, W * 4);
            gs_stagesurface_unmap(stage);
        }
        gs_enable_framebuffer_srgb(previousSrgb);
        gs_set_linear_srgb(previousLinear);
        obs_leave_graphics();
        if (!mapped) throw std::runtime_error("Test output readback failed");
    }
    ac::Preview wait(uint64_t after = 0)
    {
        for (int i = 0; i < 100; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            draw();
            auto p = ac::readPreview(filter);
            if (p.original.valid() && p.processed.valid() && p.original.timestamp > after) return p;
        }
        throw std::runtime_error("Preview timed out: " + ac::readPreview(filter).error);
    }
};

const uint8_t *pixel(const ac::Frame &f, uint32_t x, uint32_t y = H / 2)
{
    return f.rgba.data() + (size_t(y) * f.width + x) * 4;
}
bool nearByte(int a, int b) { return std::abs(a - b) <= 2; }
bool colorAt(const ac::Frame &f, uint32_t x, int r, int g, int b, int a)
{
    if (!f.valid()) return false;
    const auto *p = pixel(f, x);
    return nearByte(p[0], r) && nearByte(p[1], g) && nearByte(p[2], b) && nearByte(p[3], a);
}
bool alphaAt(const ac::Frame &f, uint32_t x, int alpha)
{
    return f.valid() && nearByte(pixel(f, x)[3], alpha);
}
bool equalFrames(const ac::Frame &a, const ac::Frame &b)
{
    if (!a.valid() || !b.valid() || a.width != b.width || a.height != b.height) return false;
    for (size_t i = 0; i < a.rgba.size(); ++i) if (!nearByte(a.rgba[i], b.rgba[i])) return false;
    return true;
}
void setRect(obs_data_t *s, const char *prefix, double x, double y, double w, double h)
{
    for (const auto &v : {std::pair<const char *, double>{"x", x}, {"y", y}, {"w", w}, {"h", h}})
        obs_data_set_double(s, (std::string(prefix) + "_" + v.first).c_str(), v.second);
}

QJsonObject stroke(int r, int g, int b, bool subtract = false, bool local = false)
{
    QJsonArray samples, points;
    // A single QJsonArray nested with braces can select its copy constructor,
    // flattening the fixture. append explicitly preserves the JSON dimensions.
    samples.append(QJsonArray{r, g, b});
    points.append(QJsonArray{0.125, 0.75});
    return {{"subtract", subtract}, {"enabled", true}, {"local", local},
            {"rect", QJsonArray{0.0, 0.5, 0.25, 0.5}},
            {"samples", samples}, {"points", points}};
}
void setStrokes(obs_data_t *settings, const QJsonArray &strokes)
{
    for (const auto &entry : strokes) {
        const auto fixture = entry.toObject();
        const auto samples = fixture.value("samples").toArray();
        const auto points = fixture.value("points").toArray();
        if (samples.isEmpty() || !samples[0].isArray() || samples[0].toArray().size() != 3
            || points.isEmpty() || !points[0].isArray() || points[0].toArray().size() != 2)
            throw std::runtime_error("GPU stroke fixture must contain nested RGB and XY arrays");
    }
    const auto json = QJsonDocument(strokes).toJson(QJsonDocument::Compact);
    obs_data_set_string(settings, "precision_strokes", json.constData());
}
bool equalAlpha(const ac::Frame &a, const ac::Frame &b)
{
    if (!a.valid() || !b.valid() || a.width != b.width || a.height != b.height) return false;
    for (size_t i = 3; i < a.rgba.size(); i += 4)
        if (!nearByte(a.rgba[i], b.rgba[i])) return false;
    return true;
}
void runPrecisionTests()
{
    Harness test(false, true);
    test.draw();
    const auto original = test.output;
    test.addFilter();
    auto *settings = obs_source_get_settings(test.filter);
    obs_data_set_string(settings, "method", "precision");
    obs_data_set_int(settings, "key_color", 30 | (180 << 8) | (60 << 16));
    obs_data_set_int(settings, "similarity", 40);
    obs_data_set_int(settings, "smoothness", 35);
    obs_data_set_double(settings, "precision_tolerance", .025);
    obs_data_set_double(settings, "precision_softness", .06);
    obs_data_set_double(settings, "precision_feather", .1);
    obs_data_set_double(settings, "despill_strength", 1);
    setStrokes(settings, {stroke(140, 230, 130)});
    obs_source_update(test.filter, settings);
    auto p = test.wait();
    check(p.error.empty() && pixel(p.processed, 16, 24)[3] <= 2
          && pixel(p.processed, 16, 72)[3] <= 2 && alphaAt(p.processed, 112, 255),
          "precision multiple background colors key independently while foreground stays opaque");

    setStrokes(settings, {stroke(140, 230, 130), stroke(140, 230, 130, true)});
    obs_source_update(test.filter, settings);
    p = test.wait();
    check(p.error.empty() && pixel(p.processed, 16, 24)[3] <= 2 && pixel(p.processed, 16, 72)[3] >= 253,
          "subtract stroke excludes that color from background selection");

    setStrokes(settings, {stroke(140, 230, 130, false, true)});
    obs_source_update(test.filter, settings);
    p = test.wait();
    check(p.error.empty() && pixel(p.processed, 16, 72)[3] <= 2 && pixel(p.processed, 48, 72)[3] >= 253,
          "local stroke changes its rectangle and preserves identical color outside it");
    check(pixel(p.processed, 30, 72)[3] > 2 && pixel(p.processed, 30, 72)[3] < 253,
          "local correction rectangle feathers rather than cutting a hard edge");

    auto scoped = stroke(140, 230, 130, false, true);
    scoped["region_points"] = QJsonArray{QJsonArray{.04, .52}, QJsonArray{.49, .52}, QJsonArray{.04, .98}};
    setStrokes(settings, {scoped});
    obs_data_set_double(settings, "precision_feather", .02);
    obs_source_update(test.filter, settings);
    p = test.wait();
    check(p.error.empty() && pixel(p.processed, 16, 72)[3] <= 2 && pixel(p.processed, 48, 72)[3] >= 253,
          "local polygon keys inside and preserves the same color outside its outline but inside its bounding box");
    obs_data_set_double(settings, "precision_feather", .1);

    setStrokes(settings, {stroke(140, 230, 130)});
    obs_data_set_double(settings, "despill_strength", 0);
    obs_source_update(test.filter, settings);
    const auto noDespill = test.wait().processed;
    obs_data_set_double(settings, "despill_strength", 1);
    obs_source_update(test.filter, settings);
    p = test.wait();
    check(equalAlpha(noDespill, p.processed), "precision Despill changes color independently from alpha");
    const auto *spillBefore = pixel(noDespill, 96, 32);
    const auto *spillAfter = pixel(p.processed, 96, 32);
    check(spillBefore[3] >= 253 && spillAfter[3] >= 253
          && spillBefore[1] > spillAfter[1] + 5
          && (int(spillBefore[1]) - spillBefore[0]) > (int(spillAfter[1]) - spillAfter[0]) + 5,
          "precision Despill visibly reduces green spill on retained opaque foreground RGB");

    ac::requestCapture(test.filter, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    test.draw();
    check(pixel(test.output, 16, 72)[3] <= 2 && alphaAt(test.output, 112, 255),
          "precision committed render still keys with dock capture disabled");
    ac::requestCapture(test.filter, true);

    obs_data_set_string(settings, "precision_strokes", "[{invalid-json");
    obs_source_update(test.filter, settings);
    p = test.wait();
    check(!p.error.empty() && equalFrames(test.output, original),
          "malformed stroke JSON reports an error and preserves original GPU output");
    obs_data_release(settings);

    Harness translucent(true);
    translucent.addFilter();
    auto *partial = obs_source_get_settings(translucent.filter);
    obs_data_set_string(partial, "method", "precision");
    obs_data_set_int(partial, "key_color", 30 | (180 << 8) | (60 << 16));
    obs_source_update(translucent.filter, partial);
    const auto transparent = translucent.wait();
    check(alphaAt(transparent.processed, 112, 128) && pixel(transparent.processed, 16)[3] <= 2,
          "precision preserves incoming partial foreground alpha while removing the background");
    obs_data_release(partial);
}

void runTests(const QString &directory)
{
    Harness test;
    test.draw();
    const auto baseline = test.output;
    test.addFilter();
    auto p = test.wait();
    check(equalFrames(test.output, baseline), "passthrough preserves committed GPU output");
    check(colorAt(p.original, 16, 30, 180, 60, 255) && colorAt(p.original, 112, 180, 70, 70, 255),
          "capture returns original straight sRGB RGBA8 without gamma shift");
    check(equalFrames(p.original, p.processed), "default preview equals original");

    auto *settings = obs_source_get_settings(test.filter);
    obs_data_set_string(settings, "method", "color");
    obs_data_set_int(settings, "key_color", 30 | (180 << 8) | (60 << 16));
    obs_data_set_int(settings, "similarity", 80);
    obs_data_set_int(settings, "smoothness", 80);
    // The dock serializes user values; defaults are not present in this JSON.
    auto *serializedCandidate = obs_data_create_from_json(obs_data_get_json(settings));
    ac::setPreviewSettings(test.filter, serializedCandidate);
    obs_data_release(serializedCandidate);
    p = test.wait();
    check(alphaAt(p.processed, 16, 0) && alphaAt(p.processed, 112, 255), "candidate removes background and keeps foreground");
    check(equalFrames(test.output, baseline), "candidate preview never changes committed output");
    const auto firstTime = p.original.timestamp;
    p = test.wait(firstTime);
    check(p.original.timestamp > firstTime && p.original.timestamp == p.processed.timestamp,
          "captured original and result share increasing output-frame timestamps");

    obs_source_update(test.filter, settings);
    ac::setPreviewSettings(test.filter, nullptr);
    p = test.wait();
    check(alphaAt(test.output, 16, 0) && alphaAt(test.output, 112, 255), "apply changes committed color-key output");
    check(colorAt(p.original, 16, 30, 180, 60, 255), "original capture stays unkeyed after apply");

    setRect(settings, "protect", 0, 0, .5, 1);
    setRect(settings, "erase", 0, 0, 1, 1);
    obs_source_update(test.filter, settings);
    p = test.wait();
    check(colorAt(p.processed, 16, 30, 180, 60, 255) && alphaAt(p.processed, 112, 0),
          "native protection wins overlap and erase makes the remaining area transparent");

    obs_data_set_string(settings, "protect_points", "[[0.05,0.1],[0.45,0.1],[0.05,0.9]]");
    obs_data_set_string(settings, "erase_points", "[[0.1,0.1],[0.98,0.1],[0.98,0.9]]");
    obs_source_update(test.filter, settings);
    p = test.wait();
    check(p.error.empty() && pixel(p.processed, 16, 24)[3] >= 253 && pixel(p.processed, 48, 72)[3] <= 2 &&
          pixel(p.processed, 112, 24)[3] <= 2 && pixel(p.processed, 112, 80)[3] >= 253,
          "hard protect and erase obey polygon outlines rather than bounding boxes");
    obs_data_set_string(settings, "protect_points", "");
    obs_data_set_string(settings, "erase_points", "");

    setRect(settings, "protect", 0, 0, 0, 0);
    setRect(settings, "erase", 0, 0, 0, 0);
    obs_data_set_string(settings, "method", "plate");
    obs_data_set_string(settings, "plate_path", "missing-ac-gpu-test-plate.png");
    obs_source_update(test.filter, settings);
    p = test.wait();
    check(equalFrames(test.output, baseline) && !p.error.empty(), "missing plate reports an error and preserves original output");

    QImage plate(W, H, QImage::Format_RGBA8888);
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            auto *b = plate.scanLine(int(y)) + x * 4;
            b[0] = 30; b[1] = 180; b[2] = 60; b[3] = 32;
        }
    const auto path = directory + "/한글-배경판.png";
    if (!plate.save(path, "PNG")) throw std::runtime_error("Test PNG save failed");
    obs_data_set_string(settings, "plate_path", path.toUtf8().constData());
    obs_data_set_string(settings, "plate_kind", "manual");
    obs_source_update(test.filter, settings);
    p = test.wait();
    check(p.error.empty() && alphaAt(p.processed, 16, 0) && alphaAt(p.processed, 112, 255),
          "Unicode PNG plate loads with sigma alpha intact and keys the background");

    obs_data_set_bool(settings, "despeck", true);
    obs_source_update(test.filter, settings);
    p = test.wait();
    check(p.error.empty() && alphaAt(p.processed, 16, 0) && alphaAt(p.processed, 112, 255),
          "manual-plate multipass despeck preserves a large foreground");

    const auto wrongPath = directory + "/wrong-size.png";
    plate.scaled(32, 32).save(wrongPath, "PNG");
    obs_data_set_string(settings, "plate_path", wrongPath.toUtf8().constData());
    obs_source_update(test.filter, settings);
    p = test.wait();
    check(equalFrames(test.output, baseline) && !p.error.empty(), "plate size mismatch preserves original and exposes an error");

    obs_data_set_string(settings, "method", "color");
    obs_data_set_bool(settings, "despeck", false);
    obs_source_update(test.filter, settings);
    ac::requestCapture(test.filter, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    test.draw();
    check(alphaAt(test.output, 16, 0) && alphaAt(test.output, 112, 255), "closed-preview one-pass path still keys correctly");
    obs_source_set_enabled(test.filter, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    test.draw();
    check(equalFrames(test.output, baseline), "disabled native filter returns the original");
    obs_data_release(settings);

    Harness translucent(true);
    translucent.draw();
    const auto transparentBaseline = translucent.output;
    translucent.addFilter();
    p = translucent.wait();
    if (!equalFrames(translucent.output, transparentBaseline)) {
        const auto *before = pixel(transparentBaseline, 16);
        const auto *after = pixel(translucent.output, 16);
        std::cout << "Partial-alpha baseline/filtered:";
        for (int i = 0; i < 4; ++i) std::cout << " " << int(before[i]) << "/" << int(after[i]);
        std::cout << std::endl;
    }
    check(equalFrames(translucent.output, transparentBaseline), "passthrough preserves a partially transparent input");
    check(alphaAt(p.original, 16, 128) && alphaAt(p.processed, 16, 128), "readback retains partial input alpha");
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 5) {
        std::cerr << "Usage: ac-gpu-test <plugin-dll> <plugin-data> <libobs-data> <Qt-bin>" << std::endl;
        return 2;
    }
    QTemporaryDir temporary;
    if (!temporary.isValid()) return 2;
    // Loading the copy leaves the development DLL available for concurrent rebuilds.
    const auto moduleCopy = temporary.path() + "/gpu-module.dll";
    if (!QFile::copy(QString::fromUtf8(argv[1]), moduleCopy)) return 2;
    base_set_log_handler(logHandler, nullptr);
    if (!obs_startup("ko-KR", temporary.path().toUtf8().constData(), nullptr)) return 2;
    const auto libobsPath = (QDir::fromNativeSeparators(QString::fromUtf8(argv[3])) + "/").toUtf8();
    obs_add_data_path(libobsPath.constData());
    obs_video_info video{};
    const auto graphicsPath = QDir(QString::fromUtf8(argv[3]))
        .absoluteFilePath("../../bin/64bit/libobs-d3d11.dll").toUtf8();
#ifdef _WIN32
    // libobs intentionally uses LOAD_LIBRARY_SEARCH_DEFAULT_DIRS, which excludes PATH.
    const auto runtimeBin = QFileInfo(QString::fromUtf8(graphicsPath)).absolutePath();
    AddDllDirectory(reinterpret_cast<const wchar_t *>(runtimeBin.utf16()));
    const auto qtBin = QString::fromUtf8(argv[4]);
    AddDllDirectory(reinterpret_cast<const wchar_t *>(qtBin.utf16()));
#endif
    video.graphics_module = graphicsPath.constData();
    video.fps_num = 30; video.fps_den = 1;
    video.base_width = video.output_width = W;
    video.base_height = video.output_height = H;
    video.output_format = VIDEO_FORMAT_RGBA;
    video.colorspace = VIDEO_CS_709;
    video.range = VIDEO_RANGE_FULL;
    video.scale_type = OBS_SCALE_BICUBIC;
    const auto reset = obs_reset_video(&video);
    if (reset != OBS_VIDEO_SUCCESS) {
        std::cerr << "Video reset failed: " << reset << std::endl;
        obs_shutdown();
        return 2;
    }
    if (obs_open_module(&testModule, moduleCopy.toUtf8().constData(), argv[2]) != MODULE_SUCCESS) {
        std::cerr << "Module open failed" << std::endl;
        obs_shutdown();
        return 2;
    }
    ac::registerFilter();
    registerSynthetic();
    try { runTests(temporary.path()); runPrecisionTests(); }
    catch (const std::exception &error) { std::cerr << "FAIL " << error.what() << std::endl; ++failures; }
    obs_shutdown();
    testModule = nullptr;
    std::cout << "GPU integration failures: " << failures << std::endl;
    return failures ? 1 : 0;
}

