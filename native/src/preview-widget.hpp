#pragma once
#include "ac-types.hpp"
#include "preview-geometry.hpp"
#include "region.hpp"
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QVector>
#include <QWidget>
#include <functional>

namespace ac {
class PreviewWidget final : public QWidget {
public:
    explicit PreviewWidget(QWidget *parent = nullptr);
    void setFrames(const Preview &preview);
    void setView(int index);
    void setBackground(const QImage &background);
    void setRegions(Rect analysis, const Region &protect, const Region &erase, const Region &local);
    void setEditRegion(int index);
    void setRegionTool(RegionTool tool);
    void setExistingRegionEdit(bool enabled);
    void setStrokes(const QJsonArray &strokes);
    void setSelectedStroke(int index);
    void setZoom(double percent);
    void setSceneTransform(const SceneTransform &scene);
    double zoomPercent() const;
    void cancelInteraction();
    std::function<void(int, Region)> regionChanged;
    std::function<void(QJsonObject)> strokeCompleted;
    std::function<void(double)> zoomChanged;
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void wheelEvent(QWheelEvent *) override;
private:
    PreviewGeometry geometry() const;
    QPointF normalized(QPointF point) const;
    void appendPoint(QPointF point);
    Region draftRegion() const;
    Region buildRegion(const RegionControls &controls) const;
    RegionControls existingControls() const;
    void beginRegionEdit(QPointF position);
    void moveRegionEdit(QPointF position, Qt::KeyboardModifiers modifiers);
    void updatePenHandle(QPointF position, Qt::KeyboardModifiers modifiers);
    void insertRegionNode(QPointF position);
    void deleteRegionNode();
    void finishRegion();
    void commitRegion(const Region &region);
    QImage original_, processed_, background_, strokeFrame_;
    QJsonArray strokes_;
    QVector<QPointF> strokePoints_;
    Region regions_[4] {{{0, 0, 1, 1}, {}}, {}, {}, {}};
    RegionTool regionTool_ = RegionTool::Rectangle;
    RegionControls pen_ {RegionTool::Pen, {}, {}, {}};
    RegionControls editStart_, editDraft_;
    bool editExisting_ = false, placingPen_ = false, independentHandle_ = false;
    bool editClickPending_ = false;
    int editNode_ = -1, editPart_ = 0, selectedEditNode_ = -1;
    QPointF penCursor_;
    SceneTransform scene_;
    int view_ = 1, edit_ = 0;
    double zoom_ = 0;
    int selectedStroke_ = -1;
    bool dragging_ = false, panning_ = false;
    QPointF start_, end_, pan_, panStart_;
};
}
