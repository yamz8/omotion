#pragma once

// Reading imported model files, once for everyone who needs them: the glTF
// exporter copying a model in, the world measuring one and working out its
// shape for shadows. glTF (or GLB, its one-file form) and OBJ.

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QMatrix4x4>
#include <QString>
#include <QVector3D>
#include <functional>

namespace ModelFile {

// A glTF or GLB: its JSON, and its buffers' bytes in order (a GLB's own
// binary for one without a file; files beside it, or data in the file).
// Empty JSON, with why, when it can't be read.
struct Gltf {
    QJsonObject json;
    QList<QByteArray> buffers;
    QString error;
};
Gltf readGltf(const QString &path);

// A node's own move, turn and size (its matrix, or its translation,
// rotation and scale).
QMatrix4x4 nodeMatrix(const QJsonObject &node);

// Every node under the scene's roots, with where its parents put it.
void walk(const QJsonObject &gltf, const std::function<void(const QJsonObject &node, const QMatrix4x4 &world)> &visit);

// An accessor's numbers (width of them an element), kept inside its
// buffer whatever the file says; false when they can't be read.
bool read(const Gltf &file, int accessor, int width, QList<double> *out);

// An OBJ's points ("v" lines, however they're spaced), and its faces as
// triangles of points (corners naming no point leave theirs out).
QList<QVector3D> objPoints(const QByteArray &text);
QList<QVector3D> objTriangles(const QByteArray &text);

// A model's triangles in its own units, three corners each.
QList<QVector3D> triangles(const QString &path);

}
