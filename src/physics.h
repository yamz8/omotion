#pragma once

// Rigid bodies, as Blender's: pieces that fall, land, bounce and knock into
// each other, worked out frame by frame and baked into keyframes. A world's
// pieces fall in meters onto its ground; a flat scene's layers in pixels
// onto the bottom of the canvas. Bodies are boxes, square to the axes: a
// stand-in good enough for things that tumble a little and land.

#include <QList>
#include <QString>
#include <QVector3D>
#include <functional>

#include "scene.h"

namespace Physics {

struct Body {
    QVector3D centre;    // Up is +y.
    QVector3D half;      // Half its size along each axis.
    QVector3D velocity;
    double mass = 1;
    double bounce = .3;    // 0 lands dead, 1 bounces back as high.
    double friction = .5;  // How much sliding slows it.
    bool fixed = false;    // Collided with but never moved.
};

// Steps bodies through time: gravity (down is -y), the ground's height at
// (x, z) (NaN where there's none), collisions between them. Returns their
// centres at each of frames frames, dt apart.
QList<QList<QVector3D>> simulate(QList<Body> bodies, double gravity, const std::function<double(double, double)> &ground,
                                 double dt, int frames);

// A layer as a body (its "body" in scene.json: mass, bounce, friction,
// fixed), or not (an empty object takes it out).
bool setBody(Scene &scene, int layer, const QJsonObject &body, QString *error);

// Simulates every body in the scene from one time to another, starting each
// moving as its keys have it then, and bakes where they go as keys of
// their position (worldX/Y/Z in a world, x and y on a flat canvas).
// Returns how many layers moved.
int bake(Scene &scene, AssetCache &assets, double from, double to, QString *error);

}
