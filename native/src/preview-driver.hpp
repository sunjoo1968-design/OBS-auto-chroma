#pragma once
#include <obs.h>
namespace ac {
// Drives a private view into an offscreen target; it never draws on Program.
class PreviewDriver {
public:
    PreviewDriver();
    ~PreviewDriver();
    void setSource(obs_source_t *source);
    void stop();
private:
    static void render(void *, uint32_t, uint32_t);
    obs_view_t *view_ = nullptr;
    gs_texrender_t *target_ = nullptr;
};
}
