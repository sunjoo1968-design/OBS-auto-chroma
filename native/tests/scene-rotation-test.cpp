// SunjooAn 1.0. Tests production scene lookup with OBS API doubles; no OBS runtime.
#include "scene-rotation.hpp"
#include <obs-frontend-api.h>
#include <graphics/matrix4.h>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

struct obs_scene { std::vector<obs_sceneitem_t *> items; };
struct obs_source { obs_scene_t *scene = nullptr; int refs = 0; };
struct obs_scene_item {
    obs_source_t *source;
    float rotation = 0;
    bool selected = false;
    obs_scene_t *group = nullptr;
    obs_sceneitem_crop crop{};
    double scaleX = 1, scaleY = 1;
    QSizeF bounds;
};
namespace {
bool studio = true;
obs_source_t *preview = nullptr, *program = nullptr;
obs_source_t *retain(obs_source_t *source) { if (source) ++source->refs; return source; }
void check(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
}
extern "C" {
obs_source_t *obs_frontend_get_current_scene() { return retain(program); }
obs_source_t *obs_frontend_get_current_preview_scene() { return studio ? retain(preview) : nullptr; }
bool obs_frontend_preview_program_mode_active() { return studio; }
void obs_source_release(obs_source_t *source) { if (source) --source->refs; }
obs_scene_t *obs_scene_from_source(const obs_source_t *source) { return source ? source->scene : nullptr; }
void obs_scene_enum_items(obs_scene_t *scene, bool (*callback)(obs_scene_t *, obs_sceneitem_t *, void *), void *data)
{
    if (scene) for (auto *item : scene->items) if (!callback(scene, item, data)) break;
}
void obs_sceneitem_get_draw_transform(const obs_sceneitem_t *item, matrix4 *matrix)
{
    const auto rotation = QTransform().rotate(item->rotation);
    matrix->x.x = float(rotation.m11() * item->scaleX);
    matrix->x.y = float(rotation.m12() * item->scaleX);
    matrix->y.x = float(rotation.m21() * item->scaleY);
    matrix->y.y = float(rotation.m22() * item->scaleY);
}
void obs_sceneitem_get_crop(const obs_sceneitem_t *item, obs_sceneitem_crop *crop) { *crop = item->crop; }
enum obs_bounds_type obs_sceneitem_get_bounds_type(const obs_sceneitem_t *item) { return item->bounds.isValid() ? OBS_BOUNDS_SCALE_OUTER : OBS_BOUNDS_NONE; }
bool obs_sceneitem_get_bounds_crop(const obs_sceneitem_t *item) { return item->bounds.isValid(); }
uint32_t obs_sceneitem_get_bounds_alignment(const obs_sceneitem_t *) { return OBS_ALIGN_CENTER; }
void obs_sceneitem_get_box_transform(const obs_sceneitem_t *item, matrix4 *matrix)
{
    matrix->x.x = float(item->bounds.width());
    matrix->y.y = float(item->bounds.height());
}
void obs_sceneitem_get_scale(const obs_sceneitem_t *item, vec2 *scale)
{
    scale->x = float(item->scaleX);
    scale->y = float(item->scaleY);
}
uint32_t obs_source_get_width(obs_source_t *) { return 1920; }
uint32_t obs_source_get_height(obs_source_t *) { return 1080; }
bool obs_sceneitem_selected(const obs_sceneitem_t *item) { return item->selected; }
obs_source_t *obs_sceneitem_get_source(const obs_sceneitem_t *item) { return item->source; }
bool obs_sceneitem_is_group(obs_sceneitem_t *item) { return item->group != nullptr; }
void obs_sceneitem_group_enum_items(obs_sceneitem_t *item, bool (*callback)(obs_scene_t *, obs_sceneitem_t *, void *), void *data)
{
    obs_scene_enum_items(item->group, callback, data);
}
}
int rotation(obs_source_t *source)
{
    const auto transform = ac::sourceTransform(source).linear;
    return int(std::round(std::atan2(transform.m12(), transform.m11()) * 180. / 3.141592653589793));
}
int main()
{
    try {
        obs_source_t camera, other;
        obs_sceneitem_t previewCamera {&camera, 90, true}, programOther {&other};
        obs_scene_t previewScene {{&previewCamera}}, programScene {{&programOther}};
        obs_source_t previewSource {&previewScene}, programSource {&programScene};
        preview = &previewSource; program = &programSource;
        check(rotation(&camera) == 90, "studio preview camera rotation is ignored (program scene has no camera)");
        check(previewSource.refs == 0 && programSource.refs == 0, "scene reference leaked");
        previewCamera.crop = {100, 20, 40, 80};
        previewCamera.scaleX = .5; previewCamera.scaleY = .75;
        auto geometry = ac::sourceTransform(&camera);
        check(geometry.crop == QMargins(100, 20, 40, 80) && geometry.linear.map(QPointF(10, 20)) == QPointF(-15, 5),
              "preview item crop and scale are not carried through the rotation");
        previewCamera.bounds = {700, 735};
        check(ac::sourceTransform(&camera).crop == QMargins(290, 20, 230, 80), "bounds crop must be added to explicit crop");
        previewCamera.bounds = {};
        previewCamera.scaleX = previewCamera.scaleY = 1;

        obs_sceneitem_t programCamera {&camera, -90, true}; programScene.items = {&programCamera};
        check(rotation(&camera) == 90, "program rotation overrides preview rotation");
        previewCamera.rotation = 0;
        check(rotation(&camera) == 0, "valid zero preview rotation falls back to program");
        previewScene.items.clear();
        check(rotation(&camera) == -90, "missing preview camera does not fall back to program");
        studio = false; previewScene.items = {&previewCamera};
        check(rotation(&camera) == -90, "normal mode must use active scene");
        studio = true;
        obs_sceneitem_t duplicate {&camera, 270, true}; previewCamera.rotation = 0; previewCamera.selected = false;
        previewScene.items = {&previewCamera, &duplicate};
        check(rotation(&camera) == -90, "selected duplicate does not win");
        duplicate.selected = false;
        check(rotation(&camera) == 0, "unselected duplicate must keep first match");
        obs_scene_t groupScene {{&duplicate}}; obs_sceneitem_t group {&other, 90, false, &groupScene};
        duplicate.rotation = 90; previewScene.items = {&group};
        check(std::abs(rotation(&camera)) == 180, "group rotation not accumulated");
        obs_source_t nestedSource {&groupScene}; obs_sceneitem_t nested {&nestedSource, -90}; previewScene.items = {&nested};
        check(rotation(&camera) == 0, "nested scene rotation not accumulated");
        duplicate.rotation = std::numeric_limits<float>::quiet_NaN();
        check(rotation(&camera) == 0, "nonfinite rotation not sanitized");
        duplicate.rotation = 90;
        check(previewSource.refs == 0 && programSource.refs == 0, "scene references not balanced");
        preview = nullptr; check(rotation(&camera) == -90, "unavailable preview fallback broken");
        program = nullptr; check(rotation(&camera) == 0, "missing scenes must show raw orientation");
        std::cout << "PASS: studio preview priority, zero/missing fallback, normal mode, duplicate selection, group/nested rotation, finite values and references\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
