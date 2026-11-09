#include "preview-widget.hpp"
#include <QKeyEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>
#include <array>
#include <cmath>
#include <algorithm>

namespace ac {
static QImage imageFromFrame(const Frame &frame)
{
    if (!frame.valid()) return {};
    return QImage(frame.rgba.data(), int(frame.width), int(frame.height), int(frame.width * 4),
                  QImage::Format_RGBA8888).copy();
}
PreviewWidget::PreviewWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(280, 240);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setToolTip(QStringLiteral("OBS 장면의 회전·자르기·크기를 반영합니다. 휠로 확대·축소하고 휠 버튼 드래그로 이동합니다. 펜은 점을 찍고 두 번 클릭하거나 Enter로 닫습니다."));
}
void PreviewWidget::setFrames(const Preview &preview)
{
    if (!preview.original.valid() || (!original_.isNull() &&
        (original_.width() != int(preview.original.width) || original_.height() != int(preview.original.height)))) cancelInteraction();
    // Keep the exact raw image being sampled stable for the duration of a stroke.
    if (dragging_ && edit_ >= 5) return;
    original_ = imageFromFrame(preview.original);
    processed_ = imageFromFrame(preview.processed);
    update();
}
void PreviewWidget::setView(int index) { view_ = index; update(); }
void PreviewWidget::setBackground(const QImage &image) { background_ = image; update(); }
void PreviewWidget::setRegions(Rect a, const Region &p, const Region &e, const Region &l)
{
    regions_[0] = {a, {}}; regions_[1] = p; regions_[2] = e; regions_[3] = l; update();
}
void PreviewWidget::setRegionTool(RegionTool tool) { cancelInteraction(); regionTool_ = tool; }
void PreviewWidget::setExistingRegionEdit(bool enabled) { cancelInteraction(); editExisting_ = enabled; update(); }
void PreviewWidget::setEditRegion(int index)
{
    cancelInteraction();
    if (index < 2 || index > 4) editExisting_ = false;
    edit_ = index; setCursor(index ? Qt::CrossCursor : Qt::ArrowCursor); update();
}
void PreviewWidget::setStrokes(const QJsonArray &strokes) { strokes_ = strokes; update(); }
void PreviewWidget::setSelectedStroke(int index) { selectedStroke_ = index; update(); }
void PreviewWidget::setZoom(double percent)
{
    cancelInteraction(); zoom_ = percent > 0 ? std::clamp(percent, 5., 800.) : 0; pan_ = {}; update();
}
void PreviewWidget::setSceneTransform(const SceneTransform &scene)
{
    if (scene_.linear == scene.linear && scene_.crop == scene.crop && scene_.canvas == scene.canvas) return;
    cancelInteraction();
    scene_ = scene;
    pan_ = {};
    update();
}
double PreviewWidget::zoomPercent() const { return geometry().scale() * 100.; }
void PreviewWidget::cancelInteraction()
{
    dragging_ = panning_ = placingPen_ = editClickPending_ = false;
    selectedEditNode_ = -1;
    strokePoints_.clear(); pen_.anchors.clear(); pen_.handles.clear(); pen_.incoming.clear(); strokeFrame_ = {};
    setCursor(edit_ ? Qt::CrossCursor : Qt::ArrowCursor); update();
}
PreviewGeometry PreviewWidget::geometry() const
{
    return {original_.isNull() ? QSizeF(9, 16) : QSizeF(original_.size()), QSizeF(size()),
            zoom_, pan_, original_.isNull() ? SceneTransform{} : scene_};
}
QPointF PreviewWidget::normalized(QPointF p) const
{
    const auto g = geometry();
    const auto n = g.normalizedAt(p);
    const auto clip = g.visibleNormalized();
    return {std::clamp(n.x(), clip.left(), clip.right()), std::clamp(n.y(), clip.top(), clip.bottom())};
}

Region PreviewWidget::draftRegion() const
{
    if (editExisting_) return buildRegion(editDraft_);
    const auto g = geometry();
    const auto tool = edit_ == 1 ? RegionTool::Rectangle : regionTool_;
    if (tool == RegionTool::Pen) return buildRegion(pen_);
    QRectF box = QRectF(start_, end_).normalized();
    if (tool == RegionTool::Circle) {
        const double side = std::max(box.width(), box.height());
        box = QRectF(start_, start_ + QPointF(end_.x() < start_.x() ? -side : side,
                                             end_.y() < start_.y() ? -side : side)).normalized();
    }
    if (box.isEmpty()) return {};
    RegionControls controls;
    controls.tool = tool;
    controls.anchors << g.normalizedAt(box.topLeft()) << g.normalizedAt(box.topRight())
                     << g.normalizedAt(box.bottomLeft());
    return buildRegion(controls);
}

Region PreviewWidget::buildRegion(const RegionControls &controls) const
{
    const auto visible = geometry().visibleNormalized();
    if (visible.isEmpty() || controls.anchors.size() < 3) return {};
    QPolygonF polygon;
    const auto &nodes = controls.anchors;
    if (controls.tool == RegionTool::Pen) {
        int curves = 0;
        for (qsizetype i = 0; i < nodes.size(); ++i)
            if (!controls.handles.value(i).isNull() || !controls.incomingAt((i + 1) % nodes.size()).isNull()) ++curves;
        const int steps = std::clamp(1 + (112 - int(nodes.size())) / std::max(1, curves), 1, 32);
        for (qsizetype i = 0; i < nodes.size(); ++i) {
            const auto next = (i + 1) % nodes.size();
            const auto a = nodes[i], b = nodes[next];
            const auto u = controls.handles.value(i), v = controls.incomingAt(next);
            polygon << a;
            if (u.isNull() && v.isNull()) continue;
            for (int k = 1; k < steps; ++k) {
                const double t = double(k) / steps, s = 1 - t;
                polygon << (s*s*s*a + 3*s*s*t*(a+u) + 3*s*t*t*(b+v) + t*t*t*b);
            }
        }
    } else {
        const auto origin = nodes[0], xAxis = nodes[1] - origin, yAxis = nodes[2] - origin;
        if (std::abs(xAxis.x() * yAxis.y() - xAxis.y() * yAxis.x()) < 1e-14) return {};
        if (controls.tool == RegionTool::Rectangle) {
            polygon << origin << nodes[1] << origin + xAxis + yAxis << nodes[2];
            const bool axisAligned = (std::abs(xAxis.x()) < 1e-8 && std::abs(yAxis.y()) < 1e-8) ||
                                     (std::abs(xAxis.y()) < 1e-8 && std::abs(yAxis.x()) < 1e-8);
            if (axisAligned || edit_ == 1) {
                const auto r = polygon.boundingRect().intersected(visible);
                return {{r.x(), r.y(), r.width(), r.height()}, {}, controls};
            }
        } else {
            for (int i = 0; i < 96; ++i) {
                const double angle = i * (2 * 3.141592653589793 / 96);
                polygon << origin + xAxis * (.5 + .5 * std::cos(angle)) + yAxis * (.5 + .5 * std::sin(angle));
            }
        }
    }
    if (polygon.size() < 3) return {};
    QPainterPath path, clip;
    path.addPolygon(polygon);
    path.closeSubpath();
    clip.addRect(visible);
    if (!visible.contains(polygon.boundingRect())) polygon = path.intersected(clip).toFillPolygon();
    if (polygon.size() > 1 && polygon.first() == polygon.last()) polygon.removeLast();
    for (auto &point : polygon)
        point = {std::clamp(point.x(), visible.left(), visible.right()), std::clamp(point.y(), visible.top(), visible.bottom())};
    auto region = polygonRegion(polygon);
    region.controls = controls;
    return region;
}

RegionControls PreviewWidget::existingControls() const
{
    if (edit_ < 2 || edit_ > 4) return {};
    const auto &region = regions_[edit_ - 1];
    if (!region.valid()) return {};
    if (!region.controls.anchors.isEmpty()) return region.controls;
    if (region.outline.size() == 96) {
        // Version 1.1 stored untouched ellipses as 96 regularly spaced vertices.
        const auto center = (region.outline[0] + region.outline[48]) / 2;
        const auto x = region.outline[0] - center, y = region.outline[24] - center;
        bool ellipse = true;
        for (int i = 0; i < 96 && ellipse; ++i) {
            const double angle = i * (2 * 3.141592653589793 / 96);
            const auto error = region.outline[i] - center - x * std::cos(angle) - y * std::sin(angle);
            ellipse = std::hypot(error.x(), error.y()) < 1e-6;
        }
        if (ellipse) return {RegionTool::Ellipse, QPolygonF{center-x-y, center+x-y, center-x+y}, {}};
    }
    if (!region.outline.isEmpty()) return {RegionTool::Pen, region.outline, {}};
    const auto r = region.bounds;
    return {RegionTool::Rectangle, QPolygonF{{r.x,r.y}, {r.x+r.width,r.y}, {r.x,r.y+r.height}}, {}};
}

static QPolygonF controlCorners(const RegionControls &controls)
{
    const auto &a = controls.anchors;
    if (controls.tool == RegionTool::Pen || a.size() != 3) return a;
    return QPolygonF{a[0], a[1], a[1] + a[2] - a[0], a[2]};
}

void PreviewWidget::beginRegionEdit(QPointF position)
{
    editStart_ = existingControls();
    if (editStart_.anchors.isEmpty()) return;
    const auto g = geometry();
    const auto corners = controlCorners(editStart_);
    editNode_ = -1; editPart_ = 0;
    independentHandle_ = false;
    editClickPending_ = true;
    start_ = position;
    double closest = 9;
    auto hit = [&](QPointF point, int index, int part) {
        const double distance = QLineF(position, g.widgetAt(point)).length();
        if (distance < closest) { closest = distance; editNode_ = index; editPart_ = part; }
    };
    for (int i = 0; i < corners.size(); ++i) {
        hit(corners[i], i, 0);
        if (editStart_.tool != RegionTool::Pen) continue;
        const auto outgoing = editStart_.handles.value(i), incoming = editStart_.incomingAt(i);
        if (!outgoing.isNull()) hit(corners[i] + outgoing, i, 1);
        if (!incoming.isNull()) hit(corners[i] + incoming, i, -1);
    }
    selectedEditNode_ = editNode_;
    update();
    if (editNode_ < 0) {
        const auto &region = regions_[edit_ - 1];
        const auto r = region.bounds;
        const auto point = g.normalizedAt(position);
        if (!QRectF(r.x, r.y, r.width, r.height).contains(point) ||
            (!region.outline.isEmpty() && !region.outline.containsPoint(point, Qt::OddEvenFill))) return;
    }
    editDraft_ = editStart_;
    dragging_ = true;
    update();
}

void PreviewWidget::moveRegionEdit(QPointF position, Qt::KeyboardModifiers modifiers)
{
    const auto g = geometry();
    const auto delta = g.normalizedAt(position) - g.normalizedAt(start_);
    editDraft_ = editStart_;
    if (editNode_ < 0) {
        for (auto &point : editDraft_.anchors) point += delta;
    } else if (editStart_.tool == RegionTool::Pen) {
        if (editPart_ == 0) editDraft_.anchors[editNode_] += delta;
        else {
            editDraft_.fillHandles();
            independentHandle_ = independentHandle_ || modifiers.testFlag(Qt::AltModifier);
            auto &side = editPart_ > 0 ? editDraft_.handles[editNode_] : editDraft_.incoming[editNode_];
            auto &other = editPart_ > 0 ? editDraft_.incoming[editNode_] : editDraft_.handles[editNode_];
            side += delta;
            if (!independentHandle_) other = -side;
        }
    } else {
        const auto &a = editStart_.anchors;
        const auto x = a[1] - a[0], y = a[2] - a[0];
        const QTransform basis(x.x(), x.y(), y.x(), y.y(), a[0].x(), a[0].y());
        if (!basis.isInvertible()) return;
        const auto corners = controlCorners(editStart_);
        const auto inverse = basis.inverted();
        const auto fixed = inverse.map(corners[(editNode_ + 2) % 4]);
        auto moving = inverse.map(corners[editNode_] + delta);
        if (editStart_.tool == RegionTool::Circle) {
            const double side = std::max(std::abs(moving.x()-fixed.x()), std::abs(moving.y()-fixed.y()));
            moving = fixed + QPointF(std::copysign(side, moving.x()-fixed.x()), std::copysign(side, moving.y()-fixed.y()));
        }
        const auto box = QRectF(fixed, moving).normalized();
        editDraft_.anchors = QPolygonF{basis.map(box.topLeft()), basis.map(box.topRight()), basis.map(box.bottomLeft())};
    }
    update();
}

static RegionControls asPen(const RegionControls &shape)
{
    if (shape.tool == RegionTool::Pen) return shape;
    RegionControls path{RegionTool::Pen, {}, {}, {}};
    if (shape.tool == RegionTool::Rectangle) {
        path.anchors = controlCorners(shape);
        return path;
    }
    const auto x = (shape.anchors[1] - shape.anchors[0]) / 2;
    const auto y = (shape.anchors[2] - shape.anchors[0]) / 2;
    const auto center = shape.anchors[0] + x + y;
    constexpr double k = .5522847498307936; // Four cubic arcs of a unit ellipse.
    path.anchors = QPolygonF{center + x, center + y, center - x, center - y};
    path.handles = QPolygonF{k*y, -k*x, -k*y, k*x};
    return path;
}

using Cubic = std::array<QPointF, 4>;

static QPointF mixPoint(QPointF a, QPointF b, double t)
{
    return a + t * (b - a);
}

static QPointF cubicPoint(const Cubic &curve, double t)
{
    const double s = 1 - t;
    return s*s*s*curve[0] + 3*s*s*t*curve[1] + 3*s*t*t*curve[2] + t*t*t*curve[3];
}

void PreviewWidget::insertRegionNode(QPointF position)
{
    const auto existing = existingControls();
    if (existing.anchors.isEmpty()) return;
    auto path = asPen(existing);
    const int count = int(path.anchors.size());
    if (count >= MaxRegionVertices) return;
    const auto g = geometry();
    double best = 8 * 8, parameter = 0;
    int edge = -1;
    Cubic segment{};
    bool curved = false;
    for (int i = 0; i < count; ++i) {
        const int next = (i + 1) % count;
        const auto a = path.anchors[i], b = path.anchors[next];
        const auto outgoing = path.handles.value(i), incoming = path.incomingAt(next);
        const bool curve = !outgoing.isNull() || !incoming.isNull();
        const Cubic raw{a, a + outgoing, b + incoming, b};
        const Cubic screen{g.widgetAt(raw[0]), g.widgetAt(raw[1]), g.widgetAt(raw[2]), g.widgetAt(raw[3])};
        const int steps = curve ? 48 : 1;
        QPointF from = screen[0];
        for (int step = 1; step <= steps; ++step) {
            const auto to = curve ? cubicPoint(screen, double(step) / steps) : screen[3];
            const auto direction = to - from;
            const double length = QPointF::dotProduct(direction, direction);
            if (length > 0) {
                const double fraction = std::clamp(QPointF::dotProduct(position - from, direction) / length, 0., 1.);
                const auto delta = mixPoint(from, to, fraction) - position;
                const double distance = QPointF::dotProduct(delta, delta);
                if (distance < best) {
                    best = distance;
                    parameter = (step - 1 + fraction) / steps;
                    edge = i;
                    segment = raw;
                    curved = curve;
                }
            }
            from = to;
        }
    }
    if (edge < 0 || parameter <= .0001 || parameter >= .9999) return;
    path.fillHandles();
    const int next = (edge + 1) % count;
    QPointF point = mixPoint(segment[0], segment[3], parameter);
    QPointF incoming, outgoing;
    if (curved) {
        // De Casteljau subdivision keeps the two new cubics on the original curve.
        const auto a = mixPoint(segment[0], segment[1], parameter);
        const auto b = mixPoint(segment[1], segment[2], parameter);
        const auto c = mixPoint(segment[2], segment[3], parameter);
        const auto d = mixPoint(a, b, parameter);
        const auto e = mixPoint(b, c, parameter);
        point = mixPoint(d, e, parameter);
        path.handles[edge] = a - segment[0];
        path.incoming[next] = c - segment[3];
        incoming = d - point;
        outgoing = e - point;
    }
    path.anchors.insert(edge + 1, point);
    path.incoming.insert(edge + 1, incoming);
    path.handles.insert(edge + 1, outgoing);
    const auto region = buildRegion(path);
    if (!region.valid()) return;
    selectedEditNode_ = edge + 1;
    commitRegion(region);
}

void PreviewWidget::deleteRegionNode()
{
    auto path = existingControls();
    const int index = selectedEditNode_;
    if (path.tool != RegionTool::Pen || index < 0 || index >= path.anchors.size() || path.anchors.size() <= 3) return;
    path.fillHandles();
    path.anchors.removeAt(index);
    path.incoming.removeAt(index);
    path.handles.removeAt(index);
    const auto region = buildRegion(path);
    if (!region.valid()) return;
    selectedEditNode_ = -1;
    commitRegion(region);
}

void PreviewWidget::finishRegion()
{
    const auto region = draftRegion();
    dragging_ = false;
    pen_.anchors.clear(); pen_.handles.clear(); pen_.incoming.clear(); placingPen_ = false;
    if (region.valid()) commitRegion(region);
    else update();
}

void PreviewWidget::commitRegion(const Region &region)
{
    regions_[edit_ - 1] = region;
    if (regionChanged) regionChanged(edit_, region);
    update();
}

void PreviewWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(28, 30, 34));
    const auto g = geometry();
    const QRectF rawArea = g.visibleSource();
    QPainterPath clip; clip.addPolygon(g.transform().map(QPolygonF(rawArea)));
    p.save(); p.setClipPath(clip);
    const int tile = 16;
    if (view_ == 4 || view_ == 5) p.fillRect(rect(), view_ == 4 ? Qt::black : Qt::white);
    else for (int y = 0; y < height(); y += tile)
        for (int x = 0; x < width(); x += tile)
            p.fillRect(x, y, tile, tile, ((x / tile + y / tile) % 2) ? QColor(70, 72, 76) : QColor(50, 52, 56));
    // Display choice is independent of the raw frame used for color sampling.
    QImage img = view_ == 0 ? original_ : processed_;
    if (view_ == 2 && !img.isNull()) {
        QImage alpha(img.size(), QImage::Format_RGB32);
        for (int y = 0; y < img.height(); ++y) {
            auto *dst = reinterpret_cast<QRgb *>(alpha.scanLine(y));
            const auto *src = img.constScanLine(y);
            for (int x = 0; x < img.width(); ++x) {
                const int a = src[x * 4 + 3]; dst[x] = qRgb(a, a, a);
            }
        }
        img = alpha;
    }
    p.save(); p.setWorldTransform(g.transform());
    if (view_ == 3 && !background_.isNull()) p.drawImage(rawArea, background_);
    if (!img.isNull() && !rawArea.isEmpty()) p.drawImage(rawArea, img, rawArea);
    p.restore();
    if (img.isNull()) { p.setPen(Qt::white); p.drawText(rect(), Qt::AlignCenter, QStringLiteral("카메라 소스를 선택하세요")); }
    auto scopePolygon = [&g](const Region &region) {
        const auto r = region.bounds;
        QPolygonF box = region.outline;
        if (box.isEmpty()) box << QPointF(r.x, r.y) << QPointF(r.x + r.width, r.y)
                              << QPointF(r.x + r.width, r.y + r.height) << QPointF(r.x, r.y + r.height);
        for (auto &point : box) point = g.widgetAt(point);
        return box;
    };
    const QColor colors[] {QColor(70, 180, 255), QColor(75, 220, 130), QColor(255, 110, 100), QColor(255, 205, 70)};
    const QString names[] {QStringLiteral("분석"), QStringLiteral("강제 보호"), QStringLiteral("강제 지움"), QStringLiteral("새 획 범위")};
    for (int i = 0; i < 4; ++i) {
        const auto r = dragging_ && edit_ == i + 1 ? draftRegion() : regions_[i];
        if (!r.valid()) continue;
        const auto box = scopePolygon(r);
        p.setPen(QPen(colors[i], edit_ == i + 1 ? 3 : 1, Qt::DashLine));
        p.drawPolygon(box);
        p.drawText(box.boundingRect().adjusted(5, 4, -4, -4), Qt::AlignTop | Qt::AlignLeft, names[i]);
    }
    if (selectedStroke_ >= 0 && selectedStroke_ < strokes_.size()) {
        const auto selected = strokes_[selectedStroke_].toObject(); const auto scope = selected["rect"].toArray();
        if (selected["local"].toBool() && scope.size() == 4) {
            const auto box = scopePolygon(readRegion(selected["region_points"],
                {scope[0].toDouble(), scope[1].toDouble(), scope[2].toDouble(), scope[3].toDouble()}));
            p.setPen(QPen(QColor(210, 140, 255), 2, Qt::DashDotLine)); p.drawPolygon(box);
            p.drawText(box.boundingRect().adjusted(6, 20, -4, -4), Qt::AlignTop | Qt::AlignLeft, QStringLiteral("선택 획 %1 · 실제 보정 범위").arg(selectedStroke_ + 1));
        }
    }
    auto drawStroke = [&](const QJsonArray &points, bool subtract, bool enabled) {
        if (points.isEmpty() || !enabled) return;
        QPainterPath path;
        bool first = true;
        for (const auto &value : points) {
            const auto xy = value.toArray(); if (xy.size() != 2) continue;
            const auto xyPixel = g.widgetAt({xy[0].toDouble(), xy[1].toDouble()});
            if (first) { path.moveTo(xyPixel); first = false; } else path.lineTo(xyPixel);
        }
        p.setPen(QPen(subtract ? QColor(255, 110, 100) : QColor(70, 190, 255), 2)); p.drawPath(path);
        if (points.size() == 1) p.drawEllipse(path.currentPosition(), 3, 3);
    };
    for (const auto &value : strokes_) { const auto s = value.toObject(); drawStroke(s["points"].toArray(), s["subtract"].toBool(), s["enabled"].toBool(true)); }
    if (dragging_ && edit_ >= 5) {
        QJsonArray points; for (const auto &point : strokePoints_) points.append(QJsonArray {point.x(), point.y()});
        drawStroke(points, edit_ == 6, true);
    }
    p.restore();
    const bool drawingPen = !pen_.anchors.isEmpty();
    const auto controls = drawingPen ? pen_ : editExisting_ ? (dragging_ ? editDraft_ : existingControls()) : RegionControls{};
    if (controls.anchors.isEmpty()) return;
    p.setRenderHint(QPainter::Antialiasing);
    const QColor marker(255, 216, 85);
    const auto points = controlCorners(controls);
    if (drawingPen) {
        QPainterPath guide(g.widgetAt(points.first()));
        for (qsizetype i = 1; i < points.size(); ++i) {
            guide.cubicTo(g.widgetAt(points[i-1] + controls.handles.value(i-1)),
                          g.widgetAt(points[i] + controls.incomingAt(i)), g.widgetAt(points[i]));
        }
        const auto first = g.widgetAt(points.first());
        const bool close = points.size() >= 3 && QLineF(first, penCursor_).length() <= 12;
        guide.lineTo(close ? first : penCursor_);
        p.setPen(QPen(marker, 2));
        p.drawPath(guide);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(first, close ? 12 : 9, close ? 12 : 9);
    }
    for (qsizetype i = 0; i < points.size(); ++i) {
        const auto point = g.widgetAt(points[i]);
        const auto outgoing = controls.handles.value(i), incoming = controls.incomingAt(i);
        if (!incoming.isNull() || !outgoing.isNull()) {
            p.setPen(QPen(QColor(85, 215, 255), 1.5));
            p.setBrush(QColor(24, 36, 44));
            for (const auto &offset : {incoming, outgoing}) {
                if (offset.isNull()) continue;
                const auto handle = g.widgetAt(points[i] + offset);
                p.drawLine(point, handle);
                p.drawEllipse(handle, 4, 4);
            }
        }
        p.setPen(QPen(QColor(25, 30, 36), 1.5));
        p.setBrush(!drawingPen && i == selectedEditNode_ ? QColor(255, 125, 70) : marker);
        p.drawRect(QRectF(point - QPointF(4, 4), QSizeF(8, 8)));
    }
}
void PreviewWidget::appendPoint(QPointF point)
{
    if (strokePoints_.size() >= 256) {
        QVector<QPointF> reduced; reduced.reserve(128);
        for (int i = 0; i < strokePoints_.size(); i += 2) reduced.append(strokePoints_[i]);
        strokePoints_ = std::move(reduced);
    }
    const auto n = normalized(point);
    if (!strokePoints_.isEmpty()) {
        const auto d = n - strokePoints_.last();
        if (std::hypot(d.x() * original_.width(), d.y() * original_.height()) < 2) return;
    }
    strokePoints_.append(n);
}
void PreviewWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::MiddleButton && !dragging_ && !placingPen_) { panning_ = true; panStart_ = event->position(); setCursor(Qt::ClosedHandCursor); return; }
    if (!panning_ && edit_ && !original_.isNull() && event->button() == Qt::LeftButton &&
        (edit_ <= 4 || geometry().contains(event->position()))) {
        if (editExisting_ && edit_ >= 2 && edit_ <= 4) {
            setFocus(Qt::MouseFocusReason);
            beginRegionEdit(event->position());
            independentHandle_ = event->modifiers().testFlag(Qt::AltModifier);
            if (dragging_ && editNode_ >= 0 && editPart_ == 0 &&
                editStart_.tool == RegionTool::Pen && independentHandle_) editPart_ = 1;
            return;
        }
        if (edit_ >= 2 && edit_ <= 4 && regionTool_ == RegionTool::Pen) {
            penCursor_ = event->position();
            const auto g = geometry();
            if (pen_.anchors.size() >= 3 && QLineF(g.widgetAt(pen_.anchors.first()), penCursor_).length() <= 12) {
                finishRegion();
                return;
            }
            const auto point = g.normalizedAt(penCursor_);
            if (pen_.anchors.size() < 100 && (pen_.anchors.isEmpty() || pen_.anchors.last() != point)) {
                pen_.anchors << point;
                pen_.handles << QPointF{};
                pen_.incoming << QPointF{};
                independentHandle_ = event->modifiers().testFlag(Qt::AltModifier);
                placingPen_ = true;
                start_ = penCursor_;
            }
            update();
            return;
        }
        dragging_ = true; start_ = end_ = event->position();
        if (edit_ >= 5) { strokeFrame_ = original_; strokePoints_.clear(); appendPoint(event->position()); }
        update();
    }
}
void PreviewWidget::updatePenHandle(QPointF position, Qt::KeyboardModifiers modifiers)
{
    independentHandle_ = independentHandle_ || modifiers.testFlag(Qt::AltModifier);
    const auto delta = geometry().normalizedAt(position) - pen_.anchors.last();
    pen_.handles.last() = QLineF(start_, position).length() >= 3 ? delta : QPointF{};
    pen_.incoming.last() = independentHandle_ ? QPointF{} : -pen_.handles.last();
}

void PreviewWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (panning_) { pan_ += event->position() - panStart_; panStart_ = event->position(); update(); return; }
    if (!pen_.anchors.isEmpty()) {
        penCursor_ = event->position();
        if (placingPen_) updatePenHandle(penCursor_, event->modifiers());
        update();
    }
    if (dragging_ && editExisting_ && edit_ >= 2 && edit_ <= 4) {
        moveRegionEdit(event->position(), event->modifiers());
        return;
    }
    if (dragging_) { end_ = event->position(); if (edit_ >= 5) appendPoint(event->position()); update(); }
}
void PreviewWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::MiddleButton) { panning_ = false; setCursor(edit_ ? Qt::CrossCursor : Qt::ArrowCursor); return; }
    if (placingPen_ && event->button() == Qt::LeftButton) {
        updatePenHandle(event->position(), event->modifiers());
        placingPen_ = false;
        update();
        return;
    }
    if (editClickPending_ && event->button() == Qt::LeftButton) {
        editClickPending_ = false;
        if (editNode_ < 0 && QLineF(start_, event->position()).length() < 3) {
            dragging_ = false;
            insertRegionNode(event->position());
            update();
            return;
        }
    }
    if (!dragging_ || event->button() != Qt::LeftButton) return;
    if (edit_ >= 5) {
        appendPoint(event->position()); dragging_ = false;
        // Preserve the release endpoint even when it is below the move-event spacing.
        const auto endpoint = normalized(event->position());
        if (strokePoints_.isEmpty() || strokePoints_.last() != endpoint) strokePoints_.append(endpoint);
        QJsonArray samples, points;
        QVector<double> distances; distances.reserve(strokePoints_.size()); distances.append(0);
        for (int i = 1; i < strokePoints_.size(); ++i) {
            const auto delta = strokePoints_[i] - strokePoints_[i - 1];
            distances.append(distances.last() + std::hypot(delta.x() * strokeFrame_.width(), delta.y() * strokeFrame_.height()));
        }
        const double length = distances.last();
        const int count = length <= .000001 ? 1 : std::min(64, int(std::ceil(length / 2.)) + 1);
        int segment = 1;
        const auto visible = geometry().visibleSource();
        for (int i = 0; i < count; ++i) {
            QPointF n = strokePoints_.first();
            if (count > 1) {
                const double target = length * i / (count - 1);
                while (segment < distances.size() - 1 && distances[segment] < target) ++segment;
                const double span = distances[segment] - distances[segment - 1];
                const double fraction = span > .000001 ? std::clamp((target - distances[segment - 1]) / span, 0., 1.) : 0.;
                n = strokePoints_[segment - 1] + (strokePoints_[segment] - strokePoints_[segment - 1]) * fraction;
            }
            const int x = std::clamp(int(n.x() * strokeFrame_.width()), int(visible.left()), int(visible.right()) - 1);
            const int y = std::clamp(int(n.y() * strokeFrame_.height()), int(visible.top()), int(visible.bottom()) - 1);
            const auto color = strokeFrame_.pixelColor(x, y);
            samples.append(QJsonArray {color.red(), color.green(), color.blue()}); points.append(QJsonArray {n.x(), n.y()});
        }
        const QJsonObject stroke {{"subtract", edit_ == 6}, {"enabled", true}, {"local", false}, {"samples", samples}, {"points", points}};
        strokePoints_.clear(); strokeFrame_ = {}; update();
        if (!samples.isEmpty() && strokeCompleted) strokeCompleted(stroke);
        return;
    }
    end_ = event->position();
    if (editExisting_) {
        if (QLineF(start_, end_).length() < 1) { dragging_ = false; update(); return; }
        moveRegionEdit(end_, event->modifiers());
    }
    finishRegion();
}
void PreviewWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && !pen_.anchors.isEmpty()) finishRegion();
    else QWidget::mouseDoubleClickEvent(event);
}
void PreviewWidget::keyPressEvent(QKeyEvent *event)
{
    if (!pen_.anchors.isEmpty() && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) finishRegion();
    else if ((!pen_.anchors.isEmpty() || dragging_) && event->key() == Qt::Key_Escape) cancelInteraction();
    else if (editExisting_ && !dragging_ && !editClickPending_ && event->key() == Qt::Key_Delete) deleteRegionNode();
    else QWidget::keyPressEvent(event);
}
void PreviewWidget::wheelEvent(QWheelEvent *event)
{
    if (dragging_ || panning_ || placingPen_ || original_.isNull()) { event->accept(); return; }
    const double steps = !event->angleDelta().isNull() ? event->angleDelta().y() / 120. : event->pixelDelta().y() / 120.;
    auto g = geometry(); g.zoomAt(event->position(), zoomPercent() * std::pow(1.2, steps));
    zoom_ = g.percent; pan_ = g.pan; update();
    if (zoomChanged) zoomChanged(zoom_);
    event->accept();
}
}
