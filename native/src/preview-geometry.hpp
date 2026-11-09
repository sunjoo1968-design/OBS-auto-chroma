#pragma once
#include "scene-transform.hpp"
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QTransform>
#include <algorithm>

namespace ac {
// All persisted selections stay in raw source coordinates, regardless of display rotation.
struct PreviewGeometry {
    QSizeF source {9, 16}, viewport;
    double percent = 0;
    QPointF pan;
    SceneTransform scene;

    QRectF visibleSource() const
    {
        const double left = std::clamp(double(scene.crop.left()), 0., source.width());
        const double top = std::clamp(double(scene.crop.top()), 0., source.height());
        const double right = std::clamp(source.width() - scene.crop.right(), left, source.width());
        const double bottom = std::clamp(source.height() - scene.crop.bottom(), top, source.height());
        return {left, top, right - left, bottom - top};
    }
    QRectF visibleNormalized() const
    {
        return QTransform::fromScale(1 / source.width(), 1 / source.height()).mapRect(visibleSource());
    }
    double scale() const
    {
        if (percent > 0) return percent / 100.;
        const auto bounds = scene.linear.mapRect(visibleSource());
        if (bounds.isEmpty()) return 1.;
        const auto fitted = bounds.size().expandedTo(scene.canvas);
        constexpr double margin = 12;
        return std::max(.000001, std::min((viewport.width() - 2 * margin) / fitted.width(),
                                          (viewport.height() - 2 * margin) / fitted.height()));
    }
    QTransform transform() const
    {
        const auto &item = scene.linear;
        const auto center = item.map(visibleSource().center());
        return item * QTransform::fromTranslate(-center.x(), -center.y())
            * QTransform::fromScale(scale(), scale())
            * QTransform::fromTranslate(viewport.width() / 2 + pan.x(), viewport.height() / 2 + pan.y());
    }
    QPointF normalizedAt(QPointF widgetPoint) const
    {
        const auto raw = transform().inverted().map(widgetPoint);
        return {raw.x() / source.width(), raw.y() / source.height()};
    }
    QPointF widgetAt(QPointF normalizedPoint) const
    {
        return transform().map(QPointF(normalizedPoint.x() * source.width(), normalizedPoint.y() * source.height()));
    }
    bool contains(QPointF widgetPoint) const
    {
        return visibleNormalized().contains(normalizedAt(widgetPoint));
    }
    void zoomAt(QPointF widgetPoint, double newPercent)
    {
        const auto anchor = normalizedAt(widgetPoint);
        percent = std::clamp(newPercent, 5., 800.);
        pan += widgetPoint - widgetAt(anchor);
    }
};
}
