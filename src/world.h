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
#include <QColor>
#include <QJsonArray>
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
    // What lies behind the near side of jumps in distance (the sky behind a
    // hill, the hills behind a sign), painted in by the AI: its distances
    // and its picture, see-through but for that band. Shown where a moving
    // camera opens a gap.
    QString farDepth;
    QString farPicture;
    // The ground, from the camera: which way is up (y down, z forward, as a
    // picture's pixels go) and how high the camera stood above it.
    QVector3D up{0, -1, 0};
    double height = 1.6;
    // The air: its colour (from the picture near the horizon) and how much
    // of a thing it hides a meter (past where the picture has it).
    QColor haze{200, 140, 160};
    double hazeDensity = .02;
    // Everything's distance times this: what the AI reads from a picture,
    // a painting above all, is only roughly to scale.
    double scale = 1;
};
Info info(const Scene &scene);

// The layer behind the background's near edges, where a world has one; it
// goes by this index in meshes and the view.
constexpr int FarLayer = -10;

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
    // x, y, z, u, v; a shape adds its own colour (r, g, b, a) and how much
    // of the picture it wears (1 where the picture saw it, 0 where not).
    QByteArray vertices;
    QByteArray indices;
    int stride = 5;  // Floats a vertex.
    QVector3D minimum;
    QVector3D maximum;
    QVector3D pivot;  // Its anchor, in the world, where it turns.
    int columns = 0;  // For a grid, its vertices across; 0 for a shape.
    // For a grid, a smooth normal a vertex (x, y, z floats), for lights.
    QByteArray normals;
    bool isNull() const { return vertices.isEmpty(); }
};
Mesh mesh(const Scene &scene, AssetCache &assets, int index);
// Just the pivot, without building the mesh.
QVector3D pivot(const Scene &scene, AssetCache &assets, int index);

// Where everything is at a moment: the camera, and each of layers() moved by
// its world properties about its pivot.
struct Placement {
    int layer = -1;
    // Farther than the picture shows it, a piece fades into the air: how
    // much (0 to 1). The picture already has the air at its own distance.
    float haze = 0;
    // The light around it, for its edges that face away (a shape's sides
    // the picture never showed).
    QColor wrap{0, 0, 0};
    QVector3D position;
    QQuaternion rotation;
    float scale = 1;
    float opacity = 1;
    bool visible = true;
    // Its material, under the lights: how much it shines, how rough.
    float shine = 0;
    float roughness = .5f;
};
// A soft shadow on the ground under a shaped piece: darkest when it
// touches, fainter and wider the higher it is.
struct Shadow {
    QVector3D position;  // On the ground, under it.
    float width = 1;     // Across and along the footprint, in meters.
    float length = 1;
    float yaw = 0;       // Degrees, turned as the piece is.
    float opacity = 0;
};
// Dust thrown up where a piece leaves the ground or lands on it.
struct Burst {
    QVector3D position;  // On the ground.
    QVector3D away;      // Which way it's kicked: back from the way it goes.
    double time = 0;     // Seconds.
    float strength = 1;  // Landings 1, takeoffs less.
};
// One puff of that dust at the frame's time.
struct Puff {
    QVector3D position;
    float size = 1;   // Across, in meters.
    float alpha = 0;
};
// A light in the world: a layer with "light" (its colour, power, and how
// far it reaches), where its world properties put it from the picture's
// camera, as bright as its opacity. It adds to the picture's own light.
struct Light {
    int layer = -1;
    QVector3D position;
    QVector3D colour;  // Times its power.
    float reach = 10;  // Meters, where it's down to a half.
};
QList<int> lightLayers(const Scene &scene);
QVector3D lightPosition(const Scene &scene, int index, double t);
struct Frame {
    double time = 0;
    QList<Light> lights;
    QColor haze{200, 140, 160};
    QList<Shadow> shadows;
    QList<Burst> bursts;
    QList<Puff> puffs;
    QColor dust{217, 160, 142};
    QVector3D cameraPosition;
    QQuaternion cameraRotation;
    double fov = 40;  // Vertical, in degrees.
    QList<Placement> layers;
};
Frame frame(const Scene &scene, AssetCache &assets, double t);
// A way of looking at the world: the shot's camera, or the window's free
// view. Looks down -z, as Qt Quick 3D's cameras do; fov is vertical.
struct View {
    QVector3D position;
    QQuaternion rotation;
    double fov = 40;
};
View cameraView(const Frame &frame);
// Where a world point lands on an image of this size seen through the view;
// false when it's behind the view.
bool project(const View &view, const QSizeF &size, const QVector3D &point, QPointF *onScreen);
// The ray from the view through a point of that image: where it starts and
// which way it goes (a unit vector).
void ray(const View &view, const QSizeF &size, const QPointF &onScreen, QVector3D *origin, QVector3D *direction);
// The world layer nearest the view under a point of that image, where its
// picture isn't see-through; -1 for none.
int pick(const Scene &scene, AssetCache &assets, double t, const View &view, const QSizeF &size, const QPointF &onScreen);
// The corners of a world layer's mesh box where it is at time t (8 points:
// bit 0 of the index picks x's end, bit 1 y's, bit 2 z's), and its pivot
// there; false for no mesh.
bool box(const Scene &scene, AssetCache &assets, int index, double t, QList<QVector3D> *corners, QVector3D *pivot);

// How far the world moves on screen between two moments, in pixels of an
// image this size: the most that any point of its pieces' meshes travels,
// as the camera and the pieces move. For how many moments a shutter needs.
double motion(const Scene &scene, AssetCache &assets, double from, double to, const QSize &size);

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

// What a world was made from, to tell when it's out of date: the shapes of
// the pieces cut out of its picture (their holes are filled in behind).
QJsonArray madeFrom(const Scene &scene, int root);
// A piece's shape as a short code: the same while its outline, mask and
// edge are (however its hole is filled), its image's path for a layer that
// wasn't cut.
QString cutCode(const Layer &layer);

// A shaped piece's mesh modifiers, as Blender's, kept in its "shape" and
// applied when its mesh is made: "smooth" (times) relaxes the AI's lumps,
// "simplify" (cells across) makes it lighter; "none" takes them away.
bool setShapeModifier(Scene &scene, int index, const QString &type, int amount, QString *error);

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
