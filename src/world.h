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
    // The picture carried on past its edges, so a camera can turn a little:
    // an image of the picture with margin pixels added each side (painted in
    // by the AI), and those margins.
    QString wide;
    QSize margin;
    // The ground, from the camera: which way is up (y down, z forward, as a
    // picture's pixels go) and how high the camera stood above it.
    QVector3D up{0, -1, 0};
    double height = 1.6;
    // Everything's distance times this: what the AI reads from a picture,
    // a painting above all, is only roughly to scale.
    double scale = 1;
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
    int columns = 0;  // For a grid, its vertices across; 0 for a shape.
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

// The picture a world layer wears: its own image, with the colour at its
// edges spread out under the see-through part, so the GPU, shrinking it,
// doesn't darken its edges with the black of empty pixels. Made once, next
// to the depth images; a path in the project.
QString texture(const Scene &scene, AssetCache &assets, int index);

// Gives a piece of the world a 3D shape (the AI makes one from its cut-out,
// about 20 seconds): kept in assets/world and named in the layer's "shape",
// it stands in for the flat cut-out, fitted to it from the picture's camera
// and wearing the picture, so it looks the same from there and has a back.
bool shapeObject(Scene &scene, AssetCache &assets, int index, QString *error);

// The ground's height (world y) at a world point (x, z), from what the
// picture shows of it: its surfaces binned by where they stand, the gaps
// (behind a crest, say) filled between what's seen around them and kept
// under the camera's line of sight. NaN outside what the world knows.
double groundAt(const Scene &scene, AssetCache &assets, double x, double z);

// Which way a piece faces in the world, as a yaw (degrees, 0 looking away
// from the camera, right positive): its shape's long axis, the end toward
// the camera's side, or straight at the camera for a flat piece.
double heading(const Scene &scene, AssetCache &assets, int index);

// Drives a piece over the ground from one world point to another at a
// speed (meters a second), from time t: it keeps to the ground, flies off
// where the ground falls away faster than gravity, lands where the ground
// is, and pitches with its path. It keeps facing as the picture shows it
// (a picture has only that side of it) unless face turns it to the way. Baked as keyframes
// of its world properties; the landings, in seconds, come back.
bool drive(Scene &scene, AssetCache &assets, int index, const QPointF &from, const QPointF &to, double speed,
           double t, QList<double> *landings, QString *error, bool face = false);

// Sizes the world so a piece of it is as tall as it really is (meters).
bool setSize(Scene &scene, AssetCache &assets, int index, double meters, QString *error);

// The picture under a layer turned into a world (the AI reads it, a few
// seconds): writes the depth images and sets the scene's "world".
// horizon, when known (0 to 1, down the picture), sets which way is level:
// better than the ground in sight, which may slope, as a road up to a crest.
bool build(Scene &scene, AssetCache &assets, int index, QString *error, double horizon = -1);

// The picture carried on past each edge by margin pixels, painted in by the
// AI a square at a time around the border, each square seeing what's
// already there; done at a working size, so quick and soft.
QImage outpaint(const QImage &picture, const QSize &margin, QString *error);

// Fills the cells of a depth grid where hole is set from the cells around,
// smoothly, in log distance; holes as wide as the grid still fill.
void fillDepth(QImage &depth, const QImage &hole);

}
