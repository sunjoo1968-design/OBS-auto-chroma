#include "preview-driver.hpp"
#include <graphics/graphics.h>
#include <graphics/vec4.h>

namespace ac {
PreviewDriver::PreviewDriver()
{
    view_ = obs_view_create();
    obs_add_main_render_callback(render, this);
}
PreviewDriver::~PreviewDriver() { stop(); }
void PreviewDriver::stop()
{
    if (!view_) return;
    obs_remove_main_render_callback(render, this);
    obs_enter_graphics();
    if (target_) gs_texrender_destroy(target_);
    target_ = nullptr;
    obs_leave_graphics();
    obs_view_destroy(view_);
    view_ = nullptr;
}
void PreviewDriver::setSource(obs_source_t *source) { if (view_) obs_view_set_source(view_, 0, source); }
void PreviewDriver::render(void *opaque, uint32_t, uint32_t)
{
    auto &self = *static_cast<PreviewDriver *>(opaque);
    obs_source_t *source = obs_view_get_source(self.view_, 0);
    if (!source) return;
    const uint32_t width = obs_source_get_base_width(source), height = obs_source_get_base_height(source);
    const bool removed = obs_source_removed(source);
    obs_source_release(source);
    if (removed || !width || !height || uint64_t(width) * height > 3840ULL * 2160ULL) return;
    if (!self.target_) self.target_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    if (!self.target_) return;
    gs_texrender_reset(self.target_);
    if (!gs_texrender_begin(self.target_, width, height)) return;
    struct vec4 clear {}; gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
    gs_ortho(0, float(width), 0, float(height), -100, 100);
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
    obs_view_render(self.view_);
    gs_blend_state_pop();
    gs_texrender_end(self.target_);
}
}
