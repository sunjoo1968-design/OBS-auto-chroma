#pragma once
#include "region.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ac {
inline constexpr size_t MaxPrecisionStrokes = 16;
inline constexpr size_t MaxPrecisionSamplesPerStroke = 64;
inline constexpr size_t MaxPrecisionKeys = 32;
// Packed float4 values match precision.effect. Colors are straight nonlinear sRGB.
struct PrecisionKey {
    std::array<float, 4> color{};
    std::array<float, 4> rect{0, 0, 1, 1};
    std::array<float, 4> flags{}; // subtract, local, outline offset, outline vertex count
};
struct PrecisionModel {
    std::array<PrecisionKey, MaxPrecisionKeys> keys{};
    std::array<QPolygonF, MaxPrecisionStrokes> outlines{};
    size_t count = 0, strokeCount = 0;
    bool valid = true;
    std::string error;
};
// Empty input means an empty stroke list. Invalid documents are rejected as a whole.
PrecisionModel parsePrecisionModel(const std::string &json, uint32_t keyColor);
}
