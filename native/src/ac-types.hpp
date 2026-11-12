#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ac {
inline constexpr const char *Version = "1.4";
inline constexpr const char *Author = "SunjooAn";
inline constexpr const char *FilterId = "sunjoo_auto_chroma";
struct Frame {
    uint32_t width = 0, height = 0;
    uint64_t timestamp = 0;
    std::vector<uint8_t> rgba;
    bool valid() const { return width && height && rgba.size() == size_t(width) * height * 4; }
};
struct Roi { int top = 0, bottom = 0, left = 0, right = 0; };
struct Rect { double x = 0, y = 0, width = 0, height = 0; };
enum class AnalysisMode { Auto, Plate };
struct AnalysisResult {
    bool success = false, cancelled = false;
    std::string message, method;
    std::map<std::string, double> numbers;
    std::map<std::string, std::string> strings;
    Frame plate;
};
struct Preview {
    Frame original, processed;
    std::string error;
    uint64_t revision = 0;
    std::string appliedRequest;
};
using CancelFlag = std::shared_ptr<std::atomic_bool>;
}
