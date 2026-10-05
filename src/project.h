#pragma once

// A project is a folder: scene.json beside the images it uses. These are the
// operations that touch that folder, kept apart from the UI so the command
// line renderer and the tests share them.

#include <QByteArray>
#include <QJsonObject>
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
// around it, so moving the piece away doesn't leave a gap; with aiFill, the
// AI paints it (LaMa), inventing texture, where it's set up. The piece remembers its shape (in "cut", in the
// source's pixels), so it can be reshaped later. New images are always new
// files, so undo can go back to the old ones. The piece is named after its
// source unless given a name. Returns the new layer's index, or -1.
int cutOut(Scene &scene, AssetCache &assets, int layer, const QPolygonF &polygon, double t,
           bool punch, QString *error, const QString &name = {}, double feather = 0, bool fill = true,
           bool aiFill = false);

// Gives a piece a new shape and edge, redrawing it and its source's holes
// from the source's original image. Its animation and pivot are kept. fill
// sets how its hole is filled: 0 not, 1 smoothly, 2 by the AI, -1 as it was.
bool reshapeCut(Scene &scene, AssetCache &assets, int piece, const QPolygonF &polygon, double t,
                double feather, QString *error, int fill = -1);
bool isCut(const Scene &scene, int layer);
// A piece's shape in canvas pixels at time t, and its edge softness.
QPolygonF cutOutline(const Scene &scene, AssetCache &assets, int piece, double t, double *feather = nullptr,
                     int *fill = nullptr);
// A piece's shape as a mask over the image it was cut from (Alpha8, the
// whole image's size, 255 inside), whether it was cut by an outline or a
// mask, with the transform from that image's pixels to the canvas at t.
QImage cutMask(const Scene &scene, AssetCache &assets, int piece, double t, QTransform *toCanvas);
// Gives a piece a new shape, as a mask like cutMask's, saved into the
// project; redraws it and its source's holes. Its animation stays.
bool reshapeCutMask(Scene &scene, AssetCache &assets, int piece, const QImage &mask, QString *error);

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

// Animates a depth-split layer as a slow camera move: each depth layer
// follows the camera by its depth (the background 30%, the nearest fully),
// and the camera drifts strength canvas pixels sideways and pushes in, as a
// loop over the scene's duration. A scene shorter than 10 seconds is made 10,
// stretching any animation it already has to match.
bool animateParallax(Scene &scene, AssetCache &assets, int layer, double strength, QString *error);

// Adds a particle layer of a kind (see Scene::particleKinds) on top, with
// its default settings and any given over them. Returns its index, or -1.
int addParticles(Scene &scene, const QString &kind, const QJsonObject &settings, QString *error);

// The motion presets, by name: sway, float, pulse, bounce, spin, fade.
const QStringList &presets();
// Gives a layer a ready-made motion, replacing that property's animation.
// All but fade fit whole cycles (spin, whole turns) into the scene, so they
// loop with it; fade starts at t. amount scales the motion (1 is the default).
bool applyPreset(Scene &scene, AssetCache &assets, int layer, const QString &preset, double t, double amount,
                 QString *error);

// Image files in the project's assets folder that none of the given scenes
// (say, the scene and its undo history) mentions anywhere.
QStringList unusedAssets(const QString &directory, const QList<QJsonObject> &scenes);
// Moves those files to the trash; returns how many bytes that freed, or -1.
qint64 cleanAssets(const QString &directory, const QList<QJsonObject> &scenes, int *count, QString *error);

// Keeps pieces pointing at their source when it's renamed.
void renameCutSource(Scene &scene, const QString &from, const QString &to);

// Moves a layer's pivot to a canvas point without moving the layer.
void setPivot(Scene &scene, AssetCache &assets, int layer, const QPointF &point, double t);

// Copies an image into the project's assets and adds it as a layer on top.
int importImage(Scene &scene, const QString &directory, const QString &path, QString *error);

}
