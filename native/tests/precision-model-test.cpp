// Persisted stroke-contract regression tests. SunjooAn 0.5.0-dev.
#include "precision-model.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>
#include <iostream>

namespace {
int failures = 0;
void check(bool ok, const char *name)
{
    std::cout << (ok ? "PASS " : "FAIL ") << name << std::endl;
    if (!ok) ++failures;
}
std::string json(const QJsonArray &strokes)
{
    return QJsonDocument(strokes).toJson(QJsonDocument::Compact).toStdString();
}
QJsonObject stroke()
{
    return {{"subtract", false}, {"enabled", true}, {"local", false},
            {"samples", QJsonArray{QJsonArray{10, 200, 20}, QJsonArray{140, 230, 130}}},
            {"points", QJsonArray{QJsonArray{.2, .3}, QJsonArray{.6, .7}}}};
}
}
int main()
{
    const auto empty = ac::parsePrecisionModel("[]", 30 | (180 << 8) | (60 << 16));
    check(empty.valid && empty.count == 1 && std::abs(empty.keys[0].color[0] - 30.f / 255.f) < 1e-6f,
          "empty persisted stroke list keeps the existing base key");
    const auto multiple = ac::parsePrecisionModel(json({stroke()}), 0);
    bool first = false, second = false;
    for (size_t i = 1; i < multiple.count; ++i) {
        first |= std::abs(multiple.keys[i].color[0] - 10.f / 255.f) < 1e-6f;
        second |= std::abs(multiple.keys[i].color[0] - 140.f / 255.f) < 1e-6f;
    }
    check(multiple.valid && first && second, "separated colors in one stroke remain sampled colors rather than an invented average");
    auto local = stroke(); local["local"] = true; local["subtract"] = true;
    local["rect"] = QJsonArray{.5, .5, .25, .25};
    const auto scope = ac::parsePrecisionModel(json({local}), 0);
    check(scope.valid && scope.keys[1].flags[0] == 1 && scope.keys[1].flags[1] == 1
          && scope.keys[1].rect[0] == .5f, "local excluded-color stroke retains its scope and intent");
    auto disabled = stroke(); disabled["enabled"] = false;
    const auto inactive = ac::parsePrecisionModel(json({disabled}), 0);
    check(inactive.valid && inactive.count == 1, "disabled stroke cannot silently affect GPU keys");
    QJsonArray maximum;
    for (size_t i = 0; i < ac::MaxPrecisionStrokes; ++i) maximum.append(stroke());
    const auto bounded = ac::parsePrecisionModel(json(maximum), 0);
    check(bounded.valid && bounded.count <= ac::MaxPrecisionKeys && bounded.strokeCount == ac::MaxPrecisionStrokes,
          "maximum accepted strokes fit the bounded GPU key array without dropping an entire stroke");
    maximum.append(stroke());
    check(!ac::parsePrecisionModel(json(maximum), 0).valid, "too many persisted strokes are rejected rather than truncated");
    auto tooManySamples = stroke(); QJsonArray samples;
    for (size_t i = 0; i <= ac::MaxPrecisionSamplesPerStroke; ++i) samples.append(QJsonArray{10, 200, 20});
    tooManySamples["samples"] = samples;
    check(!ac::parsePrecisionModel(json({tooManySamples}), 0).valid, "oversized sample arrays are rejected");
    auto invalidColor = stroke(); QJsonArray badRgb;
    badRgb.append(QJsonArray{-1, 260, .5}); invalidColor["samples"] = badRgb;
    check(!ac::parsePrecisionModel(json({invalidColor}), 0).valid, "out-of-range or fractional RGB samples are rejected");
    local["rect"] = QJsonArray{.9, .5, .2, .2};
    check(!ac::parsePrecisionModel(json({local}), 0).valid, "local rectangle cannot extend beyond the source image");
    auto invalidPoint = stroke(); QJsonArray badPoint;
    badPoint.append(QJsonArray{.2, 1.1}); invalidPoint["points"] = badPoint;
    check(!ac::parsePrecisionModel(json({invalidPoint}), 0).valid, "stroke paths outside normalized source coordinates are rejected");
    check(!ac::parsePrecisionModel("[{broken", 0).valid && !ac::parsePrecisionModel("{}", 0).valid,
          "malformed or wrong-root JSON is rejected as a whole");
    check(!ac::parsePrecisionModel(std::string(131073, ' '), 0).valid,
          "oversized persisted JSON is rejected before parsing");
    std::cout << "Precision model failures: " << failures << std::endl;
    return failures ? 1 : 0;
}
