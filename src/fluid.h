#pragma once

// Smoke and fire, as Blender's fluid simulation makes them, in the
// picture's plane: a grid of moving air over the canvas, with a source
// (a point of the canvas, or of a layer as it moves) that puts smoke in,
// and for fire heat, which rise, swirl, drift with the wind and fade.
// Worked out from the scene's start, the same every time, and kept for the
// frames after (worked out again when its settings or its source's moves
// change).
//
// A particle layer of kind "smoke" or "fire": "x", "y" (canvas pixels) or
// "from" and "at" (a layer, and a point of it), "size" (the source's
// radius, pixels), "speed" (how fast it rises, pixels a second), "rate"
// (how much it puts in), "life" (seconds a puff lasts), "wind" (pixels a
// second to the right), "swirl" (0 to 1), "color", "seed"; and "loop"
// (true unless false: it loops with the scene, crossfading into itself).

#include <QImage>
#include <QRectF>
#include <QVector3D>
#include <functional>

#include "scene.h"

namespace Fluid {

bool isFluid(const QString &kind);

// The smoke (or fire) at a time, as a picture of the grid with see-through
// where there's none, and the canvas rectangle it covers.
QImage at(const Scene &scene, AssetCache &assets, const Layer &layer, double t, QRectF *onCanvas);

// The same in three dimensions, for a world: air in a box of grid cells
// (centimeters, up is +y), its source at a point of the box at each frame
// (a function of the frame), smoke drawn as slices across one axis (0 for
// x, 2 for z), back to front from its low end. Each slice's picture spans
// the box's other two sides (the horizontal one, then up).
struct Box {
    QVector3D size;        // Centimeters.
    QList<QImage> slices;  // Along the axis, from its low end.
};
Box volume(const QString &key, const QJsonObject &settings, const QVector3D &size, double fps, double duration,
           const std::function<QVector3D(int frame)> &sourceAt, double t, int axis);

}
