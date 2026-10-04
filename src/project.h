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
// came from. With punch, the source layer gets a copy of its image with a
// hole where the piece was. New images are always new files, so undo can go
// back to the old ones. Returns the new layer's index, or -1.
int cutOut(Scene &scene, AssetCache &assets, int layer, const QPolygonF &polygon, double t,
           bool punch, QString *error);

// Moves a layer's pivot to a canvas point without moving the layer.
void setPivot(Scene &scene, AssetCache &assets, int layer, const QPointF &point, double t);

// Copies an image into the project's assets and adds it as a layer on top.
int importImage(Scene &scene, const QString &directory, const QString &path, QString *error);

}
