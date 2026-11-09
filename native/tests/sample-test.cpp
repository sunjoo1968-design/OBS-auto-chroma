// Actual D3D11 sample-image regression and evidence capture.
// SunjooAn — OBS Auto Chroma 0.5.0-dev, GPL-2.0-or-later.
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
#include <cmath>
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
extern "C" obs_module_t *obs_current_module(void) { return testModule; }

namespace {
QImage inputImage;
int failures = 0;
void check(bool passed, const char *name)
{
    std::cout << (passed ? "PASS " : "FAIL ") << name << std::endl;
    if (!passed) ++failures;
}
void logHandler(int level, const char *format, va_list args, void *)
{
    if (level > LOG_WARNING) return;
    char text[4096]; vsnprintf(text, sizeof(text), format, args);
    std::cerr << "OBS: " << text << std::endl;
}
struct ImageSource { gs_texture_t *texture = nullptr; };
void registerImageSource()
{
    obs_source_info info{};
    info.id = "ac-sample-test-image";
    info.type = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
    info.get_name = [](void *) { return "Sample validation · SunjooAn 0.5.0-dev"; };
    info.create = [](obs_data_t *, obs_source_t *) -> void * {
        auto *source = new ImageSource;
        const uint8_t *bytes = inputImage.constBits();
        obs_enter_graphics();
        source->texture = gs_texture_create(uint32_t(inputImage.width()), uint32_t(inputImage.height()),
                                            GS_RGBA, 1, &bytes, 0);
        obs_leave_graphics();
        if (!source->texture) { delete source; return nullptr; }
        return source;
    };
    info.destroy = [](void *opaque) {
        auto *source = static_cast<ImageSource *>(opaque);
        obs_enter_graphics(); gs_texture_destroy(source->texture); obs_leave_graphics();
        delete source;
    };
    info.get_width = [](void *) { return uint32_t(inputImage.width()); };
    info.get_height = [](void *) { return uint32_t(inputImage.height()); };
    info.video_render = [](void *opaque, gs_effect_t *) {
        obs_source_draw(static_cast<ImageSource *>(opaque)->texture, 0, 0,
                        uint32_t(inputImage.width()), uint32_t(inputImage.height()), false);
    };
    obs_register_source(&info);
}
class Harness {
public:
    obs_source_t *source = nullptr, *filter = nullptr;
    gs_texrender_t *target = nullptr;
    explicit Harness()
    {
        source = obs_source_create_private("ac-sample-test-image", "Sample validation image", nullptr);
        if (!source || !obs_obj_get_data(source)) throw std::runtime_error("Sample source creation failed");
        obs_source_inc_showing(source);
        filter = obs_source_create_private(ac::FilterId, "Sample validation filter", nullptr);
        if (!filter || !obs_obj_get_data(filter)) throw std::runtime_error("Sample filter creation failed");
        obs_source_filter_add(source, filter);
        ac::requestCapture(filter, true);
        obs_enter_graphics(); target = gs_texrender_create(GS_RGBA, GS_ZS_NONE); obs_leave_graphics();
        if (!target) throw std::runtime_error("Sample render target creation failed");
    }
    ~Harness()
    {
        ac::requestCapture(filter, false);
        obs_source_filter_remove(source, filter); obs_source_release(filter);
        obs_source_dec_showing(source); obs_source_release(source);
        obs_enter_graphics(); gs_texrender_destroy(target); obs_leave_graphics();
    }
    void draw()
    {
        obs_enter_graphics();
        const bool previousLinear = gs_set_linear_srgb(true);
        const bool previousSrgb = gs_framebuffer_srgb_enabled();
        gs_texrender_reset(target);
        if (!gs_texrender_begin(target, uint32_t(inputImage.width()), uint32_t(inputImage.height()))) {
            obs_leave_graphics(); throw std::runtime_error("Sample render begin failed");
        }
        vec4 clear{}; gs_clear(GS_CLEAR_COLOR, &clear, 0, 0);
        gs_ortho(0, float(inputImage.width()), 0, float(inputImage.height()), -100, 100);
        gs_blend_state_push();
        gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
        obs_source_video_render(source);
        gs_blend_state_pop(); gs_texrender_end(target);
        gs_enable_framebuffer_srgb(previousSrgb); gs_set_linear_srgb(previousLinear);
        obs_leave_graphics();
    }
    ac::Preview render(obs_data_t *settings)
    {
        // Clear old staged data so the returned pair belongs to this exact candidate.
        ac::requestCapture(filter, false);
        obs_source_update(filter, settings);
        ac::setPreviewSettings(filter, nullptr);
        ac::requestCapture(filter, true);
        for (int i = 0; i < 100; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            draw();
            auto p = ac::readPreview(filter);
            if (p.original.valid() && p.processed.valid()) {
                if (!p.error.empty()) throw std::runtime_error(p.error);
                return p;
            }
        }
        throw std::runtime_error("Sample preview timed out: " + ac::readPreview(filter).error);
    }
};

QImage image(const ac::Frame &frame)
{
    if (!frame.valid()) throw std::runtime_error("Invalid sample frame");
    return QImage(frame.rgba.data(), int(frame.width), int(frame.height), int(frame.width * 4),
                  QImage::Format_RGBA8888).copy();
}
uint8_t compositeChannel(uint8_t channel, uint8_t alpha, bool white)
{
    if (alpha == 255) return channel;
    if (alpha == 0) return white ? 255 : 0;
    const double s = channel / 255.0;
    const double linear = s <= .04045 ? s / 12.92 : std::pow((s + .055) / 1.055, 2.4);
    const double a = alpha / 255.0;
    const double mixed = linear * a + (white ? 1.0 - a : 0.0);
    const double encoded = mixed <= .0031308 ? mixed * 12.92 : 1.055 * std::pow(mixed, 1.0 / 2.4) - .055;
    return uint8_t(std::clamp(std::round(encoded * 255.0), 0.0, 255.0));
}
void saveEvidence(const ac::Frame &frame, const QString &directory, const QString &name)
{
    if (!image(frame).save(directory + "/" + name + ".png", "PNG"))
        throw std::runtime_error("Sample RGBA evidence save failed");
    QImage alpha(int(frame.width), int(frame.height), QImage::Format_Grayscale8);
    QImage black(int(frame.width), int(frame.height), QImage::Format_RGB888);
    QImage white(int(frame.width), int(frame.height), QImage::Format_RGB888);
    for (uint32_t y = 0; y < frame.height; ++y) {
        auto *a = alpha.scanLine(int(y)); auto *b = black.scanLine(int(y)); auto *w = white.scanLine(int(y));
        for (uint32_t x = 0; x < frame.width; ++x) {
            const auto *p = frame.rgba.data() + (size_t(y) * frame.width + x) * 4;
            a[x] = p[3];
            for (int c = 0; c < 3; ++c) {
                // Recompose the captured straight RGBA in linear sRGB, then encode for display.
                b[x * 3 + c] = compositeChannel(p[c], p[3], false);
                w[x * 3 + c] = compositeChannel(p[c], p[3], true);
            }
        }
    }
    if (!alpha.save(directory + "/" + name + "-alpha.png")
        || !black.save(directory + "/" + name + "-black.png")
        || !white.save(directory + "/" + name + "-white.png"))
        throw std::runtime_error("Sample matte/composite evidence save failed");
}
QJsonObject alphaMetrics(const ac::Frame &frame, int x0, int y0, int x1, int y1)
{
    std::vector<int> alpha;
    double sum = 0;
    int nonzero = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const int a = frame.rgba[(size_t(y) * frame.width + x) * 4 + 3];
            alpha.push_back(a); sum += a; if (a > 5) ++nonzero;
        }
    std::sort(alpha.begin(), alpha.end());
    return {{"roi", QJsonArray{x0, y0, x1, y1}}, {"mean_alpha", sum / alpha.size() / 255.0},
            {"p95_alpha", alpha[size_t((alpha.size() - 1) * .95)] / 255.0},
            {"alpha_gt_5_percent", 100.0 * nonzero / alpha.size()}};
}
bool equalAlpha(const ac::Frame &a, const ac::Frame &b)
{
    if (a.width != b.width || a.height != b.height || !a.valid() || !b.valid()) return false;
    for (size_t i = 3; i < a.rgba.size(); i += 4) if (a.rgba[i] != b.rgba[i]) return false;
    return true;
}
QJsonObject sampleStroke(bool local)
{
    QJsonArray samples, points;
    // Only the unobstructed right corner is sampled; no person/hair pixels are included.
    for (int y = 960; y <= 1060; y += 20)
        for (int x = 1790; x <= 1910; x += 20) {
            const auto *p = inputImage.constScanLine(y) + x * 4;
            samples.append(QJsonArray{p[0], p[1], p[2]});
            points.append(QJsonArray{double(x) / inputImage.width(), double(y) / inputImage.height()});
        }
    return {{"subtract", false}, {"enabled", true}, {"local", local},
            {"rect", QJsonArray{.88, .83, .12, .17}}, {"samples", samples}, {"points", points}};
}
void runTests(const QString &outputDirectory)
{
    if (inputImage.width() != 1920 || inputImage.height() != 1080)
        throw std::runtime_error("This sample regression expects the supplied 1920x1080 fixture");
    Harness test;
    auto *settings = obs_source_get_settings(test.filter);
    obs_data_set_string(settings, "method", "color");
    obs_data_set_int(settings, "key_color", 4278255384LL);
    obs_data_set_int(settings, "similarity", 65);
    obs_data_set_int(settings, "smoothness", 85);
    obs_data_set_double(settings, "despill_strength", 1);
    const auto baseline = test.render(settings);
    saveEvidence(baseline.original, outputDirectory, "original");
    saveEvidence(baseline.processed, outputDirectory, "baseline");

    obs_data_set_string(settings, "method", "precision");
    obs_data_set_double(settings, "precision_tolerance", .025);
    obs_data_set_double(settings, "precision_softness", .06);
    obs_data_set_double(settings, "precision_edge", .5);
    obs_data_set_double(settings, "precision_feather", .02);
    const auto strokeJson = QJsonDocument(QJsonArray{sampleStroke(false)}).toJson(QJsonDocument::Compact);
    obs_data_set_string(settings, "precision_strokes", strokeJson.constData());
    const auto precise = test.render(settings);
    saveEvidence(precise.processed, outputDirectory, "precision");

    obs_data_set_double(settings, "despill_strength", 0);
    const auto noDespill = test.render(settings);
    saveEvidence(noDespill.processed, outputDirectory, "precision-despill0");
    obs_data_release(settings);

    const auto cornerBefore = alphaMetrics(baseline.processed, 1800, 950, 1920, 1080);
    const auto cornerAfter = alphaMetrics(precise.processed, 1800, 950, 1920, 1080);
    const auto face = alphaMetrics(precise.processed, 1200, 220, 1350, 350);
    const auto shirt = alphaMetrics(precise.processed, 1200, 700, 1400, 850);
    const double before = cornerBefore.value("mean_alpha").toDouble();
    const double after = cornerAfter.value("mean_alpha").toDouble();
    check(before > .05, "supplied sample reproduces lower-right alpha residue in legacy mode");
    check(after < .05 && after < before * .35, "stroke precision mode reduces lower-right actual alpha residue");
    check(face.value("mean_alpha").toDouble() > .98 && shirt.value("mean_alpha").toDouble() > .98,
          "precision correction preserves opaque face and white shirt interiors");
    check(equalAlpha(precise.processed, noDespill.processed), "sample Despill slider does not change actual alpha");
    QJsonObject report{{"author", "SunjooAn"}, {"version", ac::Version},
        {"measurement", "Actual GPU straight RGBA8 alpha, not black RGB residue"},
        {"evidence_composites", "CPU linear-sRGB composite of the actual GPU RGBA capture"},
        {"corner_baseline", cornerBefore}, {"corner_precision", cornerAfter},
        {"face_precision", face}, {"shirt_precision", shirt},
        {"despill_alpha_unchanged", equalAlpha(precise.processed, noDespill.processed)},
        {"hair_ground_truth", "unavailable; inspect exported RGBA/black/white evidence, no accuracy percentage claimed"},
        {"precision_strokes", QString::fromUtf8(strokeJson)}, {"failures", failures}};
    QFile file(outputDirectory + "/metrics.json");
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(report).toJson(QJsonDocument::Indented)) < 0)
        throw std::runtime_error("Sample metrics save failed");
    std::cout << "Corner alpha baseline=" << before << " precision=" << after << std::endl;
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 7) {
        std::cerr << "Usage: ac-sample-test <plugin-dll> <plugin-data> <libobs-data> <Qt-bin> <input-png> <output-dir>" << std::endl;
        return 2;
    }
    inputImage = QImage(QString::fromUtf8(argv[5])).convertToFormat(QImage::Format_RGBA8888);
    if (inputImage.isNull()) { std::cerr << "Sample PNG cannot be read" << std::endl; return 2; }
    const QString outputDirectory = QString::fromUtf8(argv[6]);
    if (!QDir().mkpath(outputDirectory)) return 2;
    QTemporaryDir temporary;
    if (!temporary.isValid()) return 2;
    const auto moduleCopy = temporary.path() + "/sample-module.dll";
    if (!QFile::copy(QString::fromUtf8(argv[1]), moduleCopy)) return 2;
    base_set_log_handler(logHandler, nullptr);
    if (!obs_startup("ko-KR", temporary.path().toUtf8().constData(), nullptr)) return 2;
    const auto libobsPath = (QDir::fromNativeSeparators(QString::fromUtf8(argv[3])) + "/").toUtf8();
    obs_add_data_path(libobsPath.constData());
    const auto graphicsPath = QDir(QString::fromUtf8(argv[3])).absoluteFilePath("../../bin/64bit/libobs-d3d11.dll").toUtf8();
#ifdef _WIN32
    const auto runtimeBin = QFileInfo(QString::fromUtf8(graphicsPath)).absolutePath();
    AddDllDirectory(reinterpret_cast<const wchar_t *>(runtimeBin.utf16()));
    const auto qtBin = QString::fromUtf8(argv[4]);
    AddDllDirectory(reinterpret_cast<const wchar_t *>(qtBin.utf16()));
#endif
    obs_video_info video{};
    video.graphics_module = graphicsPath.constData(); video.fps_num = 30; video.fps_den = 1;
    video.base_width = video.output_width = uint32_t(inputImage.width());
    video.base_height = video.output_height = uint32_t(inputImage.height());
    video.output_format = VIDEO_FORMAT_RGBA; video.colorspace = VIDEO_CS_709;
    video.range = VIDEO_RANGE_FULL; video.scale_type = OBS_SCALE_BICUBIC;
    if (obs_reset_video(&video) != OBS_VIDEO_SUCCESS) { obs_shutdown(); return 2; }
    if (obs_open_module(&testModule, moduleCopy.toUtf8().constData(), argv[2]) != MODULE_SUCCESS) {
        obs_shutdown(); return 2;
    }
    ac::registerFilter(); registerImageSource();
    try { runTests(outputDirectory); }
    catch (const std::exception &error) { std::cerr << "FAIL " << error.what() << std::endl; ++failures; }
    obs_shutdown(); testModule = nullptr;
    std::cout << "Sample regression failures: " << failures << std::endl;
    return failures ? 1 : 0;
}
