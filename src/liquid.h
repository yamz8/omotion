#pragma once

// Water, as Blender's liquid simulation pours it, in the picture's plane:
// drops from a source (a point of the canvas, or of a layer as it moves)
// that fall, splash, pool on the floor and against the canvas's sides, and
// hold together as water does (Clavet's particle fluid: each drop pushed
// apart from crowding neighbours and drawn to sparse ones). Drawn as one
// body of water, where the drops run together. Worked out from the scene's
// start, the same every time, and kept for the frames after.
//
// A particle layer of kind "water": "x", "y" (canvas pixels) or "from" and
// "at", "rate" (drops a second), "size" (a drop's radius, pixels), "speed"
// and "direction" (how it pours out, pixels a second, degrees with 90
// down), "spread" (degrees), "gravity" (pixels a second squared), "floor"
// (canvas y; the canvas's bottom when left out), "viscosity" (0 to 1),
// "color", "seed", and "loop" (true unless false: it loops with the scene,
// crossfading into itself).

#include <QImage>
#include <QRectF>

#include "scene.h"

namespace Liquid {

// The water at a time, drawn at the canvas's size scaled by scale, with
// see-through where there's none; and the canvas rectangle it covers.
QImage at(const Scene &scene, AssetCache &assets, const Layer &layer, double t, double scale, QRectF *onCanvas);

// Where its drops are at a time, in canvas pixels.
QList<QPointF> drops(const Scene &scene, AssetCache &assets, const Layer &layer, double t);

}
