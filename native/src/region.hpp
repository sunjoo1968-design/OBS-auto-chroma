#pragma once
#include "ac-types.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPolygonF>
#include <cmath>

namespace ac {
inline constexpr int MaxRegionVertices = 128;
enum class RegionTool { Rectangle, Circle, Ellipse, Pen };
struct RegionControls {
    RegionTool tool = RegionTool::Rectangle;
    QPolygonF anchors;
    QPolygonF handles;  // Outgoing offsets.
    QPolygonF incoming; // Incoming offsets. Older paths mirror the outgoing side.
    QPointF incomingAt(qsizetype i) const { return incoming.value(i, -handles.value(i)); }
    void fillHandles()
    {
        handles.resize(anchors.size());
        while (incoming.size() < anchors.size()) incoming << -handles[incoming.size()];
    }
};
struct Region {
    Rect bounds;
    QPolygonF outline;
    RegionControls controls;
    bool valid() const { return bounds.width > 0 && bounds.height > 0; }
};

inline QJsonObject encodeControls(const RegionControls &controls)
{
    QJsonArray nodes;
    for (qsizetype i = 0; i < controls.anchors.size(); ++i) {
        const auto p = controls.anchors[i];
        const auto h = controls.handles.value(i);
        const auto in = controls.incomingAt(i);
        auto round = [](double v) { return std::round(v * 1e7) / 1e7; };
        nodes.append(QJsonArray{round(p.x()), round(p.y()), round(h.x()), round(h.y()), round(in.x()), round(in.y())});
    }
    return {{"tool", int(controls.tool)}, {"nodes", nodes}};
}

inline RegionControls decodeControls(const QJsonValue &value)
{
    const auto object = value.toObject();
    const auto nodes = object["nodes"].toArray();
    const int tool = object["tool"].toInt(-1);
    if (tool < 0 || tool > int(RegionTool::Pen) || nodes.size() < 3 || nodes.size() > MaxRegionVertices ||
        (tool != int(RegionTool::Pen) && nodes.size() != 3)) return {};
    RegionControls controls;
    controls.tool = RegionTool(tool);
    for (const auto &node : nodes) {
        const auto a = node.toArray();
        if (a.size() != 4 && a.size() != 6) return {};
        for (const auto &v : a)
            if (!v.isDouble() || !std::isfinite(v.toDouble()) || std::abs(v.toDouble()) > 10000) return {};
        controls.anchors << QPointF(a[0].toDouble(), a[1].toDouble());
        controls.handles << QPointF(a[2].toDouble(), a[3].toDouble());
        controls.incoming << (a.size() == 6 ? QPointF(a[4].toDouble(), a[5].toDouble()) : -controls.handles.last());
    }
    return controls;
}

inline Region polygonRegion(const QPolygonF &points)
{
    if (points.size() < 3 || points.size() > MaxRegionVertices) return {};
    const auto box = points.boundingRect();
    return {{box.x(), box.y(), box.width(), box.height()}, points};
}

inline Region readRegion(const QJsonValue &value, Rect rectangle)
{
    if (value.isUndefined() || (value.isArray() && value.toArray().isEmpty())) return {rectangle, {}};
    if (!value.isArray()) return {};
    const auto points = value.toArray();
    if (points.size() > MaxRegionVertices) return {};
    QPolygonF polygon;
    for (const auto &point : points) {
        const auto xy = point.toArray();
        if (xy.size() != 2) return {};
        const double x = xy[0].toDouble(-1), y = xy[1].toDouble(-1);
        if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || x > 1 || y < 0 || y > 1) return {};
        polygon << QPointF(x, y);
    }
    return polygonRegion(polygon);
}

inline QJsonArray regionPoints(const Region &region)
{
    QJsonArray points;
    for (const auto &p : region.outline)
        points.append(QJsonArray{std::round(p.x() * 1e7) / 1e7, std::round(p.y() * 1e7) / 1e7});
    return points;
}

inline Region parseRegion(const QByteArray &json, Rect rectangle)
{
    if (json.isEmpty()) return {rectangle, {}};
    if (json.size() > 8192) return {};
    const auto document = QJsonDocument::fromJson(json);
    return document.isArray() ? readRegion(document.array(), rectangle) : Region{};
}
}
