// OBS Auto Chroma — SunjooAn. GPL-2.0-or-later.
#include "filter.hpp"
#include "precision-model.hpp"
#include <obs-module.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

namespace ac {
namespace {
struct Config {
    std::string method = "passthrough", platePath, plateKind = "auto";
    std::string request;
    uint32_t keyColor = 0x00ff00;
    float similarity = .4f, smoothness = .08f, opacity = 1, despill = 1, rotate = 0;
    std::array<float, 9> rotation{1, 0, 0, 0, 1, 0, 0, 0, 1};
    float tauCd = 3, wCd = 1, tauLo = .55f, tauHi = 1.1f, wA = .05f;
    bool despeck = false;
    PrecisionModel precision;
    float precisionTolerance = .025f, precisionSoftness = .06f, precisionEdge = .5f, precisionFeather = .02f;
    Region protect, erase;
};

struct Shared {
    std::mutex mutex;
    Config config;
    std::optional<Config> previewConfig;
    Preview preview;
    std::string currentError;
    std::atomic_bool capture{false};
    std::atomic_bool liveCapture{false};
    std::atomic<uint64_t> epoch{1};
};

std::mutex registryMutex;
std::unordered_map<obs_source_t *, std::shared_ptr<Shared>> registry;

struct Renderer {
    gs_texture_t *plate = nullptr;
    std::string platePath;
    gs_texrender_t *keyed = nullptr, *masked = nullptr, *straight = nullptr, *veto = nullptr;
};

struct Stage {
    gs_stagesurf_t *original = nullptr, *processed = nullptr;
    uint64_t timestamp = 0;
    std::string error;
};

struct Filter {
    obs_source_t *context = nullptr;
    std::shared_ptr<Shared> shared = std::make_shared<Shared>();
    gs_effect_t *colorEffect = nullptr, *plateEffect = nullptr, *cleanEffect = nullptr, *maskEffect = nullptr;
    gs_effect_t *precisionEffect = nullptr;
    gs_texrender_t *original = nullptr, *encoded = nullptr;
    Renderer live, candidate;
    std::array<Stage, 2> stages;
    uint32_t width = 0, height = 0;
    uint64_t stageEpoch = 0, lastCapture = 0;
    size_t nextStage = 0;
    std::string loggedError;
};

std::shared_ptr<Shared> stateFor(obs_source_t *source)
{
    std::lock_guard lock(registryMutex);
    const auto it = registry.find(source);
    return it == registry.end() ? nullptr : it->second;
}

float finiteClamp(double value, float low, float high, float fallback = 0)
{
    return std::isfinite(value) ? std::clamp(float(value), low, high) : fallback;
}

Rect readRect(obs_data_t *settings, const char *prefix)
{
    const std::string p(prefix);
    Rect r;
    r.x = finiteClamp(obs_data_get_double(settings, (p + "_x").c_str()), 0, 1);
    r.y = finiteClamp(obs_data_get_double(settings, (p + "_y").c_str()), 0, 1);
    r.width = finiteClamp(obs_data_get_double(settings, (p + "_w").c_str()), 0, float(1 - r.x));
    r.height = finiteClamp(obs_data_get_double(settings, (p + "_h").c_str()), 0, float(1 - r.y));
    return r;
}

Config readConfig(obs_data_t *s)
{
    Config c;
    c.method = obs_data_get_string(s, "method");
    c.request = obs_data_get_string(s, "ac_request_id");
    c.platePath = obs_data_get_string(s, "plate_path");
    c.plateKind = obs_data_get_string(s, "plate_kind");
    c.despeck = obs_data_get_bool(s, "despeck") && c.plateKind == "manual";
    c.keyColor = uint32_t(obs_data_get_int(s, "key_color"));
    const std::string keyType = obs_data_get_string(s, "key_color_type");
    if (keyType == "green") c.keyColor = 0x00ff00;
    else if (keyType == "blue") c.keyColor = 0xff9900;
    else if (keyType == "magenta") c.keyColor = 0xff00ff;
    c.similarity = finiteClamp(obs_data_get_double(s, "similarity") / 1000., .001f, 1, .4f);
    c.smoothness = finiteClamp(obs_data_get_double(s, "smoothness") / 1000., .001f, 1, .08f);
    c.opacity = finiteClamp(obs_data_get_double(s, "opacity"), 0, 1, 1);
    c.despill = finiteClamp(obs_data_get_double(s, "despill_strength"), 0, 1, 1);
    c.rotate = finiteClamp(obs_data_get_double(s, "despill_rotate"), 0, 1);
    const char *rows[]{"rot_r", "rot_g", "rot_b"};
    for (size_t row = 0; row < 3; ++row)
        for (size_t col = 0; col < 3; ++col) {
            const auto key = std::string(rows[row]) + char('1' + col);
            c.rotation[row * 3 + col] = finiteClamp(obs_data_get_double(s, key.c_str()), -2, 2,
                                                   row == col ? 1.f : 0.f);
        }
    c.tauCd = finiteClamp(obs_data_get_double(s, "tau_cd"), .5f, 30, 3);
    c.wCd = finiteClamp(obs_data_get_double(s, "w_cd"), .001f, 30, 1);
    c.tauLo = finiteClamp(obs_data_get_double(s, "tau_lo"), .1f, 1, .55f);
    c.tauHi = finiteClamp(obs_data_get_double(s, "tau_hi"), 1, 2, 1.1f);
    c.wA = finiteClamp(obs_data_get_double(s, "w_a"), .001f, 1, .05f);
    c.protect = parseRegion(obs_data_get_string(s, "protect_points"), readRect(s, "protect"));
    c.erase = parseRegion(obs_data_get_string(s, "erase_points"), readRect(s, "erase"));
    c.precision = parsePrecisionModel(obs_data_get_string(s, "precision_strokes"), c.keyColor);
    c.precisionTolerance = finiteClamp(obs_data_get_double(s, "precision_tolerance"), .005f, .30f, .025f);
    c.precisionSoftness = finiteClamp(obs_data_get_double(s, "precision_softness"), .005f, .30f, .06f);
    c.precisionEdge = finiteClamp(obs_data_get_double(s, "precision_edge"), 0, 1, .5f);
    c.precisionFeather = finiteClamp(obs_data_get_double(s, "precision_feather"), 0, .2f, .02f);
    return c;
}

void setFloat(gs_effect_t *e, const char *name, float value)
{
    if (auto *p = gs_effect_get_param_by_name(e, name)) gs_effect_set_float(p, value);
}
void setTexture(gs_effect_t *e, const char *name, gs_texture_t *texture, bool srgb = false)
{
    if (auto *p = gs_effect_get_param_by_name(e, name)) {
        if (srgb) gs_effect_set_texture_srgb(p, texture);
        else gs_effect_set_texture(p, texture);
    }
}
void setVec2(gs_effect_t *e, const char *name, float x, float y)
{
    vec2 v;
    vec2_set(&v, x, y);
    if (auto *p = gs_effect_get_param_by_name(e, name)) gs_effect_set_vec2(p, &v);
}
void setVec3(gs_effect_t *e, const char *name, float x, float y, float z)
{
    vec3 v;
    vec3_set(&v, x, y, z);
    if (auto *p = gs_effect_get_param_by_name(e, name)) gs_effect_set_vec3(p, &v);
}
void setRect(gs_effect_t *e, const char *name, const Rect &rect)
{
    vec4 v;
    vec4_set(&v, float(rect.x), float(rect.y), float(rect.width), float(rect.height));
    gs_effect_set_vec4(gs_effect_get_param_by_name(e, name), &v);
}

using ShaderVertex = std::array<float, 4>;
void packOutline(ShaderVertex *destination, const QPolygonF &outline)
{
    for (qsizetype i = 0; i < outline.size(); ++i)
        destination[i] = {float(outline[i].x()), float(outline[i].y()), 0, 0};
}

void colorParams(Filter *f, const Config &c)
{
    auto *e = f->colorEffect;
    const float r = (c.keyColor & 255) / 255.f;
    const float g = ((c.keyColor >> 8) & 255) / 255.f;
    const float b = ((c.keyColor >> 16) & 255) / 255.f;
    setVec2(e, "chroma_key", -.100644f * r - .338572f * g + .439216f * b + .501961f,
            .439216f * r - .398942f * g - .040274f * b + .501961f);
    setVec2(e, "pixel_size", 1.f / f->width, 1.f / f->height);
    setFloat(e, "similarity", c.similarity);
    setFloat(e, "smoothness", c.smoothness);
    setFloat(e, "opacity", c.opacity);
    setFloat(e, "despill_strength", c.despill);
    setFloat(e, "despill_rotate", c.rotate);
    setVec3(e, "rot_r", c.rotation[0], c.rotation[1], c.rotation[2]);
    setVec3(e, "rot_g", c.rotation[3], c.rotation[4], c.rotation[5]);
    setVec3(e, "rot_b", c.rotation[6], c.rotation[7], c.rotation[8]);
}

void precisionParams(Filter *f, const Config &c)
{
    auto *e = f->precisionEffect;
    std::array<std::array<float, 4>, MaxPrecisionKeys> colors{}, rects{}, flags{};
    for (size_t i = 0; i < c.precision.count; ++i) {
        colors[i] = c.precision.keys[i].color;
        rects[i] = c.precision.keys[i].rect;
        flags[i] = c.precision.keys[i].flags;
    }
    if (auto *p = gs_effect_get_param_by_name(e, "key_colors")) gs_effect_set_val(p, colors.data(), sizeof(colors));
    if (auto *p = gs_effect_get_param_by_name(e, "key_rects")) gs_effect_set_val(p, rects.data(), sizeof(rects));
    if (auto *p = gs_effect_get_param_by_name(e, "key_flags")) gs_effect_set_val(p, flags.data(), sizeof(flags));
    std::array<ShaderVertex, MaxPrecisionStrokes * MaxRegionVertices> vertices{};
    for (size_t i = 0; i < c.precision.outlines.size(); ++i)
        packOutline(vertices.data() + i * MaxRegionVertices, c.precision.outlines[i]);
    if (auto *p = gs_effect_get_param_by_name(e, "region_vertices")) gs_effect_set_val(p, vertices.data(), sizeof(vertices));
    setFloat(e, "key_count", float(c.precision.count));
    setFloat(e, "tolerance", c.precisionTolerance);
    setFloat(e, "softness", c.precisionSoftness);
    setFloat(e, "edge_strength", c.precisionEdge);
    setFloat(e, "feather", c.precisionFeather);
    setFloat(e, "despill_strength", c.despill);
    setFloat(e, "opacity", c.opacity);
    setVec2(e, "pixel_size", 1.f / f->width, 1.f / f->height);
}

void plateParams(Filter *f, const Config &c, gs_texture_t *plate)
{
    auto *e = f->plateEffect;
    setTexture(e, "plate", plate, true);
    setVec2(e, "pixel_size", 1.f / f->width, 1.f / f->height);
    setFloat(e, "tau_cd", c.tauCd); setFloat(e, "w_cd", c.wCd);
    setFloat(e, "tau_lo", c.tauLo); setFloat(e, "tau_hi", c.tauHi); setFloat(e, "w_a", c.wA);
    // Exact constants from v0.3 ac_plate_math.SHADER_CONSTS. No hair option is enabled.
    const std::pair<const char *, float> constants[]{
        {"sigma_floor", 2.f / 255}, {"sigma_scale", 16}, {"ee_min", 1e-6f},
        {"same_enable", 1}, {"same_a_lo", .35f}, {"same_a_hi", 1},
        {"same_dc_max", .04f}, {"same_dc_w", .01f}, {"same_chroma_min", .23f},
        {"same_gate_w", .02f}, {"chroma_sum_min", 1e-6f}, {"chroma_gray", 1.f / 3},
        {"ring_px", 3}, {"edge_boundary_alpha", .5f}, {"unmix_alpha_min", .7f},
        {"unmix_denom_min", 1e-6f}, {"edge_tie_eps", 1e-6f}, {"unmix_clamp", 1},
        {"keep_alpha_max", .5f / 255}, {"veto_r", 2}, {"guard_k", 4}, {"guard_fg_alpha", .5f}};
    for (const auto &[name, value] : constants) setFloat(e, name, value);
}

void cleanParams(Filter *f, gs_texture_t *plate)
{
    auto *e = f->cleanEffect;
    setTexture(e, "plate", plate, true);
    setVec2(e, "pixel_size", 1.f / f->width, 1.f / f->height);
    const std::pair<const char *, float> constants[]{
        {"hair_on", 0}, {"despeck_on", 1}, {"iso_t", .5f}, {"hair_k", 1},
        {"hair_lum_max", .3f}, {"hair_plate_sat_min", .05f}, {"hair_ring_px", 6},
        {"hair_boundary_alpha", .5f}, {"lum_eps", 1e-6f}};
    for (const auto &[name, value] : constants) setFloat(e, name, value);
    setVec3(e, "hair_lum_w", .2126f, .7152f, .0722f);
}

struct GraphicsState {
    bool linear = gs_set_linear_srgb(true);
    bool srgb = gs_framebuffer_srgb_enabled();
    GraphicsState() { gs_blend_state_push(); }
    ~GraphicsState() {
        gs_blend_state_pop();
        gs_enable_framebuffer_srgb(srgb);
        gs_set_linear_srgb(linear);
    }
};

void draw(gs_effect_t *effect, const char *technique, gs_texture_t *texture, uint32_t w, uint32_t h)
{
    while (gs_effect_loop(effect, technique)) gs_draw_sprite(texture, 0, w, h);
}

gs_texture_t *renderTo(gs_texrender_t *&target, uint32_t w, uint32_t h,
                       const std::function<void()> &render, gs_color_format format = GS_RGBA32F)
{
    if (!target) target = gs_texrender_create(format, GS_ZS_NONE);
    if (!target) return nullptr;
    gs_texrender_reset(target);
    if (!gs_texrender_begin_with_color_space(target, w, h, GS_CS_SRGB)) return nullptr;
    const bool previous = gs_framebuffer_srgb_enabled();
    gs_enable_framebuffer_srgb(false); // Float intermediates are linear; Encode writes explicit sRGB bytes.
    vec4 clear{};
    gs_clear(GS_CLEAR_COLOR, &clear, 0, 0);
    gs_ortho(0, float(w), 0, float(h), -100, 100);
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
    render();
    gs_texrender_end(target);
    gs_enable_framebuffer_srgb(previous);
    return gs_texrender_get_texture(target);
}

void destroyRenderer(Renderer &r)
{
    if (r.plate) gs_texture_destroy(r.plate);
    for (auto *tr : {r.keyed, r.masked, r.straight, r.veto}) if (tr) gs_texrender_destroy(tr);
    r = {};
}

void clearStages(Filter *f)
{
    for (auto &stage : f->stages) {
        if (stage.original) gs_stagesurface_destroy(stage.original);
        if (stage.processed) gs_stagesurface_destroy(stage.processed);
        stage = {};
    }
    f->nextStage = 0;
    f->lastCapture = 0;
}

void publishError(Filter *f, const std::string &error)
{
    {
        std::lock_guard lock(f->shared->mutex);
        f->shared->currentError = error;
    }
    if (error != f->loggedError) {
        if (!error.empty()) blog(LOG_WARNING, "[auto-chroma] %s", error.c_str());
        f->loggedError = error;
    }
}

bool loadPlate(Renderer &r, const Config &c, uint32_t w, uint32_t h, std::string &error)
{
    if (r.platePath != c.platePath) {
        if (r.plate) gs_texture_destroy(r.plate);
        r.plate = nullptr;
        r.platePath = c.platePath;
        if (!r.platePath.empty()) r.plate = gs_texture_create_from_file(r.platePath.c_str());
    }
    if (!r.plate) {
        error = "배경판을 읽지 못했습니다. 빈 배경 학습이나 자동 맞춤을 다시 실행하세요.";
        return false;
    }
    if (gs_texture_get_width(r.plate) != w || gs_texture_get_height(r.plate) != h) {
        error = "배경판과 소스 해상도가 다릅니다. 현재 해상도에서 다시 맞추세요.";
        return false;
    }
    return true;
}

gs_texture_t *process(Filter *f, Renderer &r, const Config &c, gs_texture_t *raw, std::string &error)
{
    auto *result = raw;
    if (c.method == "color") {
        result = renderTo(r.keyed, f->width, f->height, [&] {
            colorParams(f, c);
            setTexture(f->colorEffect, "image", raw);
            draw(f->colorEffect, "Draw", raw, f->width, f->height);
        });
    } else if (c.method == "precision") {
        if (!c.precision.valid) { error = c.precision.error; return raw; }
        if (!f->precisionEffect) { error = "정밀 키잉 GPU 엔진을 읽지 못했습니다. 기본 색 키를 사용할 수 있습니다."; return raw; }
        result = renderTo(r.keyed, f->width, f->height, [&] {
            precisionParams(f, c);
            setTexture(f->precisionEffect, "image", raw);
            draw(f->precisionEffect, "Draw", raw, f->width, f->height);
        });
    } else if (c.method == "plate") {
        if (!loadPlate(r, c, f->width, f->height, error)) return raw;
        if (c.despeck) {
            auto *straight = renderTo(r.straight, f->width, f->height, [&] {
                plateParams(f, c, r.plate);
                setTexture(f->plateEffect, "image", raw);
                draw(f->plateEffect, "DrawStraight", raw, f->width, f->height);
            });
            if (!straight) { error = "잡티 처리용 GPU 텍스처를 만들지 못했습니다."; return raw; }
            auto *veto = renderTo(r.veto, f->width, f->height, [&] {
                plateParams(f, c, r.plate);
                setTexture(f->plateEffect, "image", raw);
                setTexture(f->plateEffect, "key_tex", straight);
                draw(f->plateEffect, "Veto", raw, f->width, f->height);
            });
            if (!veto) { error = "잡티 보호용 GPU 텍스처를 만들지 못했습니다."; return raw; }
            result = renderTo(r.keyed, f->width, f->height, [&] {
                cleanParams(f, r.plate);
                setTexture(f->cleanEffect, "image", veto);
                draw(f->cleanEffect, "Draw", veto, f->width, f->height);
            });
        } else {
            result = renderTo(r.keyed, f->width, f->height, [&] {
                plateParams(f, c, r.plate);
                setTexture(f->plateEffect, "image", raw);
                draw(f->plateEffect, "Draw", raw, f->width, f->height);
            });
        }
    } else if (c.method != "passthrough" && !c.method.empty()) {
        error = "지원하지 않는 처리 방식입니다. 원본 화면을 표시합니다.";
        return raw;
    }
    if (!result) { error = "화면 처리용 GPU 텍스처를 만들지 못했습니다. 원본을 표시합니다."; return raw; }
    if (c.protect.valid() || c.erase.valid()) {
        auto *masked = renderTo(r.masked, f->width, f->height, [&] {
            setTexture(f->maskEffect, "image", result);
            setTexture(f->maskEffect, "original", raw);
            setRect(f->maskEffect, "protect_rect", c.protect.bounds);
            setRect(f->maskEffect, "erase_rect", c.erase.bounds);
            setFloat(f->maskEffect, "protect_count", float(c.protect.outline.size()));
            setFloat(f->maskEffect, "erase_count", float(c.erase.outline.size()));
            std::array<ShaderVertex, 2 * MaxRegionVertices> vertices{};
            packOutline(vertices.data(), c.protect.outline);
            packOutline(vertices.data() + MaxRegionVertices, c.erase.outline);
            if (auto *p = gs_effect_get_param_by_name(f->maskEffect, "region_vertices"))
                gs_effect_set_val(p, vertices.data(), sizeof(vertices));
            draw(f->maskEffect, "Mask", result, f->width, f->height);
        });
        if (!masked) { error = "보호·지움 영역을 처리하지 못했습니다. 원본을 표시합니다."; return raw; }
        result = masked;
    }
    return result;
}

bool mapFrame(gs_stagesurf_t *stage, uint32_t w, uint32_t h, uint64_t timestamp, Frame &frame)
{
    uint8_t *data = nullptr;
    uint32_t pitch = 0;
    if (!gs_stagesurface_map(stage, &data, &pitch)) return false;
    if (!data || pitch < w * 4) { gs_stagesurface_unmap(stage); return false; }
    try {
        frame.width = w; frame.height = h; frame.timestamp = timestamp;
        frame.rgba.resize(size_t(w) * h * 4);
        for (uint32_t y = 0; y < h; ++y)
            std::memcpy(frame.rgba.data() + size_t(y) * w * 4, data + size_t(y) * pitch, size_t(w) * 4);
    } catch (...) {
        gs_stagesurface_unmap(stage);
        throw;
    }
    gs_stagesurface_unmap(stage);
    return true;
}

bool stageTexture(Filter *f, gs_stagesurf_t *&stage, gs_texture_t *texture)
{
    if (!stage) stage = gs_stagesurface_create(f->width, f->height, GS_RGBA_UNORM);
    if (!stage) return false;
    auto *encoded = renderTo(f->encoded, f->width, f->height, [&] {
        setTexture(f->maskEffect, "image", texture);
        draw(f->maskEffect, "Encode", texture, f->width, f->height);
    }, GS_RGBA_UNORM);
    if (!encoded) return false;
    gs_stage_texture(stage, encoded);
    return true;
}

bool captureDue(const Filter *f, uint64_t interval)
{
    const auto timestamp = obs_get_video_frame_time();
    return timestamp && (!f->lastCapture ||
        (timestamp != f->lastCapture && (timestamp < f->lastCapture || timestamp - f->lastCapture >= interval)));
}

void capturePreview(Filter *f, gs_texture_t *raw, gs_texture_t *processed, const std::string &error,
                    uint64_t epoch, const std::string &request)
{
    if (epoch != f->shared->epoch.load()) return;
    if (f->stageEpoch != epoch) { clearStages(f); f->stageEpoch = epoch; }
    const auto timestamp = obs_get_video_frame_time();
    if (timestamp == 0 || timestamp == f->lastCapture) return;
    auto &previous = f->stages[1 - f->nextStage];
    if (previous.timestamp) {
        Preview preview;
        preview.error = previous.error;
        preview.revision = epoch;
        preview.appliedRequest = request;
        const bool ok = mapFrame(previous.original, f->width, f->height, previous.timestamp, preview.original)
            && mapFrame(previous.processed, f->width, f->height, previous.timestamp, preview.processed);
        previous.timestamp = 0;
        if (ok) {
            std::lock_guard lock(f->shared->mutex);
            if (epoch == f->shared->epoch.load()) f->shared->preview = std::move(preview);
        } else {
            publishError(f, "미리보기 GPU 프레임을 읽지 못했습니다. 소스와 그래픽 장치를 확인하세요.");
        }
    }
    auto &next = f->stages[f->nextStage];
    if (stageTexture(f, next.original, raw) && stageTexture(f, next.processed, processed)) {
        next.error = error;
        next.timestamp = timestamp;
        f->lastCapture = timestamp;
        f->nextStage = 1 - f->nextStage;
    } else {
        next.timestamp = 0;
        publishError(f, "미리보기용 GPU 자원을 만들지 못했습니다.");
    }
}

void render(void *data, gs_effect_t *)
{
    auto *f = static_cast<Filter *>(data);
    auto *target = obs_filter_get_target(f->context);
    const auto w = target ? obs_source_get_base_width(target) : 0;
    const auto h = target ? obs_source_get_base_height(target) : 0;
    if (!w || !h) { publishError(f, "소스 영상이 아직 없습니다. 카메라와 소스 표시 상태를 확인하세요."); return; }
    Config config;
    std::optional<Config> previewConfig;
    uint64_t epoch;
    {
        std::lock_guard lock(f->shared->mutex);
        config = f->shared->config;
        previewConfig = f->shared->previewConfig;
        epoch = f->shared->epoch.load();
    }
    GraphicsState state;
    const bool oversizedCapture = f->shared->capture.load() && uint64_t(w) * h > 3840ULL * 2160ULL;
    const std::string captureLimitError = oversizedCapture ? "미리보기는 최대 4K 픽셀 수까지 지원합니다. 소스 해상도를 낮춰 주세요." : "";
    const bool capture = f->shared->capture.load() && !oversizedCapture;
    if (w != f->width || h != f->height) {
        clearStages(f);
        f->width = w; f->height = h;
        std::lock_guard lock(f->shared->mutex);
        f->shared->preview = {};
    }
    if (!capture && (f->lastCapture || f->stages[0].original)) clearStages(f);
    if (!previewConfig && (f->candidate.plate || f->candidate.keyed)) destroyRenderer(f->candidate);
    const bool masked = config.protect.valid() || config.erase.valid();
    // With the dock preview closed, use the original one-pass path whenever possible.
    if (!capture && !masked) {
        if (config.method.empty() || config.method == "passthrough") {
            obs_source_skip_video_filter(f->context);
            publishError(f, captureLimitError);
            return;
        }
        gs_effect_t *effect = nullptr;
        if (config.method == "color") effect = f->colorEffect;
        else if (config.method == "precision") {
            if (!config.precision.valid || !f->precisionEffect) {
                obs_source_skip_video_filter(f->context);
                publishError(f, config.precision.valid ? "정밀 키잉 GPU 엔진을 읽지 못했습니다. 기본 색 키를 사용할 수 있습니다." : config.precision.error);
                return;
            }
            effect = f->precisionEffect;
        }
        else if (config.method == "plate") {
            std::string error;
            if (!loadPlate(f->live, config, w, h, error)) {
                obs_source_skip_video_filter(f->context);
                publishError(f, error);
                return;
            }
            if (!config.despeck) effect = f->plateEffect;
        }
        if (effect) {
            if (!obs_source_process_filter_begin_with_color_space(f->context, GS_RGBA, GS_CS_SRGB,
                                                                 OBS_ALLOW_DIRECT_RENDERING)) {
                publishError(f, "소스 GPU 프레임을 준비하지 못했습니다.");
                return;
            }
            if (effect == f->colorEffect) colorParams(f, config);
            else if (effect == f->precisionEffect) precisionParams(f, config);
            else plateParams(f, config, f->live.plate);
            gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
            obs_source_process_filter_end(f->context, effect, 0, 0);
            publishError(f, captureLimitError);
            return;
        }
    }
    // Disable bypass: both the real output and candidate preview must see identical source pixels.
    if (!obs_source_process_filter_begin_with_color_space(f->context, GS_RGBA, GS_CS_SRGB,
                                                         OBS_NO_DIRECT_RENDERING)) {
        publishError(f, "소스 GPU 프레임을 준비하지 못했습니다.");
        return;
    }
    auto *raw = renderTo(f->original, w, h, [&] {
        obs_source_process_filter_tech_end(f->context, f->maskEffect, w, h, "Draw");
    });
    if (!raw) {
        // Complete the begin/end pair even if the independent intermediate failed.
        obs_source_process_filter_end(f->context, f->maskEffect, w, h);
        publishError(f, "원본 GPU 텍스처를 만들지 못했습니다. 원본을 표시합니다.");
        return;
    }
    std::string liveError;
    auto *output = process(f, f->live, config, raw, liveError);
    gs_texture_t *preview = output;
    std::string previewError = liveError;
    const uint64_t interval = f->shared->liveCapture.load() ? 100000000 : 33333333;
    const bool sample = capture && captureDue(f, interval);
    if (sample && previewConfig) preview = process(f, f->candidate, *previewConfig, raw, previewError);
    // Draw only committed settings. Candidate settings never enter the broadcast output.
    gs_enable_framebuffer_srgb(true);
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
    setTexture(f->maskEffect, "image", output);
    draw(f->maskEffect, "Draw", output, w, h);
    if (sample || !previewConfig || !liveError.empty())
        publishError(f, capture ? previewError : (liveError.empty() ? captureLimitError : liveError));
    if (sample) {
        try { capturePreview(f, raw, preview, previewError, epoch, config.request); }
        catch (const std::exception &) { publishError(f, "미리보기 메모리를 확보하지 못했습니다."); }
    }
}

void update(void *data, obs_data_t *settings)
{
    auto *f = static_cast<Filter *>(data);
    auto config = readConfig(settings);
    std::lock_guard lock(f->shared->mutex);
    f->shared->config = std::move(config);
    f->shared->preview = {};
    f->shared->currentError.clear();
    f->shared->epoch.fetch_add(1);
}

gs_effect_t *loadEffect(const char *name)
{
    char *path = obs_module_file(name);
    if (!path) return nullptr;
    char *errors = nullptr;
    auto *effect = gs_effect_create_from_file(path, &errors);
    if (errors) { blog(LOG_WARNING, "[auto-chroma] %s: %s", name, errors); bfree(errors); }
    bfree(path);
    return effect;
}

void destroy(void *data)
{
    auto *f = static_cast<Filter *>(data);
    if (!f) return;
    {
        std::lock_guard lock(registryMutex);
        registry.erase(f->context);
    }
    obs_enter_graphics();
    clearStages(f);
    destroyRenderer(f->live);
    destroyRenderer(f->candidate);
    for (auto *tr : {f->original, f->encoded}) if (tr) gs_texrender_destroy(tr);
    for (auto *e : {f->colorEffect, f->plateEffect, f->cleanEffect, f->maskEffect, f->precisionEffect}) if (e) gs_effect_destroy(e);
    obs_leave_graphics();
    delete f;
}

void *create(obs_data_t *settings, obs_source_t *context)
{
    auto *f = new Filter;
    f->context = context;
    obs_enter_graphics();
    f->colorEffect = loadEffect("ac_keyer.effect");
    f->plateEffect = loadEffect("ac_plate_keyer.effect");
    f->cleanEffect = loadEffect("ac_plate_clean.effect");
    f->maskEffect = loadEffect("mask.effect");
    f->precisionEffect = loadEffect("precision.effect");
    obs_leave_graphics();
    if (!f->colorEffect || !f->plateEffect || !f->cleanEffect || !f->maskEffect) {
        blog(LOG_ERROR, "[auto-chroma] Shader initialization failed; filter was not created.");
        destroy(f);
        return nullptr;
    }
    update(f, settings);
    {
        std::lock_guard lock(registryMutex);
        registry[context] = f->shared;
    }
    return f;
}

void defaults(obs_data_t *s)
{
    obs_data_set_default_string(s, "method", "passthrough");
    obs_data_set_default_string(s, "key_color_type", "custom");
    obs_data_set_default_int(s, "key_color", 0x00ff00);
    obs_data_set_default_int(s, "similarity", 400);
    obs_data_set_default_int(s, "smoothness", 80);
    const std::pair<const char *, double> values[]{
        {"opacity", 1}, {"despill_strength", 1}, {"despill_rotate", 0},
        {"rot_r1", 1}, {"rot_r2", 0}, {"rot_r3", 0},
        {"rot_g1", 0}, {"rot_g2", 1}, {"rot_g3", 0},
        {"rot_b1", 0}, {"rot_b2", 0}, {"rot_b3", 1},
        {"tau_cd", 3}, {"w_cd", 1}, {"tau_lo", .55}, {"tau_hi", 1.1}, {"w_a", .05}};
    for (const auto &[key, value] : values) obs_data_set_default_double(s, key, value);
    obs_data_set_default_string(s, "plate_path", "");
    obs_data_set_default_string(s, "plate_kind", "auto");
    obs_data_set_default_bool(s, "despeck", false);
    obs_data_set_default_string(s, "precision_strokes", "[]");
    obs_data_set_default_double(s, "precision_tolerance", .025);
    obs_data_set_default_double(s, "precision_softness", .06);
    obs_data_set_default_double(s, "precision_edge", .5);
    obs_data_set_default_double(s, "precision_feather", .02);
    for (const char *prefix : {"protect", "erase"})
        for (const char *axis : {"x", "y", "w", "h"})
            obs_data_set_default_double(s, (std::string(prefix) + "_" + axis).c_str(), 0);
}

obs_properties_t *properties(void *data)
{
    auto *props = obs_properties_create();
    const std::string credits = std::string(Author) + " · v" + Version;
    obs_properties_add_text(props, "credits", credits.c_str(), OBS_TEXT_INFO);
    obs_properties_add_text(props, "guide", "자동 맞춤과 적용 전 비교는 OBS 도크의 ‘자동 크로마키’에서 실행하세요.", OBS_TEXT_INFO);
    auto *method = obs_properties_add_list(props, "method", "처리 방식", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(method, "원본 유지", "passthrough");
    obs_property_list_add_string(method, "색 키", "color");
    obs_property_list_add_string(method, "정밀 색 키 (스트로크)", "precision");
    obs_property_list_add_string(method, "배경판", "plate");
    obs_properties_add_color(props, "key_color", "키 색");
    obs_properties_add_int_slider(props, "similarity", "유사성", 1, 1000, 1);
    obs_properties_add_int_slider(props, "smoothness", "매끄러움", 1, 1000, 1);
    obs_properties_add_float_slider(props, "despill_strength", "배경색 번짐 제거", 0, 1, .01);
    obs_properties_add_float_slider(props, "precision_tolerance", "정밀 배경색 허용", .005, .30, .001);
    obs_properties_add_float_slider(props, "precision_softness", "정밀 경계 부드러움", .005, .30, .001);
    obs_properties_add_float_slider(props, "precision_edge", "머리카락 경계 보정", 0, 1, .01);
    obs_properties_add_float_slider(props, "precision_feather", "국소 보정 경계 부드러움", 0, .2, .001);
    obs_properties_add_float_slider(props, "opacity", "불투명도", 0, 1, .01);
    obs_properties_add_float_slider(props, "tau_cd", "배경판 색 차이", .5, 30, .1);
    obs_properties_add_float_slider(props, "tau_lo", "배경판 밝기 하한", .1, 1, .01);
    obs_properties_add_float_slider(props, "tau_hi", "배경판 밝기 상한", 1, 2, .01);
    obs_properties_add_bool(props, "despeck", "잡티 점 지우기 (빈 배경 학습판만)");
    if (data) {
        auto *f = static_cast<Filter *>(data);
        std::lock_guard lock(f->shared->mutex);
        if (!f->shared->preview.error.empty())
            obs_properties_add_text(props, "render_status", f->shared->preview.error.c_str(), OBS_TEXT_INFO);
    }
    return props;
}

uint32_t width(void *data)
{
    auto *target = obs_filter_get_target(static_cast<Filter *>(data)->context);
    return target ? obs_source_get_base_width(target) : 0;
}
uint32_t height(void *data)
{
    auto *target = obs_filter_get_target(static_cast<Filter *>(data)->context);
    return target ? obs_source_get_base_height(target) : 0;
}
} // namespace

void registerFilter()
{
    obs_source_info info{};
    info.id = FilterId;
    info.type = OBS_SOURCE_TYPE_FILTER;
    info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
    info.get_name = [](void *) { return "자동 크로마키"; };
    info.create = create;
    info.destroy = destroy;
    info.update = update;
    info.get_defaults = defaults;
    info.get_properties = properties;
    info.get_width = width;
    info.get_height = height;
    info.video_render = render;
    obs_register_source(&info);
}

void requestCapture(obs_source_t *filter, bool enabled, bool live)
{
    if (auto state = stateFor(filter)) {
        state->liveCapture.store(live);
        const bool previous = state->capture.exchange(enabled);
        if (previous != enabled) {
            std::lock_guard lock(state->mutex);
            state->preview = {};
            state->currentError.clear();
            state->epoch.fetch_add(1);
        }
    }
}

uint64_t setPreviewSettings(obs_source_t *filter, obs_data_t *settings)
{
    if (auto state = stateFor(filter)) {
        std::optional<Config> config;
        if (settings) {
            // JSON candidates contain only user values; apply defaults just as OBS does for a source.
            auto *merged = obs_data_create();
            defaults(merged);
            obs_data_apply(merged, settings);
            config = readConfig(merged);
            obs_data_release(merged);
        }
        std::lock_guard lock(state->mutex);
        state->previewConfig = std::move(config);
        state->preview = {};
        state->currentError.clear();
        return state->epoch.fetch_add(1) + 1;
    }
    return 0;
}

Preview readPreview(obs_source_t *filter)
{
    if (auto state = stateFor(filter)) {
        std::lock_guard lock(state->mutex);
        auto preview = state->preview;
        if (!state->currentError.empty()) preview.error = state->currentError;
        return preview;
    }
    Preview p;
    p.error = "자동 크로마키 필터가 없거나 해제되었습니다.";
    return p;
}

std::string appliedRequest(obs_source_t *filter)
{
    if (auto state = stateFor(filter)) {
        std::lock_guard lock(state->mutex);
        return state->config.request;
    }
    return {};
}

bool previewVerified(obs_source_t *filter, uint64_t revision)
{
    if (auto state = stateFor(filter)) {
        std::lock_guard lock(state->mutex);
        const auto &preview = state->preview;
        return revision && state->previewConfig && revision == state->epoch.load() &&
            preview.revision == revision && preview.original.valid() && preview.processed.valid() &&
            preview.error.empty() && state->currentError.empty();
    }
    return false;
}
} // namespace ac
