// SunjooAn. Read scene-item geometry without changing OBS transforms.
#include "scene-rotation.hpp"
#include <obs-frontend-api.h>
#include <graphics/matrix4.h>
#include <algorithm>
#include <cmath>

namespace ac {
namespace {
struct SceneMatch { obs_source_t *source; SceneTransform transform; bool found = false, selected = false; };
struct SearchContext { SceneMatch *match; QTransform parent; QSizeF canvas; int depth = 0; bool parentSelected = false; };

QMargins itemCrop(obs_sceneitem_t *item, obs_source_t *source, const matrix4 &draw)
{
    obs_sceneitem_crop crop{};
    obs_sceneitem_get_crop(item, &crop);
    const auto type = obs_sceneitem_get_bounds_type(item);
    if (obs_sceneitem_get_bounds_crop(item) &&
        (type == OBS_BOUNDS_SCALE_OUTER || type == OBS_BOUNDS_SCALE_TO_WIDTH || type == OBS_BOUNDS_SCALE_TO_HEIGHT)) {
        matrix4 box{};
        obs_sceneitem_get_box_transform(item, &box);
        vec2 scale{};
        obs_sceneitem_get_scale(item, &scale);
        const auto align = obs_sceneitem_get_bounds_alignment(item);
        auto extendCrop = [align](double pixels, double drawnScale, double bound, bool flipped,
                                   uint32_t firstFlag, uint32_t lastFlag, int &first, int &last) {
            const double excess = pixels * drawnScale - bound;
            if (!(excess > .1 && drawnScale > 0 && std::isfinite(excess))) return;
            const double overdraw = std::min(pixels, excess / drawnScale);
            const double leading = (align & firstFlag) ? 0. : (align & lastFlag) ? overdraw : overdraw / 2.;
            const int a = int(std::round(leading)), b = int(std::round(overdraw - leading));
            first += flipped ? b : a;
            last += flipped ? a : b;
        };
        extendCrop(double(obs_source_get_width(source)) - crop.left - crop.right,
                   std::hypot(draw.x.x, draw.x.y), std::hypot(box.x.x, box.x.y), scale.x < 0,
                   OBS_ALIGN_LEFT, OBS_ALIGN_RIGHT, crop.left, crop.right);
        extendCrop(double(obs_source_get_height(source)) - crop.top - crop.bottom,
                   std::hypot(draw.y.x, draw.y.y), std::hypot(box.y.x, box.y.y), scale.y < 0,
                   OBS_ALIGN_TOP, OBS_ALIGN_BOTTOM, crop.top, crop.bottom);
    }
    return {crop.left, crop.top, crop.right, crop.bottom};
}

bool findTransform(obs_scene_t *, obs_sceneitem_t *item, void *opaque)
{
    const auto &context = *static_cast<SearchContext *>(opaque);
    auto &match = *context.match;
    matrix4 draw{};
    obs_sceneitem_get_draw_transform(item, &draw);
    QTransform linear(draw.x.x, draw.x.y, draw.y.x, draw.y.y, 0, 0);
    linear *= context.parent;
    const bool selected = context.parentSelected || obs_sceneitem_selected(item);
    auto *source = obs_sceneitem_get_source(item);
    if (source == match.source && (!match.found || (selected && !match.selected))) {
        const bool valid = std::isfinite(linear.determinant()) && linear.isInvertible();
        match.transform = {valid ? linear : QTransform{}, itemCrop(item, source, draw), context.canvas};
        match.found = true;
        match.selected = selected;
    }
    if (context.depth < 16) {
        SearchContext child {&match, linear, context.canvas, context.depth + 1, selected};
        if (obs_sceneitem_is_group(item)) obs_sceneitem_group_enum_items(item, findTransform, &child);
        else if (auto *nested = obs_scene_from_source(source)) obs_scene_enum_items(nested, findTransform, &child);
    }
    return true;
}
}
SceneTransform sourceTransform(obs_source_t *source)
{
    if (!source) return {};
    SceneMatch match {source, {}};
    auto search = [&match](obs_source_t *sceneSource) {
        if (sceneSource) {
            SearchContext context {&match, {}, {double(obs_source_get_width(sceneSource)), double(obs_source_get_height(sceneSource))}};
            if (auto *scene = obs_scene_from_source(sceneSource)) obs_scene_enum_items(scene, findTransform, &context);
        }
        obs_source_release(sceneSource);
    };
    // In studio mode get_current_scene() is the PROGRAM scene, not the one being edited.
    search(obs_frontend_get_current_preview_scene());
    if (!match.found) search(obs_frontend_get_current_scene());
    return match.transform;
}
}
