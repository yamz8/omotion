#pragma once

// A scene's world: the picture it was made from, turned into 3D. The AI
// reads where every pixel is in meters and the lens it was seen through; the
// picture and each piece cut from it become meshes at those distances, the
// background filled in behind the pieces, so a camera can move among them.
// The picture's camera stands on the ground the AI found: the world has x to
// the right, y up and z toward the viewer (as Qt Quick 3D has it), with the
// ground at y = 0 under the camera. From that camera, with nothing moved,
// the world looks exactly like the picture.

#include <QByteArray>
#include <QImage>
#include <QList>
#include <QMatrix4x4>
#include <QPointF>
#include <QQuaternion>
#include <QSize>
#include <QString>
#include <QVector3D>

#include "scene.h"

namespace World {

// Distances are kept as 16-bit images: logarithmic, so 5 cm and 2 km both
// keep their detail. The top code is the sky, as far as the world goes.
constexpr double Near = .05;
constexpr double Far = 2000;
constexpr quint16 Sky = 65535;
quint16 encodeDepth(double meters);
double decodeDepth(quint16 code);

// What scene.json's "world" says.
struct Info {
    bool valid = false;
    QString picture;   // The layer it was made from.
    QString depth;     // The picture's depth.
    QString behind;    // The same with the pieces taken out and filled in.
    double focal = 0;  // The lens, in the picture's pixels.
    // The ground, from the camera: which way is up (y down, z forward, as a
    // picture's pixels go) and how high the camera stood above it.
    QVector3D up{0, -1, 0};
    double height = 1.6;
};
Info info(const Scene &scene);

// The world's layers, in drawing order: the picture and everything cut from
// it (and from its pieces), but not particles.
QList<int> layers(const Scene &scene);
// The layer the world was made from, for a layer in it (or itself).
int rootOf(const Scene &scene, int index);
// Where a piece's image sits on the picture, in the picture's pixels.
QPointF offsetInPicture(const Scene &scene, int index);

// From the picture's camera (x right, y down, z forward, in meters) to the
// world.
QMatrix4x4 pictureToWorld(const Info &info);

// A layer as a mesh in the world, at rest: a grid over its image, each
// point pushed out to its distance. Vertices are x, y, z, u, v floats;
// indices are 32-bit triangles.
struct Mesh {
    QByteArray vertices;
    QByteArray indices;
    QVector3D minimum;
    QVector3D maximum;
    QVector3D pivot;  // Its anchor, in the world, where it turns.
    bool isNull() const { return vertices.isEmpty(); }
};
Mesh mesh(const Scene &scene, AssetCache &assets, int index);
// Just the pivot, without building the mesh.
QVector3D pivot(const Scene &scene, AssetCache &assets, int index);

// Where everything is at a moment: the camera, and each of layers() moved by
// its world properties about its pivot.
struct Placement {
    int layer = -1;
    QVector3D position;
    QQuaternion rotation;
    float scale = 1;
    float opacity = 1;
    bool visible = true;
};
struct Frame {
    QVector3D cameraPosition;
    QQuaternion cameraRotation;
    double fov = 40;  // Vertical, in degrees.
    QList<Placement> layers;
};
Frame frame(const Scene &scene, AssetCache &assets, double t);

// The picture under a layer turned into a world (the AI reads it, a few
// seconds): writes the depth images and sets the scene's "world".
bool build(Scene &scene, AssetCache &assets, int index, QString *error);

// Fills the cells of a depth grid where hole is set from the cells around,
// smoothly, in log distance; holes as wide as the grid still fill.
void fillDepth(QImage &depth, const QImage &hole);

}
