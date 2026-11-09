#pragma once
#include "ac-types.hpp"
#include <obs.h>
namespace ac {
void registerFilter();
// All functions below are thread safe; source must be held by caller.
void requestCapture(obs_source_t *filter, bool enabled);
Preview readPreview(obs_source_t *filter);
// Candidate is rendered into the preview only; it never changes the live output.
void setPreviewSettings(obs_source_t *filter, obs_data_t *settings);
}
