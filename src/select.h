#pragma once

// Smart select: from one click, the connected area of colours close to the
// clicked one, as a solid region, and its outline as a cut shape.

#include <QImage>
#include <QPoint>
#include <QPolygonF>

namespace Select {

// The pixels reachable from seed through colours within tolerance (0–255) of
// the seed's, ignoring near-transparent ones, with any holes inside filled so
// the region is solid. An Alpha8 mask the size of image, 255 where selected.
QImage region(const QImage &image, const QPoint &seed, int tolerance);

// For each pixel, the lowest tolerance at which it joins the selection grown
// from seed: the worst colour step on the best path to it. Alpha8, 255 for
// never. One pass gives the selection at every tolerance at once.
QImage joinLevels(const QImage &image, const QPoint &seed);

// The selection at a tolerance, from join levels, made solid as region() is.
QImage regionAt(const QImage &levels, int tolerance);

// A tolerance that takes the clicked thing and stops before the selection
// bursts out into its surroundings, judged by how the area grows.
int autoTolerance(const QImage &levels);

// Grows a selection by radius pixels, to take the blended pixels along an
// edge that are part of the thing but no longer its colour.
void grow(QImage &mask, int radius);

// Adds b's selection into a.
void unite(QImage &a, const QImage &b);

// How many separate regions a mask holds.
int regionCount(const QImage &mask);

// The outline of the largest region in a mask, in mask pixels, simplified to
// within tolerance pixels of the traced edge.
QPolygonF outline(const QImage &mask, double tolerance = 1.0);

}
