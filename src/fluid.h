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

#include "scene.h"

namespace Fluid {

bool isFluid(const QString &kind);

// The smoke (or fire) at a time, as a picture of the grid with see-through
// where there's none, and the canvas rectangle it covers.
QImage at(const Scene &scene, AssetCache &assets, const Layer &layer, double t, QRectF *onCanvas);

}
