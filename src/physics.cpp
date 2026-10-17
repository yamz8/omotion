#include "physics.h"

#include <QJsonObject>
#include <cmath>

#include "world.h"

namespace Physics {

namespace {
constexpr double degrees = 180 / M_PI;

// How hard a body is to turn about each of its own axes: a box's.
QVector3D inertia(const Body &b) {
    const double x = b.half.x() * b.half.x(), y = b.half.y() * b.half.y(), z = b.half.z() * b.half.z();
    return QVector3D(float(b.mass * (y + z) / 3), float(b.mass * (x + z) / 3), float(b.mass * (x + y) / 3));
}

double inverseMass(const Body &b) { return b.fixed ? 0 : 1 / b.mass; }

// A twist (torque or impulse about its middle) as the turning it causes:
// taken into the body's own axes, where its box is square, and back.
QVector3D turnedBy(const Body &b, const QVector3D &twist) {
    if (b.fixed || !b.tumble)
        return {};
    const QVector3D own = b.turn.inverted().rotatedVector(twist), hard = inertia(b);
    QVector3D change;
    for (int k = 0; k < 3; ++k)
        change[k] = float(own[k] / std::max(1e-9, double(hard[k])));
    return b.turn.rotatedVector(change);
}

// Turning in radians a second, as the simulation works.
QVector3D turning(const Body &b) { return b.spin / float(degrees); }

// How a point on it, r from its middle, moves.
QVector3D pointVelocity(const Body &b, const QVector3D &r) { return b.velocity + QVector3D::crossProduct(turning(b), r); }

// How much a point r from its middle gives to a push along a direction:
// what a push of one moves it by, for working out the push that stops it.
double give(const Body &b, const QVector3D &r, const QVector3D &direction) {
    if (b.fixed)
        return 0;
    const QVector3D swing = QVector3D::crossProduct(turnedBy(b, QVector3D::crossProduct(r, direction)), r);
    return 1 / b.mass + QVector3D::dotProduct(swing, direction);
}

void push(Body &b, const QVector3D &r, const QVector3D &impulse) {
    if (b.fixed)
        return;
    b.velocity += impulse / float(b.mass);
    b.spin += turnedBy(b, QVector3D::crossProduct(r, impulse)) * float(degrees);
}

// Rubbing at a contact: a push against how the point slides, no more than
// friction times how hard they press (pressed, the share that stops it).
void rub(Body &a, const QVector3D &ra, Body *b, const QVector3D &rb, const QVector3D &normal, double friction, double pressed) {
    QVector3D slide = pointVelocity(a, ra) - (b ? pointVelocity(*b, rb) : QVector3D());
    slide -= normal * QVector3D::dotProduct(slide, normal);
    const float speed = slide.length();
    if (speed < 1e-6f || friction <= 0 || pressed <= 0)
        return;
    const QVector3D along = slide / speed;
    const double share = give(a, ra, along) + (b ? give(*b, rb, along) : 0);
    if (share <= 0)
        return;
    const double stop = std::min(speed / share, friction * pressed);
    push(a, ra, along * float(-stop));
    if (b)
        push(*b, rb, along * float(stop));
}

// The corners of its box where it is now; a flat body's only across and
// up, as it only turns in the picture's plane.
QList<QVector3D> corners(const Body &b) {
    const bool flat = b.half.z() > 1e5f;
    QList<QVector3D> list;
    for (int c = 0; c < (flat ? 4 : 8); ++c) {
        const QVector3D own((c & 1 ? 1 : -1) * b.half.x(), (c & 2 ? 1 : -1) * b.half.y(), flat ? 0 : (c & 4 ? 1 : -1) * b.half.z());
        list << b.centre + b.turn.rotatedVector(own);
    }
    return list;
}

// How far it reaches along each axis as it's turned: the box square to the
// axes around it, for meeting other bodies.
QVector3D reach(const Body &b) {
    if (b.half.z() > 1e5f) {
        QVector3D r;
        for (const QVector3D &c : corners(b))
            r = QVector3D(std::max(r.x(), std::abs(c.x() - b.centre.x())), std::max(r.y(), std::abs(c.y() - b.centre.y())), b.half.z());
        return r;
    }
    QVector3D r;
    for (const QVector3D &c : corners(b))
        for (int k = 0; k < 3; ++k)
            r[k] = std::max(r[k], std::abs(c[k] - b.centre[k]));
    return r;
}
}

QList<QList<Pose>> simulate(QList<Body> bodies, double gravity, const std::function<double(double, double)> &ground,
                            double dt, int frames) {
    QList<QList<Pose>> path(bodies.size());
    constexpr int substeps = 8;
    const double h = dt / substeps;
    for (Body &b : bodies) {
        b.turn = QQuaternion();
        if (!b.tumble || b.fixed)
            b.spin = QVector3D();
    }
    for (int f = 0; f < frames; ++f) {
        for (int i = 0; i < bodies.size(); ++i)
            path[i] << Pose{bodies.at(i).centre, bodies.at(i).turn};
        for (int step = 0; step < substeps; ++step) {
            for (Body &b : bodies) {
                if (b.fixed)
                    continue;
                b.velocity.setY(b.velocity.y() - float(gravity * h));
                b.centre += b.velocity * float(h);
                const QVector3D w = turning(b);
                if (!w.isNull())
                    b.turn = (QQuaternion::fromAxisAndAngle(w.normalized(), float(w.length() * h * degrees)) * b.turn).normalized();
                // Its corners under the ground: lifted out by the deepest,
                // then each pushed back up (bouncing when it hit hard),
                // and rubbed along it.
                QList<QVector3D> touching;
                double deepest = 0;
                for (const QVector3D &corner : corners(b)) {
                    const double floor = ground(corner.x(), corner.z());
                    if (!std::isnan(floor) && corner.y() < floor) {
                        touching << corner;
                        deepest = std::max(deepest, floor - corner.y());
                    }
                }
                if (touching.isEmpty())
                    continue;
                b.centre.setY(b.centre.y() + float(deepest));
                const QVector3D up(0, 1, 0);
                for (int pass = 0; pass < 2; ++pass)
                    for (const QVector3D &corner : touching) {
                        const QVector3D r = corner - b.centre;
                        const double closing = pointVelocity(b, r).y();
                        if (closing >= 0)
                            continue;
                        // Too slow to bounce: it settles.
                        const double bounce = pass == 0 && -closing > gravity * h * 4 ? b.bounce : 0;
                        const double pressed = -(1 + bounce) * closing / give(b, r, up);
                        push(b, r, up * float(pressed));
                        rub(b, r, nullptr, {}, up, b.friction, pressed);
                    }
                // A box rolls badly: turning on the ground dies away.
                b.spin *= float(std::max(0.0, 1 - h * 2));
            }
            // Each pair that overlaps is pushed apart along where they
            // overlap least, and their speeds that way exchanged, as boxes
            // that bounce off each other; how they rub turns them.
            for (int i = 0; i < bodies.size(); ++i)
                for (int j = i + 1; j < bodies.size(); ++j) {
                    Body &a = bodies[i], &b = bodies[j];
                    if (a.fixed && b.fixed)
                        continue;
                    const QVector3D ha = reach(a), hb = reach(b);
                    const QVector3D d = b.centre - a.centre;
                    int axis = -1;
                    float least = 1e30f;
                    for (int k = 0; k < 3; ++k) {
                        const float overlap = ha[k] + hb[k] - std::abs(d[k]);
                        if (overlap <= 0) {
                            axis = -1;
                            break;
                        }
                        if (overlap < least)
                            least = overlap, axis = k;
                    }
                    if (axis < 0)
                        continue;
                    QVector3D normal;
                    normal[axis] = d[axis] >= 0 ? 1.f : -1.f;
                    const double ia = inverseMass(a), ib = inverseMass(b);
                    const double share = ia + ib;
                    a.centre -= normal * float(least * ia / share);
                    b.centre += normal * float(least * ib / share);
                    // They touch in the middle of where they overlap, on
                    // the face between them.
                    QVector3D contact;
                    for (int k = 0; k < 3; ++k)
                        contact[k] = k == axis ? a.centre[k] + normal[k] * ha[k]
                                               : (std::max(a.centre[k] - ha[k], b.centre[k] - hb[k]) +
                                                  std::min(a.centre[k] + ha[k], b.centre[k] + hb[k])) / 2;
                    const QVector3D ra = contact - a.centre, rb = contact - b.centre;
                    const double closing = QVector3D::dotProduct(b.velocity - a.velocity, normal);
                    double pressed = 0;
                    if (closing < 0) {
                        pressed = -(1 + std::min(a.bounce, b.bounce)) * closing / share;
                        a.velocity -= normal * float(pressed * ia);
                        b.velocity += normal * float(pressed * ib);
                    }
                    rub(b, rb, &a, ra, normal, std::sqrt(a.friction * b.friction), pressed);
                }
        }
    }
    return path;
}

bool setBody(Scene &scene, int index, const QJsonObject &body, QString *error) {
    if (index < 0 || index >= scene.layers.size()) {
        if (error)
            *error = "There's no such layer.";
        return false;
    }
    Layer &layer = scene.layers[index];
    if (!layer.particles.isEmpty() || layer.raw.value("empty").toBool()) {
        if (error)
            *error = QString("%1 has nothing solid to be a body.").arg(layer.name);
        return false;
    }
    if (body.isEmpty())
        layer.raw.remove("body");
    else
        layer.raw["body"] = body;
    return true;
}

namespace {
QRectF solid(const QImage &image) {
    QRect box;
    const QImage argb = image.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < argb.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(argb.constScanLine(y));
        for (int x = 0; x < argb.width(); ++x)
            if (qAlpha(line[x]) > 128)
                box |= QRect(x, y, 1, 1);
    }
    return QRectF(box);
}
}

int bake(Scene &scene, AssetCache &assets, double from, double to, QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return -1;
    };
    QList<int> layers;
    for (int i = 0; i < scene.layers.size(); ++i)
        if (!scene.layers.at(i).raw.value("body").toObject().isEmpty())
            layers << i;
    if (layers.isEmpty())
        return fail("Nothing is a body yet: make some pieces bodies first.");
    from = std::clamp(from, 0.0, scene.duration);
    to = std::clamp(to, from, scene.duration);
    const double dt = 1 / std::max(1.0, scene.fps);
    const int frames = int(std::lround((to - from) / dt)) + 1;
    const bool world = World::info(scene).valid;
    // A world in meters, a flat canvas in pixels with up as less y.
    const double gravity = world ? 9.81 : 2000;
    auto where = [&](int index, double t) -> std::pair<QVector3D, QVector3D> {
        if (world) {
            QList<QVector3D> corners;
            QVector3D pivot;
            if (!World::box(scene, assets, index, t, &corners, &pivot))
                return {};
            QVector3D low = corners.first(), high = corners.first();
            for (const QVector3D &c : corners)
                for (int k = 0; k < 3; ++k)
                    low[k] = std::min(low[k], c[k]), high[k] = std::max(high[k], c[k]);
            return {(low + high) / 2, (high - low) / 2};
        }
        const Layer &layer = scene.layers.at(index);
        const QRectF box = layerTransform(scene, assets, index, t).mapRect(solid(assets.image(layer.source)));
        // As deep as can be, so they only ever push each other across and up.
        return {QVector3D(float(box.center().x()), float(-box.center().y()), 0), QVector3D(float(box.width() / 2), float(box.height() / 2), 1e6f)};
    };
    QList<Body> bodies;
    for (const int index : layers) {
        const QJsonObject settings = scene.layers.at(index).raw.value("body").toObject();
        const auto [centre, half] = where(index, from);
        if (half.isNull())
            return fail(QString("%1 has no size to be a body.").arg(scene.layers.at(index).name));
        // It starts moving as its keys have it moving then.
        const auto [later, unused] = where(index, std::min(scene.duration, from + dt));
        Q_UNUSED(unused)
        Body body;
        body.centre = centre;
        body.half = half;
        body.velocity = from + dt <= scene.duration ? (later - centre) / float(dt) : QVector3D();
        body.mass = std::max(1e-3, settings.value("mass").toDouble(1));
        body.bounce = std::clamp(settings.value("bounce").toDouble(.3), 0.0, 1.0);
        body.friction = std::clamp(settings.value("friction").toDouble(.5), 0.0, 1.0);
        body.fixed = settings.value("fixed").toBool();
        body.tumble = settings.value("tumble").toBool(true);
        // It turns as its keys have it turning then, in the simulation's
        // axes (up is +y, and toward the viewer +z).
        if (from + dt <= scene.duration) {
            const Layer &layer = scene.layers.at(index);
            auto rate = [&](const char *p) { return float((scene.value(layer, p, from + dt) - scene.value(layer, p, from)) / dt); };
            body.spin = world ? QVector3D(rate("pitch"), -rate("yaw"), -rate("roll")) : QVector3D(0, 0, -rate("rotation"));
        }
        bodies << body;
    }
    const double floorY = -scene.raw.value("floor").toDouble(scene.height);
    auto ground = [&](double x, double z) {
        return world ? World::groundAt(scene, assets, x, z) : floorY;
    };
    const QList<QList<Pose>> path = simulate(bodies, gravity, ground, dt, frames);
    int moved = 0;
    const Scene before = scene;
    for (int b = 0; b < layers.size(); ++b) {
        if (bodies.at(b).fixed)
            continue;
        const int index = layers.at(b);
        Layer &layer = scene.layers[index];
        const Layer &was = before.layers.at(index);
        const QList<Pose> &poses = path.at(b);
        // Its turn only when it turned, so an upright body keeps its keys.
        bool turned = false;
        for (const Pose &pose : poses)
            turned = turned || std::abs(pose.turn.scalar()) < 1 - 1e-7f;
        QStringList properties = world ? QStringList{"worldX", "worldY", "worldZ"} : QStringList{"x", "y"};
        if (turned)
            properties << (world ? QStringList{"pitch", "yaw", "roll"} : QStringList{"rotation"});
        QHash<QString, QList<double>> values;
        // It turns about its pivot, not the middle of its box: where the
        // pivot goes is where the box's middle went, less the turned way
        // from the pivot to the middle.
        if (world) {
            const QVector3D pivot = World::pivot(before, assets, index);
            const QVector3D move0(float(before.value(was, "worldX", from)), float(before.value(was, "worldY", from)), float(-before.value(was, "worldZ", from)));
            const QQuaternion start = World::turn(before.value(was, "yaw", from), before.value(was, "pitch", from), before.value(was, "roll", from));
            const QVector3D toMiddle = poses.first().centre - pivot - move0;
            QVector3D last;
            for (int f = 0; f < frames; ++f) {
                const Pose &pose = poses.at(f);
                const QVector3D move = pose.centre - pivot - pose.turn.rotatedVector(toMiddle);
                values["worldX"] << move.x();
                values["worldY"] << move.y();
                values["worldZ"] << -move.z();
                // As yaw, pitch and roll again, kept going the same way
                // round rather than jumping at 180°.
                const QVector3D e = (pose.turn * start).toEulerAngles();
                QVector3D angles(-e.y(), e.x(), -e.z());
                if (f > 0)
                    for (int k = 0; k < 3; ++k)
                        angles[k] = last[k] + float(std::remainder(angles[k] - last[k], 360.0));
                else
                    angles = QVector3D(float(before.value(was, "yaw", from)), float(before.value(was, "pitch", from)), float(before.value(was, "roll", from)));
                last = angles;
                values["yaw"] << angles.x();
                values["pitch"] << angles.y();
                values["roll"] << angles.z();
            }
        } else {
            bool invertible = true;
            const QTransform back = parentTransform(before, assets, index, from).inverted(&invertible);
            if (!invertible)
                continue;
            const QSizeF size = assets.size(was.source);
            const QPointF pivot = layerTransform(before, assets, index, from).map(QPointF(was.anchor.x() * size.width(), was.anchor.y() * size.height()));
            const QPointF middle(poses.first().centre.x(), -poses.first().centre.y());
            const QPointF here = back.map(pivot);
            double angle = 0;
            for (int f = 0; f < frames; ++f) {
                const Pose &pose = poses.at(f);
                // Turned in the canvas's plane: counter-clockwise as seen is
                // less rotation, the canvas's y running down.
                const double z = 2 * qRadiansToDegrees(std::atan2(double(pose.turn.z()), double(pose.turn.scalar())));
                angle = f == 0 ? 0 : angle + std::remainder(-z - angle, 360.0);
                const QPointF now(pose.centre.x(), -pose.centre.y());
                const QPointF goes = now - QTransform().rotate(angle).map(middle - pivot);
                const QPointF inParent = back.map(goes);
                values["x"] << before.ownValue(was, "x", from) + inParent.x() - here.x();
                values["y"] << before.ownValue(was, "y", from) + inParent.y() - here.y();
                values["rotation"] << before.ownValue(was, "rotation", from) + angle;
            }
        }
        for (const QString &p : properties) {
            QList<Key> keys;
            for (const Key &key : layer.keys.value(p))
                if (key.t < from - 1e-9 || key.t > to + 1e-9)
                    keys << key;
            for (int f = 0; f < frames; ++f)
                keys << Key{from + f * dt, std::round(values.value(p).at(f) * 1e4) / 1e4, "linear"};
            std::sort(keys.begin(), keys.end(), [](const Key &a, const Key &k) { return a.t < k.t; });
            layer.keys[p] = keys;
        }
        ++moved;
    }
    return moved;
}

}
