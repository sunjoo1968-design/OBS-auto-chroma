#pragma once
#include "ac-types.hpp"
#include <obs.h>
namespace ac {
void registerFilter();
// All functions below are thread safe; source must be held by caller.
void requestCapture(obs_source_t *filter, bool enabled, bool live = false);
Preview readPreview(obs_source_t *filter);
std::string appliedRequest(obs_source_t *filter);
bool previewVerified(obs_source_t *filter, uint64_t revision);
// Candidate is rendered into the preview only; it never changes the live output.
uint64_t setPreviewSettings(obs_source_t *filter, obs_data_t *settings);
}
