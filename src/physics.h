#pragma once

// Rigid bodies, as Blender's: pieces that fall, land, bounce and knock into
// each other, worked out frame by frame and baked into keyframes. A world's
// pieces fall in meters onto its ground; a flat scene's layers in pixels
// onto the bottom of the canvas. Bodies are boxes that turn: they meet the
// ground with their corners, so a knock off their middle or a landing on
// an edge spins them, and they tip over onto a face as they come to rest;
// against each other they meet as turned boxes too.

#include <QList>
#include <QString>
#include <QQuaternion>
#include <QVector3D>
#include <functional>

#include "scene.h"

namespace Physics {

struct Body {
    QVector3D centre;    // Up is +y.
    QVector3D half;      // Half its size along each axis.
    QVector3D velocity;
    QQuaternion turn;    // How it has turned since the start.
    QVector3D spin;      // Degrees a second about x, y and z.
    double mass = 1;
    double bounce = .3;    // 0 lands dead, 1 bounces back as high.
    double friction = .5;  // How much sliding slows it.
    bool fixed = false;    // Collided with but never moved.
    bool tumble = true;    // False keeps it upright: it only slides.
};

// Where a body is at one frame, and how it has turned since the start.
struct Pose {
    QVector3D centre;
    QQuaternion turn;
};

// Steps bodies through time: gravity (down is -y), the ground's height at
// (x, z) (NaN where there's none), collisions between them. Returns their
// poses at each of frames frames, dt apart.
QList<QList<Pose>> simulate(QList<Body> bodies, double gravity, const std::function<double(double, double)> &ground,
                                 double dt, int frames);

// A layer as a body (its "body" in scene.json: mass, bounce, friction,
// fixed, tumble), or not (an empty object takes it out).
bool setBody(Scene &scene, int layer, const QJsonObject &body, QString *error);

// Simulates every body in the scene from one time to another, starting each
// moving as its keys have it then, and bakes where they go as keys of
// their position (worldX/Y/Z in a world, x and y on a flat canvas) and, as
// they tumble, their turn (pitch, roll and yaw, or rotation).
// Returns how many layers moved.
int bake(Scene &scene, AssetCache &assets, double from, double to, QString *error);

}
