#include "physics.h"

#include <QJsonObject>
#include <QtMath>
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
    QList<QVector3D> list;
    if (!b.points.isEmpty()) {
        for (const QVector3D &p : b.points)
            list << b.centre + b.turn.rotatedVector(p);
        return list;
    }
    const bool flat = b.half.z() > 1e5f;
    for (int c = 0; c < (flat ? 4 : 8); ++c) {
        const QVector3D own((c & 1 ? 1 : -1) * b.half.x(), (c & 2 ? 1 : -1) * b.half.y(), flat ? 0 : (c & 4 ? 1 : -1) * b.half.z());
        list << b.centre + b.turn.rotatedVector(own);
    }
    return list;
}

// Two turned boxes against each other, as the separating axis test has
// it: if no line (a face's way, or across an edge of each) keeps them
// apart, they overlap, and least along one of those: which way (from a to
// b), how deep, and where they touch (the corners of one inside the other
// at the face they meet, or between their edges where edges cross).
// Two bodies' own shapes against each other, by the same test: every way a
// face of either looks (and across their main axes in 3D) tried as a line
// that might keep them apart, their points' spans along it compared.
bool overlapShapes(const Body &a, const Body &b, QVector3D *normal, float *depth, QList<QVector3D> *contacts) {
    const bool flat = a.half.z() > 1e5f || b.half.z() > 1e5f;
    auto facesOf = [&](const Body &body) {
        QList<QVector3D> list;
        if (!body.faces.isEmpty())
            for (const QVector3D &f : body.faces)
                list << body.turn.rotatedVector(f);
        else
            for (int k = 0; k < (flat ? 2 : 3); ++k) {
                QVector3D unit;
                unit[k] = 1;
                list << body.turn.rotatedVector(unit) << -body.turn.rotatedVector(unit);
            }
        return list;
    };
    const QList<QVector3D> pa = corners(a), pb = corners(b), fa = facesOf(a), fb = facesOf(b);
    QList<QVector3D> axes = fa + fb;
    if (!flat)
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                QVector3D ui, uj;
                ui[i] = 1;
                uj[j] = 1;
                const QVector3D across = QVector3D::crossProduct(a.turn.rotatedVector(ui), b.turn.rotatedVector(uj));
                if (across.length() > 1e-3f)
                    axes << across.normalized();
            }
    auto span = [](const QList<QVector3D> &points, const QVector3D &along) {
        double low = 1e30, high = -1e30;
        for (const QVector3D &p : points) {
            const double d = QVector3D::dotProduct(p, along);
            low = std::min(low, d), high = std::max(high, d);
        }
        return std::make_pair(low, high);
    };
    double least = 1e30;
    QVector3D best;
    for (const QVector3D &along : axes) {
        const auto [la, ha] = span(pa, along);
        const auto [lb, hb] = span(pb, along);
        const double gap = std::min(ha, hb) - std::max(la, lb);
        if (gap <= 0)
            return false;
        if (gap < least)
            least = gap, best = (lb + hb) / 2 >= (la + ha) / 2 ? along : -along;
    }
    *normal = best;
    *depth = float(least);
    // Points of each inside the other (within all its faces), where they
    // touch; else between the points of each deepest in the other.
    auto inside = [&](const QList<QVector3D> &points, const QList<QVector3D> &faces, const QList<QVector3D> &own) {
        QList<QVector3D> found;
        for (const QVector3D &p : points) {
            bool in = true;
            for (const QVector3D &f : faces) {
                double most = -1e30;
                for (const QVector3D &q : own)
                    most = std::max(most, double(QVector3D::dotProduct(q, f)));
                if (QVector3D::dotProduct(p, f) > most + 1e-3 * std::max(1.0, std::abs(most))) {
                    in = false;
                    break;
                }
            }
            if (in)
                found << p;
        }
        return found;
    };
    *contacts = inside(pb, fa, pa) + inside(pa, fb, pb);
    if (contacts->isEmpty()) {
        auto deepest = [](const QList<QVector3D> &points, const QVector3D &way) {
            QVector3D found;
            double most = -1e30;
            for (const QVector3D &p : points)
                if (QVector3D::dotProduct(p, way) > most)
                    most = QVector3D::dotProduct(p, way), found = p;
            return found;
        };
        *contacts << (deepest(pa, best) + deepest(pb, -best)) / 2;
    }
    return true;
}

bool overlap(const Body &a, const Body &b, QVector3D *normal, float *depth, QList<QVector3D> *contacts) {
    if (!a.points.isEmpty() || !b.points.isEmpty())
        return overlapShapes(a, b, normal, depth, contacts);
    const bool flat = a.half.z() > 1e5f || b.half.z() > 1e5f;
    const int sides = flat ? 2 : 3;
    QVector3D axesA[3], axesB[3];
    for (int k = 0; k < 3; ++k) {
        QVector3D unit;
        unit[k] = 1;
        axesA[k] = a.turn.rotatedVector(unit);
        axesB[k] = b.turn.rotatedVector(unit);
    }
    auto radius = [&](const Body &body, const QVector3D *axes, const QVector3D &along) {
        double r = 0;
        for (int k = 0; k < sides; ++k)
            r += body.half[k] * std::abs(QVector3D::dotProduct(axes[k], along));
        return r;
    };
    QList<QVector3D> candidates;
    QList<int> kinds;  // 0 a face, 1 b face, 2 edges.
    for (int k = 0; k < sides; ++k)
        candidates << axesA[k], kinds << 0;
    for (int k = 0; k < sides; ++k)
        candidates << axesB[k], kinds << 1;
    if (!flat)
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                const QVector3D across = QVector3D::crossProduct(axesA[i], axesB[j]);
                if (across.length() > 1e-3f)
                    candidates << across.normalized(), kinds << 2;
            }
    const QVector3D between = b.centre - a.centre;
    double least = 1e30;
    int best = -1;
    for (int c = 0; c < candidates.size(); ++c) {
        const QVector3D &along = candidates.at(c);
        const double gap = radius(a, axesA, along) + radius(b, axesB, along) - std::abs(QVector3D::dotProduct(between, along));
        if (gap <= 0)
            return false;
        // Faces are preferred a little over edges, which wobble.
        const double weighed = kinds.at(c) == 2 ? gap * 1.05 : gap;
        if (weighed < least)
            least = weighed, best = c;
    }
    QVector3D n = candidates.at(best);
    if (QVector3D::dotProduct(between, n) < 0)
        n = -n;
    *normal = n;
    *depth = float(radius(a, axesA, n) + radius(b, axesB, n) - std::abs(QVector3D::dotProduct(between, n)));
    // The corners of one inside the other, at the face they meet; edges
    // crossing touch between the corners of each deepest in the other.
    auto inside = [&](const Body &body, const Body &other, const QVector3D *otherAxes) {
        QList<QVector3D> found;
        const double slack = .02 * std::max({other.half.x(), other.half.y(), flat ? 0.f : other.half.z()});
        for (const QVector3D &p : corners(body)) {
            bool in = true;
            for (int k = 0; k < sides && in; ++k)
                in = std::abs(QVector3D::dotProduct(p - other.centre, otherAxes[k])) <= other.half[k] + slack;
            if (in)
                found << p;
        }
        return found;
    };
    auto deepest = [&](const Body &body, const QVector3D &way) {
        QVector3D found;
        double most = -1e30;
        for (const QVector3D &p : corners(body))
            if (QVector3D::dotProduct(p, way) > most)
                most = QVector3D::dotProduct(p, way), found = p;
        return found;
    };
    contacts->clear();
    if (kinds.at(best) == 0)
        *contacts = inside(b, a, axesA);
    else if (kinds.at(best) == 1)
        *contacts = inside(a, b, axesB);
    if (contacts->isEmpty())
        *contacts << (deepest(a, n) + deepest(b, -n)) / 2;
    return true;
}
}

QList<QList<Pose>> simulate(QList<Body> bodies, double gravity, const std::function<double(double, double)> &ground,
                            double dt, int frames) {
    QList<QList<Pose>> path(bodies.size());
    constexpr int substeps = 8;
    const double h = dt / substeps;
    // Each starts turned as it is; its poses say how it has turned since.
    QList<QQuaternion> starts;
    for (Body &b : bodies) {
        b.turn.normalize();
        starts << b.turn;
        if (!b.tumble || b.fixed)
            b.spin = QVector3D();
    }
    for (int f = 0; f < frames; ++f) {
        for (int i = 0; i < bodies.size(); ++i)
            path[i] << Pose{bodies.at(i).centre, (bodies.at(i).turn * starts.at(i).inverted()).normalized()};
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
                // A box rolls badly: turning on the ground dies away; an
                // outline of its own rolls as far as its shape lets it.
                b.spin *= float(std::max(0.0, 1 - h * (b.points.isEmpty() ? 2 : .3)));
            }
            // Each pair that overlaps is pushed apart the way they overlap
            // least, and pushed off each other where they touch: bouncing,
            // rubbing and turning about it.
            for (int i = 0; i < bodies.size(); ++i)
                for (int j = i + 1; j < bodies.size(); ++j) {
                    Body &a = bodies[i], &b = bodies[j];
                    if (a.fixed && b.fixed)
                        continue;
                    QVector3D normal;
                    QList<QVector3D> contacts;
                    float depth = 0;
                    if (!overlap(a, b, &normal, &depth, &contacts))
                        continue;
                    const double ia = inverseMass(a), ib = inverseMass(b);
                    const double share = ia + ib;
                    a.centre -= normal * float(depth * ia / share);
                    b.centre += normal * float(depth * ib / share);
                    for (int pass = 0; pass < 2; ++pass)
                        for (const QVector3D &contact : contacts) {
                            const QVector3D ra = contact - a.centre, rb = contact - b.centre;
                            const double closing = QVector3D::dotProduct(pointVelocity(b, rb) - pointVelocity(a, ra), normal);
                            if (closing >= 0)
                                continue;
                            const double bounce = pass == 0 && -closing > gravity * h * 4 ? std::min(a.bounce, b.bounce) : 0;
                            const double pressed = -(1 + bounce) * closing / (give(a, ra, normal) + give(b, rb, normal));
                            push(a, ra, normal * float(-pressed));
                            push(b, rb, normal * float(pressed));
                            rub(b, rb, &a, ra, normal, std::sqrt(a.friction * b.friction), pressed);
                        }
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

namespace {
// The outline of a picture's solid part, as a convex polygon of at most
// some points (dropping those that add least to it), in its pixels.
QList<QPointF> hullOf(const QImage &image) {
    const QImage argb = image.convertToFormat(QImage::Format_ARGB32);
    QList<QPointF> edge;
    for (int y = 0; y < argb.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(argb.constScanLine(y));
        int first = -1, last = -1;
        for (int x = 0; x < argb.width(); ++x)
            if (qAlpha(line[x]) > 128) {
                if (first < 0)
                    first = x;
                last = x;
            }
        if (first >= 0)
            edge << QPointF(first, y) << QPointF(last + 1, y) << QPointF(first, y + 1) << QPointF(last + 1, y + 1);
    }
    if (edge.size() < 3)
        return {};
    std::sort(edge.begin(), edge.end(), [](const QPointF &a, const QPointF &b) { return a.x() < b.x() || (a.x() == b.x() && a.y() < b.y()); });
    auto cross = [](const QPointF &o, const QPointF &a, const QPointF &b) { return (a.x() - o.x()) * (b.y() - o.y()) - (a.y() - o.y()) * (b.x() - o.x()); };
    // Andrew's monotone chain: the lower and upper runs.
    QList<QPointF> hull;
    for (int pass = 0; pass < 2; ++pass) {
        const qsizetype start = hull.size();
        for (qsizetype k = 0; k < edge.size(); ++k) {
            const QPointF &p = pass == 0 ? edge.at(k) : edge.at(edge.size() - 1 - k);
            while (hull.size() >= start + 2 && cross(hull.at(hull.size() - 2), hull.last(), p) <= 0)
                hull.removeLast();
            hull << p;
        }
        hull.removeLast();
    }
    while (hull.size() > 24) {
        qsizetype weakest = 0;
        double smallest = 1e30;
        for (qsizetype k = 0; k < hull.size(); ++k) {
            const double area = std::abs(cross(hull.at((k + hull.size() - 1) % hull.size()), hull.at(k), hull.at((k + 1) % hull.size())));
            if (area < smallest)
                smallest = area, weakest = k;
        }
        hull.removeAt(weakest);
    }
    return hull;
}

// The ways to look for a 3D shape's farthest points: along the axes, and
// across their edges and corners.
QList<QVector3D> lookouts() {
    QList<QVector3D> ways;
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y)
            for (int z = -1; z <= 1; ++z)
                if (x || y || z)
                    ways << QVector3D(float(x), float(y), float(z)).normalized();
    return ways;
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
    // Where a body is, how big (half its size along its own sides) and how
    // it's turned: a world piece's box as it stands, a flat layer's solid
    // part as it's drawn, in the simulation's axes (up is +y).
    struct Place {
        QVector3D centre, half;
        QQuaternion turn;
        QList<QVector3D> points, faces;  // Its own shape, when it has one.
    };
    auto where = [&](int index, double t) -> Place {
        if (world) {
            QList<QVector3D> corners;
            QVector3D pivot;
            if (!World::box(scene, assets, index, t, &corners, &pivot) || corners.size() != 8)
                return {};
            // Its corners go along its own x, then y, then z.
            const QVector3D x = corners.at(1) - corners.at(0), y = corners.at(2) - corners.at(0), z = corners.at(4) - corners.at(0);
            if (x.length() < 1e-6f || y.length() < 1e-6f || z.length() < 1e-6f)
                return {};
            Place place{(corners.at(0) + corners.at(7)) / 2, QVector3D(x.length(), y.length(), z.length()) / 2,
                        QQuaternion::fromAxes(x.normalized(), y.normalized(), z.normalized()), {}, {}};
            // A shaped piece: its mesh's farthest points every way, put
            // where its box is now (its rest box's corners give the map).
            const Layer &layer = scene.layers.at(index);
            const World::Mesh shape = layer.raw.value("shape").toObject().isEmpty() ? World::Mesh() : World::mesh(scene, assets, index);
            if (!shape.isNull()) {
                const QVector3D low = shape.minimum, size = shape.maximum - shape.minimum;
                const float *v = reinterpret_cast<const float *>(shape.vertices.constData());
                const qsizetype count = shape.vertices.size() / qsizetype(shape.stride * sizeof(float));
                const QQuaternion back = place.turn.inverted();
                for (const QVector3D &way : lookouts()) {
                    double most = -1e30;
                    QVector3D found;
                    for (qsizetype k = 0; k < count; ++k) {
                        const QVector3D rest(v[k * shape.stride], v[k * shape.stride + 1], v[k * shape.stride + 2]);
                        if (QVector3D::dotProduct(rest, way) > most)
                            most = QVector3D::dotProduct(rest, way), found = rest;
                    }
                    const QVector3D f = (found - low) / QVector3D(std::max(size.x(), 1e-6f), std::max(size.y(), 1e-6f), std::max(size.z(), 1e-6f));
                    const QVector3D now = corners.at(0) + x * f.x() + y * f.y() + z * f.z();
                    const QVector3D own = back.rotatedVector(now - place.centre);
                    if (!place.points.contains(own))
                        place.points << own;
                    place.faces << way;
                }
            }
            return place;
        }
        const Layer &layer = scene.layers.at(index);
        const QTransform m = layerTransform(scene, assets, index, t);
        const QRectF box = solid(assets.image(layer.source));
        const QPointF centre = m.map(box.center());
        const double sx = std::hypot(m.m11(), m.m12()), sy = std::hypot(m.m21(), m.m22());
        const double angle = qRadiansToDegrees(std::atan2(m.m12(), m.m11()));
        // As deep as can be, so they only ever push each other across and up.
        Place place{QVector3D(float(centre.x()), float(-centre.y()), 0), QVector3D(float(box.width() / 2 * sx), float(box.height() / 2 * sy), 1e6f),
                    QQuaternion::fromAxisAndAngle(0, 0, 1, float(-angle)), {}, {}};
        // Its outline, when it isn't near enough its box: a ball, a wheel,
        // a rock, in its own axes (up is +y), its faces' outward ways.
        const QList<QPointF> hull = hullOf(assets.image(layer.source));
        double area = 0;
        for (qsizetype k = 0; k < hull.size(); ++k)
            area += hull.at(k).x() * hull.at((k + 1) % hull.size()).y() - hull.at((k + 1) % hull.size()).x() * hull.at(k).y();
        if (hull.size() >= 3 && std::abs(area) / 2 < box.width() * box.height() * .9) {
            const QQuaternion back = place.turn.inverted();
            QList<QVector3D> own;
            for (const QPointF &p : hull) {
                const QPointF c = m.map(p);
                own << back.rotatedVector(QVector3D(float(c.x()), float(-c.y()), 0) - place.centre);
            }
            // Wound the same way whatever the picture: faces look outward.
            double turning = 0;
            for (qsizetype k = 0; k < own.size(); ++k)
                turning += own.at(k).x() * own.at((k + 1) % own.size()).y() - own.at((k + 1) % own.size()).x() * own.at(k).y();
            for (qsizetype k = 0; k < own.size(); ++k) {
                const QVector3D along = own.at((k + 1) % own.size()) - own.at(k);
                QVector3D out = turning > 0 ? QVector3D(along.y(), -along.x(), 0) : QVector3D(-along.y(), along.x(), 0);
                if (out.length() > 1e-6f)
                    place.faces << out.normalized();
            }
            place.points = own;
        }
        return place;
    };
    QList<Body> bodies;
    for (const int index : layers) {
        const QJsonObject settings = scene.layers.at(index).raw.value("body").toObject();
        const Place place = where(index, from);
        if (place.half.isNull())
            return fail(QString("%1 has no size to be a body.").arg(scene.layers.at(index).name));
        // It starts moving as its keys have it moving then.
        const QVector3D later = where(index, std::min(scene.duration, from + dt)).centre;
        Body body;
        body.centre = place.centre;
        body.half = place.half;
        body.turn = place.turn;
        body.points = place.points;
        body.faces = place.faces;
        body.velocity = from + dt <= scene.duration ? (later - place.centre) / float(dt) : QVector3D();
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
