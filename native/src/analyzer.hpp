#pragma once
#include "ac-types.hpp"
#include <string>
namespace ac {
AnalysisResult analyze(const std::vector<Frame> &frames, Roi roi, AnalysisMode mode,
                       const CancelFlag &cancel, const std::string &moduleDirectory,
                       const std::string &luaLibraryPath);
}
