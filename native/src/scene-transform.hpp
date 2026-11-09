#pragma once
#include <QMargins>
#include <QSizeF>
#include <QTransform>

namespace ac {
struct SceneTransform {
    QTransform linear;
    QMargins crop;
    QSizeF canvas;
};
}
