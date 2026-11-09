// OBS Auto Chroma 0.5.0-dev — SunjooAn. GPL-2.0-or-later.
#include "precision-model.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <algorithm>
#include <cmath>
#include <vector>

namespace ac {
namespace {
using Color = std::array<float, 4>;
float distanceSquared(const Color &a, const Color &b)
{
    const float dr = a[0] - b[0], dg = a[1] - b[1], db = a[2] - b[2];
    return dr * dr + dg * dg + db * db;
}
bool number(const QJsonValue &v, double lo, double hi)
{
    return v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble() >= lo && v.toDouble() <= hi;
}
bool boolean(const QJsonObject &o, const char *name)
{
    return !o.contains(name) || o.value(name).isBool();
}
PrecisionModel invalid(const char *reason)
{
    PrecisionModel m;
    m.valid = false;
    m.error = reason;
    return m;
}
}
PrecisionModel parsePrecisionModel(const std::string &json, uint32_t keyColor)
{
    if (json.size() > 262144) return invalid("정밀 스트로크 데이터가 너무 큽니다. 최대 256 KiB입니다.");
    QJsonArray strokes;
    if (!json.empty()) {
        QJsonParseError error{};
        const auto doc = QJsonDocument::fromJson(QByteArray(json.data(), int(json.size())), &error);
        if (error.error != QJsonParseError::NoError || !doc.isArray())
            return invalid("정밀 스트로크 형식이 올바르지 않습니다. 스트로크를 다시 지정하세요.");
        strokes = doc.array();
    }
    if (strokes.size() > int(MaxPrecisionStrokes))
        return invalid("정밀 스트로크는 최대 16개입니다.");
    PrecisionModel model;
    model.keys[0].color = {float(keyColor & 255) / 255.f, float((keyColor >> 8) & 255) / 255.f,
                           float((keyColor >> 16) & 255) / 255.f, 0};
    model.count = 1;
    size_t activeCount = 0, activeIndex = 0;
    for (const auto &entry : strokes)
        if (entry.isObject() && entry.toObject().value("enabled").toBool(true)) ++activeCount;
    for (int strokeIndex = 0; strokeIndex < strokes.size(); ++strokeIndex) {
        const auto entry = strokes[strokeIndex];
        if (!entry.isObject()) return invalid("정밀 스트로크 항목이 올바르지 않습니다.");
        const auto stroke = entry.toObject();
        if (!boolean(stroke, "enabled") || !boolean(stroke, "subtract") || !boolean(stroke, "local"))
            return invalid("정밀 스트로크 선택 상태가 올바르지 않습니다.");
        const bool local = stroke.value("local").toBool();
        std::array<float, 4> rect{0, 0, 1, 1};
        if (local) {
            if (!stroke.value("rect").isArray()) return invalid("국소 보정 영역이 없습니다.");
            const auto box = stroke.value("rect").toArray();
            if (box.size() != 4) return invalid("국소 보정 영역 형식이 올바르지 않습니다.");
            for (int i = 0; i < 4; ++i) {
                if (!number(box[i], 0, 1)) return invalid("국소 보정 영역은 화면 안에 지정하세요.");
                rect[size_t(i)] = float(box[i].toDouble());
            }
            if (rect[2] <= 0 || rect[3] <= 0 || rect[0] + rect[2] > 1.000001f || rect[1] + rect[3] > 1.000001f)
                return invalid("국소 보정 영역 크기를 확인하세요.");
            const auto shape = readRegion(stroke.value("region_points"), {rect[0], rect[1], rect[2], rect[3]});
            if (!shape.valid()) return invalid("국소 보정 윤곽을 다시 지정하세요.");
            model.outlines[size_t(strokeIndex)] = shape.outline;
            if (!shape.outline.isEmpty()) {
                const auto r = shape.bounds;
                rect = {float(r.x), float(r.y), float(r.width), float(r.height)};
            }
        }
        if (stroke.contains("points")) {
            if (!stroke.value("points").isArray()) return invalid("스트로크 경로가 올바르지 않습니다.");
            const auto points = stroke.value("points").toArray();
            if (points.size() > 1024) return invalid("스트로크 경로가 너무 깁니다.");
            for (const auto &point : points) {
                if (!point.isArray()) return invalid("스트로크 좌표가 올바르지 않습니다.");
                const auto xy = point.toArray();
                if (xy.size() != 2 || !number(xy[0], 0, 1) || !number(xy[1], 0, 1))
                    return invalid("스트로크 좌표는 화면 안에 지정하세요.");
            }
        }
        if (!stroke.value("samples").isArray()) return invalid("스트로크 색 샘플이 없습니다.");
        const auto samples = stroke.value("samples").toArray();
        if (samples.isEmpty() || samples.size() > int(MaxPrecisionSamplesPerStroke))
            return invalid("스트로크 한 개에 색 샘플을 1~64개 지정하세요.");
        std::vector<Color> colors;
        for (const auto &sample : samples) {
            if (!sample.isArray()) return invalid("색 샘플이 올바르지 않습니다.");
            const auto rgb = sample.toArray();
            if (rgb.size() != 3) return invalid("색 샘플에는 RGB 세 값이 필요합니다.");
            Color color{};
            for (int i = 0; i < 3; ++i) {
                if (!number(rgb[i], 0, 255) || rgb[i].toDouble() != std::round(rgb[i].toDouble()))
                    return invalid("색 샘플 RGB는 0~255 정수여야 합니다.");
                color[size_t(i)] = float(rgb[i].toDouble() / 255.);
            }
            colors.push_back(color);
        }
        if (!stroke.value("enabled").toBool(true)) continue;
        ++model.strokeCount;
        // Choose actual sampled colors: retain separated color modes instead of one average key.
        Color mean{};
        for (const auto &color : colors) for (size_t j = 0; j < 3; ++j) mean[j] += color[j] / float(colors.size());
        const auto middle = std::min_element(colors.begin(), colors.end(), [&mean](const auto &a, const auto &b) {
            return distanceSquared(a, mean) < distanceSquared(b, mean);
        });
        auto append = [&](const Color &color) {
            auto &key = model.keys[model.count++];
            key.color = color; key.rect = rect;
            key.flags = {stroke.value("subtract").toBool() ? 1.f : 0.f, local ? 1.f : 0.f,
                         float(strokeIndex * MaxRegionVertices), float(model.outlines[size_t(strokeIndex)].size())};
        };
        // Divide the fixed GPU budget fairly between active strokes. A lone stroke
        // can describe a lighting gradient with many colors instead of losing its
        // bright end when a dark endpoint is farthest from its medoid.
        const size_t available = MaxPrecisionKeys - 1;
        const size_t budget = available / activeCount + (activeIndex < available % activeCount ? 1 : 0);
        ++activeIndex;
        std::vector<Color> representatives{*middle};
        while (representatives.size() < budget) {
            float farthestDistance = 0;
            size_t farthestIndex = 0;
            for (size_t i = 0; i < colors.size(); ++i) {
                float nearest = 4;
                for (const auto &representative : representatives)
                    nearest = std::min(nearest, distanceSquared(colors[i], representative));
                if (nearest > farthestDistance) { farthestDistance = nearest; farthestIndex = i; }
            }
            // RGB distance 0.015 is below the default chroma tolerance; avoid
            // spending shader slots on nearly duplicate compression/noise colors.
            if (farthestDistance <= .000225f) break;
            representatives.push_back(colors[farthestIndex]);
        }
        for (const auto &representative : representatives) append(representative);
    }
    return model;
}
}
