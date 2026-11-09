// SunjooAn 0.5.1-dev. Synthetic, offscreen Qt tests; never starts OBS.
#include "preview-widget.hpp"
#include <QApplication>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QWheelEvent>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}
bool near(QPointF a, QPointF b) { return std::hypot(a.x() - b.x(), a.y() - b.y()) < .00001; }
void mouse(QWidget &widget, QEvent::Type type, QPointF point, Qt::MouseButton button, Qt::MouseButtons buttons,
           Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QMouseEvent event(type, point, point, button, buttons, modifiers);
    QApplication::sendEvent(&widget, &event);
}
QImage render(QWidget &widget)
{
    QImage image(widget.size(), QImage::Format_ARGB32); image.fill(Qt::transparent); widget.render(&image); return image;
}
}
int main(int argc, char **argv)
{
    // The OBS Qt SDK ships qminimal rather than qoffscreen. No window is shown.
    qputenv("QT_QPA_PLATFORM", "minimal");
    QApplication app(argc, argv);
    try {
        for (double angle : {0., 90., -90., 180., 270., 33.}) {
            ac::PreviewGeometry g {{1920, 1080}, {600, 900}, 0, {}, {QTransform().rotate(angle), {}, {}}};
            for (QPointF point : {QPointF(0, 0), QPointF(1, 1), QPointF(.2, .8), QPointF(.5, .5)})
                check(near(g.normalizedAt(g.widgetAt(point)), point), "rotation inverse mismatch");
            const auto bounds = g.transform().mapRect(QRectF(QPointF(), g.source));
            check(bounds.width() <= 600.0001 && bounds.height() <= 900.0001, "fit clips rotated frame");
            if (angle == 90 || angle == 270 || angle == -90)
                check(std::abs(bounds.width() / bounds.height() - 9. / 16.) < .000001, "portrait aspect lost");
            const auto cursor = g.widgetAt({.23, .81}); const auto anchor = g.normalizedAt(cursor);
            g.zoomAt(cursor, 175.3);
            check(near(g.normalizedAt(cursor), anchor), "zoom moves cursor anchor");
            g.zoomAt(cursor, 9000); check(g.percent == 800, "upper zoom clamp");
            g.zoomAt(cursor, .001); check(g.percent == 5, "lower zoom clamp");
        }
        const ac::SceneTransform portrait{QTransform().rotate(90), {}, {}};
        ac::PreviewGeometry rotated {{1920, 1080}, {600, 900}, 0, {}, portrait};
        check(rotated.widgetAt({0, 0}).x() > rotated.widgetAt({0, 1}).x(), "90 degree direction reversed");
        ac::PreviewGeometry empty; empty.viewport = {600, 900};
        const auto emptyBounds = empty.transform().mapRect(QRectF(QPointF(), empty.source));
        check(std::abs(emptyBounds.width() / emptyBounds.height() - 9. / 16.) < .000001, "empty view is not 9:16");

        ac::Preview preview; preview.original.width = preview.processed.width = 160;
        preview.original.height = preview.processed.height = 90;
        preview.original.rgba.resize(160 * 90 * 4); preview.processed.rgba.resize(160 * 90 * 4);
        for (int y = 0; y < 90; ++y) for (int x = 0; x < 160; ++x) {
            const int i = (y * 160 + x) * 4;
            preview.original.rgba[i] = uint8_t(x); preview.original.rgba[i + 1] = uint8_t(y);
            preview.original.rgba[i + 2] = 40; preview.original.rgba[i + 3] = 255;
            preview.processed.rgba[i] = 80; preview.processed.rgba[i + 1] = 120;
            preview.processed.rgba[i + 2] = 180; preview.processed.rgba[i + 3] = 96;
        }
        ac::PreviewWidget widget; widget.resize(512, 700); widget.setFrames(preview); widget.setSceneTransform(portrait); widget.setEditRegion(5);
        widget.setView(2);
        ac::PreviewGeometry g {{160, 90}, QSizeF(widget.size()), 0, {}, portrait};
        const auto probe = g.widgetAt({.65, .65}).toPoint();
        auto isAlpha = [&] { const auto c = render(widget).pixelColor(probe); return c.red() == 96 && c.green() == 96 && c.blue() == 96; };
        check(isAlpha(), "alpha view unavailable");
        QJsonObject stroke; widget.strokeCompleted = [&](QJsonObject value) { stroke = value; };
        const auto begin = g.widgetAt({.2, .3}), end = g.widgetAt({.4, .3});
        mouse(widget, QEvent::MouseButtonPress, begin, Qt::LeftButton, Qt::LeftButton);
        check(isAlpha(), "alpha switches to raw during stroke");
        mouse(widget, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
        mouse(widget, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
        check(isAlpha(), "alpha lost after stroke");
        check(!stroke.isEmpty(), "stroke not emitted");
        const auto xy = stroke["points"].toArray()[0].toArray();
        check(near({xy[0].toDouble(), xy[1].toDouble()}, {.2, .3}), "rotated stroke stores screen coordinates");
        const auto rgb = stroke["samples"].toArray()[0].toArray();
        check(std::abs(rgb[0].toInt() - 32) <= 1 && std::abs(rgb[1].toInt() - 27) <= 1 && rgb[2].toInt() == 40,
              "alpha stroke does not sample raw RGB");

        widget.setView(1); const auto colorBefore = render(widget).pixelColor(probe);
        mouse(widget, QEvent::MouseButtonPress, begin, Qt::LeftButton, Qt::LeftButton);
        check(render(widget).pixelColor(probe) == colorBefore, "color result switches to raw during stroke");
        mouse(widget, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
        check(render(widget).pixelColor(probe) == colorBefore, "color result lost after stroke");

        widget.setEditRegion(1); ac::Rect region; widget.regionChanged = [&](int, ac::Region value) { region = value.bounds; };
        mouse(widget, QEvent::MouseButtonPress, begin, Qt::LeftButton, Qt::LeftButton);
        mouse(widget, QEvent::MouseButtonRelease, g.widgetAt({.6, .7}), Qt::LeftButton, Qt::NoButton);
        check(std::abs(region.x - .2) < .00001 && std::abs(region.y - .3) < .00001 && std::abs(region.width - .4) < .00001 && std::abs(region.height - .4) < .00001,
              "rotated region mapping incorrect");

        widget.setEditRegion(5); stroke = {}; const double initialZoom = widget.zoomPercent(); double reported = 0;
        widget.zoomChanged = [&](double value) { reported = value; };
        QWheelEvent wheel(begin, begin, {}, {0, 120}, Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(&widget, &wheel);
        check(widget.zoomPercent() > initialZoom && std::abs(reported - widget.zoomPercent()) < 1e-8, "wheel zoom not applied");
        mouse(widget, QEvent::MouseButtonPress, begin, Qt::LeftButton, Qt::LeftButton);
        mouse(widget, QEvent::MouseButtonRelease, begin, Qt::LeftButton, Qt::NoButton);
        const auto wheelXY = stroke["points"].toArray()[0].toArray();
        check(near({wheelXY[0].toDouble(), wheelXY[1].toDouble()}, {.2, .3}), "wheel cursor anchor changed");
        const QPointF delta(45, -30);
        mouse(widget, QEvent::MouseButtonPress, begin, Qt::MiddleButton, Qt::MiddleButton);
        mouse(widget, QEvent::MouseMove, begin + delta, Qt::NoButton, Qt::MiddleButton);
        mouse(widget, QEvent::MouseButtonRelease, begin + delta, Qt::MiddleButton, Qt::NoButton);
        stroke = {};
        mouse(widget, QEvent::MouseButtonPress, begin + delta, Qt::LeftButton, Qt::LeftButton);
        mouse(widget, QEvent::MouseButtonRelease, begin + delta, Qt::LeftButton, Qt::NoButton);
        const auto panXY = stroke["points"].toArray()[0].toArray();
        check(near({panXY[0].toDouble(), panXY[1].toDouble()}, {.2, .3}), "middle drag pan mapping incorrect");
        widget.setZoom(0);
        ac::SceneTransform scene{QTransform::fromScale(.5, 1.25), {16, 9, 16, 9}, {}};
        widget.setSceneTransform(scene);
        g = {{160, 90}, QSizeF(widget.size()), 0, {}, scene};
        check(!g.contains(g.widgetAt({.05, .4})), "hidden crop must not accept color samples");
        check(near(g.normalizedAt(g.widgetAt({.2, .8})), {.2, .8}), "cropped scale inverse mismatch");
        widget.setEditRegion(2);
        ac::Region mask;
        widget.regionChanged = [&](int, ac::Region value) { mask = value; };
        auto drag = [&](QPointF start, QPointF end, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
            mouse(widget, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton, modifiers);
            mouse(widget, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton, modifiers);
        };
        drag(g.widgetAt({.05, .09}), g.widgetAt({.5, .6}));
        check(mask.valid() && near({mask.bounds.x, mask.bounds.y}, {.1, .1}) && mask.outline.isEmpty(),
              "outside drag did not select up to cropped edges");
        widget.setRegionTool(ac::RegionTool::Ellipse);
        drag(g.widgetAt({.05, .09}), g.widgetAt({.75, .7}));
        check(mask.outline.size() > 4 && near({mask.bounds.x, mask.bounds.y}, {.1, .1}),
              "ellipse was not clipped to cropped edges");
        widget.setRegionTool(ac::RegionTool::Circle);
        const auto start = g.widgetAt({.3, .3});
        drag(start, start + QPointF(45, 30));
        QPolygonF display;
        for (const auto &point : mask.outline) display << g.widgetAt(point);
        check(near({display.boundingRect().width(), display.boundingRect().height()}, {45, 45}),
              "circle must stay round after OBS nonuniform scaling");
        widget.setRegionTool(ac::RegionTool::Pen);
        for (const auto &point : {QPointF(.05, .3), QPointF(.55, .2), QPointF(.55, .7), QPointF(.05, .65)})
            drag(g.widgetAt(point), g.widgetAt(point));
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(&widget, &enter);
        check(mask.outline.size() == 4 && std::abs(mask.bounds.x - .1) < .00001, "pen polygon clipping failed");
        const QPointF first(.25, .28), tangent(.10, -.08);
        drag(g.widgetAt(first), g.widgetAt(first + tangent));
        for (const QPointF point : {QPointF(.65, .30), QPointF(.62, .65), QPointF(.27, .68)})
            drag(g.widgetAt(point), g.widgetAt(point));
        const auto nearStart = g.widgetAt(first) + QPointF(8, 0);
        drag(nearStart, nearStart);
        check(mask.outline.size() > 4 && mask.controls.anchors.size() == 4 &&
              near(mask.controls.handles[0], tangent), "pen drag or near-start closure failed");
        auto savedMask = ac::readRegion(ac::regionPoints(mask), mask.bounds);
        savedMask.controls = ac::decodeControls(ac::encodeControls(mask.controls));
        check(savedMask.controls.anchors.size() == 4 && near(savedMask.controls.handles[0], tangent),
              "saved curve lost its control points");
        widget.setRegions({0, 0, 1, 1}, savedMask, {}, {});
        widget.setExistingRegionEdit(true);
        const QPointF newTangent(.12, -.04);
        drag(g.widgetAt(first + tangent), g.widgetAt(first + newTangent));
        check(near(mask.controls.handles[0], newTangent) && near(mask.controls.anchors[0], first),
              "reopened curve handle did not move independently of the anchor");
        const QPointF moved(.29, .29);
        drag(g.widgetAt(first), g.widgetAt(moved));
        check(near(mask.controls.anchors[0], moved) && near(mask.controls.handles[0], newTangent),
              "anchor edit discarded its tangent");
        check(near(mask.controls.incomingAt(0), -newTangent), "ordinary drag did not move both handles");
        const QPointF independentOutgoing(.16, -.09);
        drag(g.widgetAt(moved + newTangent), g.widgetAt(moved + independentOutgoing), Qt::AltModifier);
        check(near(mask.controls.handles[0], independentOutgoing) && near(mask.controls.incomingAt(0), -newTangent),
              "Alt drag of outgoing handle changed the incoming handle");
        const QPointF independentIncoming(-.10, .06);
        drag(g.widgetAt(moved - newTangent), g.widgetAt(moved + independentIncoming), Qt::AltModifier);
        check(near(mask.controls.handles[0], independentOutgoing) && near(mask.controls.incomingAt(0), independentIncoming),
              "Alt drag of incoming handle changed the outgoing handle");
        const auto restored = ac::decodeControls(ac::encodeControls(mask.controls));
        check(near(restored.handles[0], independentOutgoing) && near(restored.incomingAt(0), independentIncoming),
              "independent handles were lost during save and reload");
        const auto legacy = ac::decodeControls(QJsonObject{{"tool", int(ac::RegionTool::Pen)},
            {"nodes", QJsonArray{QJsonArray{.2,.2,.05,.03}, QJsonArray{.6,.3,0,0}, QJsonArray{.6,.7,0,0}}}});
        check(legacy.anchors.size() == 3 && near(legacy.incomingAt(0), {-.05, -.03}),
              "version 1.2 tangent was not migrated");
        const auto next = mask.controls.anchors[1];
        const auto midpoint = (moved + next) * .5 + independentOutgoing * .375;
        drag(g.widgetAt(midpoint), g.widgetAt(midpoint));
        check(mask.controls.anchors.size() == 5 && near(mask.controls.anchors[1], midpoint) &&
              near(mask.controls.handles[0], independentOutgoing / 2) &&
              near(mask.controls.incomingAt(0), independentIncoming),
              "curve insertion did not preserve the subdivided curve and unrelated handle");
        QKeyEvent eraseNode(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
        QApplication::sendEvent(&widget, &eraseNode);
        check(mask.controls.anchors.size() == 4 && near(mask.controls.anchors[1], next),
              "Delete did not remove the inserted anchor");
        for (int i = 0; i < 2; ++i) {
            const auto point = g.widgetAt(mask.controls.anchors.last());
            drag(point, point);
            QApplication::sendEvent(&widget, &eraseNode);
        }
        check(mask.controls.anchors.size() == 3, "a closed mask must retain at least three anchors");
        for (const auto tool : {ac::RegionTool::Rectangle, ac::RegionTool::Circle, ac::RegionTool::Ellipse}) {
            widget.setExistingRegionEdit(false);
            widget.setRegionTool(tool);
            const auto origin = g.widgetAt({.32, .32});
            drag(origin, origin + QPointF(42, 38));
            const auto previous = mask.bounds;
            const auto &a = mask.controls.anchors;
            const auto corner = g.widgetAt(a[1] + a[2] - a[0]);
            widget.setExistingRegionEdit(true);
            drag(corner, corner + QPointF(14, 10));
            check(mask.bounds.width > previous.width && mask.bounds.height > previous.height &&
                  mask.controls.tool == tool, "shape corner resize failed");
            const auto b = mask.bounds;
            const auto center = g.widgetAt({b.x + b.width / 2, b.y + b.height / 2});
            drag(center, center + QPointF(8, 6));
            check(mask.bounds.x > b.x && mask.bounds.y > b.y, "shape interior drag did not move it");
            const auto shape = mask.controls.anchors;
            const auto edge = tool == ac::RegionTool::Rectangle
                ? (shape[0] + shape[1]) / 2
                : (shape[0] + shape[1]) / 2 + (shape[2] - shape[0]) * (.5 + std::sqrt(.125))
                  + (shape[1] - shape[0]) * std::sqrt(.125);
            drag(g.widgetAt(edge), g.widgetAt(edge));
            check(mask.controls.tool == ac::RegionTool::Pen && mask.controls.anchors.size() == 5,
                  "adding an anchor did not convert the primitive to a pen path");
        }
        std::cout << "PASS: rotation fit/inverse (6 angles), zoom anchor/clamps, portrait fallback, alpha/color stroke display, raw sampling, region, wheel and middle pan\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
