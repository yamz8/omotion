#pragma once

// Cloth, as Blender's: a layer's picture as fabric, held where it's pinned
// (along an edge, or at corners), blown by the wind in gusts, and swinging
// under its own weight. It moves out of the picture's plane as well as in
// it, so it folds: the folds are shaded, and fold it narrower as seen.
// Worked out from the scene's start to any time, the same every time, and
// kept for the frames after.
//
// A layer's "cloth" in scene.json: {"pin": "left", "wind": 300, "gust": .5,
// "stiffness": .8, "weight": .3, "seed": 1}. Distances in its picture's
// pixels: wind is pixels a second, toward its direction (degrees, 0 to the
// right).

#include <QJsonObject>
#include <QList>
#include <QPointF>
#include <QSizeF>
#include <QVector3D>

#include "scene.h"

namespace Cloth {

// The fabric at a time: a grid of points over its picture, each where the
// point of the picture under it has gone (x and y in its pixels, z toward
// the viewer).
struct State {
    int columns = 0, rows = 0;
    QSizeF size;
    QList<QVector3D> points;
    bool isNull() const { return points.isEmpty(); }
};

State at(const Scene &scene, const Layer &layer, const QSizeF &size, double t);

// Where a point of its picture is now, and how much the fold there turns
// away from the viewer (0 facing, toward 1 edge on), for shading.
QPointF bend(const State &state, const QPointF &point);
double fold(const State &state, const QPointF &point);

// Sets a layer's cloth (an empty object takes it away); checks its settings.
bool set(Scene &scene, int layer, const QJsonObject &cloth, QString *error);

}
