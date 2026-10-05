#pragma once

// A project is a folder: scene.json beside the images it uses. These are the
// operations that touch that folder, kept apart from the UI so the command
// line renderer and the tests share them.

#include <QByteArray>
#include <QPolygonF>
#include <QString>

#include "scene.h"

namespace Project {

QString sceneFile(const QString &directory);
bool isImage(const QString &path);

// Turns whatever was opened into a project folder and returns it. An image
// gets a new "<name>.omotion" folder beside it holding a copy, so the original
// is never touched; opening it again reopens that folder. A folder of loose
// images gets a scene.json with one layer per image.
QString open(const QString &path, QString *error);

Scene load(const QString &directory, QString *error, QByteArray *bytes = nullptr);
QByteArray serialize(const Scene &scene);
// Writes beside scene.json and renames over it, so a crash or a reader
// never sees half a file.
bool save(const QString &directory, const Scene &scene, QString *error, QByteArray *bytes = nullptr);

// Cuts the part of a layer inside a canvas-space polygon out into its own
// image and layer, placed exactly where it was and parented to the layer it
// came from. feather softens the edge, in canvas pixels. With punch, the
// source layer gets a hole where the piece was, worked out so the two still
// add up to the original; with fill, the hole is painted in from the colours
// around it, so moving the piece away doesn't leave a gap. The piece remembers its shape (in "cut", in the
// source's pixels), so it can be reshaped later. New images are always new
// files, so undo can go back to the old ones. The piece is named after its
// source unless given a name. Returns the new layer's index, or -1.
int cutOut(Scene &scene, AssetCache &assets, int layer, const QPolygonF &polygon, double t,
           bool punch, QString *error, const QString &name = {}, double feather = 0, bool fill = true);

// Gives a piece a new shape and edge, redrawing it and its source's holes
// from the source's original image. Its animation and pivot are kept. fill
// turns filling its hole on (1) or off (0), or leaves it as it was (-1).
bool reshapeCut(Scene &scene, AssetCache &assets, int piece, const QPolygonF &polygon, double t,
                double feather, QString *error, int fill = -1);
bool isCut(const Scene &scene, int layer);
// A piece's shape in canvas pixels at time t, and its edge softness.
QPolygonF cutOutline(const Scene &scene, AssetCache &assets, int piece, double t, double *feather = nullptr,
                     bool *fill = nullptr);
// Smart select on a layer: the area of colour within tolerance around each
// canvas point, joined, as a cut shape in canvas pixels. A tolerance below 0
// picks one for each point (see Select::autoTolerance), and *used says the
// last one. regions says how many separate areas that made; only the biggest
// is outlined. levels, when given, keeps each point's search to reuse while
// the points stay the same, so trying tolerances is instant.
QPolygonF smartOutline(const Scene &scene, AssetCache &assets, int layer, const QList<QPointF> &points, double t,
                       int tolerance, int *regions = nullptr, int *used = nullptr, QList<QImage> *levels = nullptr);

// AI select on a layer: the thing under the take points, less what's under
// the leave-out ones, as a cut shape in canvas pixels. choices says how many
// guesses the AI made; choice picks one (0 is its best).
QPolygonF aiOutline(const Scene &scene, AssetCache &assets, int layer, const QList<QPointF> &take,
                    const QList<QPointF> &leave, double t, int choice, int *choices, QString *error);

// Depth for a layer's image, from the AI.
QImage depthOf(const Scene &scene, AssetCache &assets, int layer, QString *error);

// Splits a layer into count layers by depth, from a depth map of its image
// (16-bit grey, larger nearer, any size). The source stays as the farthest
// layer; the nearer ones are cut out of it as pieces, nearest on top, each
// with the places of the ones in front of it filled in behind, so nothing
// opens up when they move apart. Returns the new layers' indexes.
QList<int> splitByDepth(Scene &scene, AssetCache &assets, int layer, const QImage &depth, int count, double feather,
                        QString *error);

// Animates a depth-split layer as a slow camera move: it drifts sideways and
// pushes in, and nearer layers slide further, strength canvas pixels for the
// nearest, as a loop over the scene's duration.
bool animateParallax(Scene &scene, AssetCache &assets, int layer, double strength, QString *error);

// Keeps pieces pointing at their source when it's renamed.
void renameCutSource(Scene &scene, const QString &from, const QString &to);

// Moves a layer's pivot to a canvas point without moving the layer.
void setPivot(Scene &scene, AssetCache &assets, int layer, const QPointF &point, double t);

// Copies an image into the project's assets and adds it as a layer on top.
int importImage(Scene &scene, const QString &directory, const QString &path, QString *error);

}
