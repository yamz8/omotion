#include "physics.h"

#include <QJsonObject>
#include <cmath>

#include "world.h"

namespace Physics {

QList<QList<QVector3D>> simulate(QList<Body> bodies, double gravity, const std::function<double(double, double)> &ground,
                                 double dt, int frames) {
    QList<QList<QVector3D>> path(bodies.size());
    constexpr int substeps = 8;
    const double h = dt / substeps;
    for (int f = 0; f < frames; ++f) {
        for (int i = 0; i < bodies.size(); ++i)
            path[i] << bodies.at(i).centre;
        for (int step = 0; step < substeps; ++step) {
            for (Body &b : bodies) {
                if (b.fixed)
                    continue;
                b.velocity.setY(b.velocity.y() - float(gravity * h));
                b.centre += b.velocity * float(h);
                // The ground under its middle.
                const double floor = ground(b.centre.x(), b.centre.z());
                if (!std::isnan(floor) && b.centre.y() - b.half.y() < floor) {
                    b.centre.setY(float(floor + b.half.y()));
                    if (b.velocity.y() < 0) {
                        const float up = float(-b.velocity.y() * b.bounce);
                        // Too slow to bounce: it settles.
                        b.velocity.setY(up < gravity * h * 4 ? 0.f : up);
                    }
                    const float slow = float(std::max(0.0, 1 - b.friction * h * 6));
                    b.velocity.setX(b.velocity.x() * slow);
                    b.velocity.setZ(b.velocity.z() * slow);
                }
            }
            // Each pair that overlaps is pushed apart along where they
            // overlap least, and their speeds that way exchanged, as boxes
            // that bounce off each other.
            for (int i = 0; i < bodies.size(); ++i)
                for (int j = i + 1; j < bodies.size(); ++j) {
                    Body &a = bodies[i], &b = bodies[j];
                    if (a.fixed && b.fixed)
                        continue;
                    const QVector3D d = b.centre - a.centre;
                    int axis = -1;
                    float least = 1e30f;
                    for (int k = 0; k < 3; ++k) {
                        const float overlap = a.half[k] + b.half[k] - std::abs(d[k]);
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
                    const double ia = a.fixed ? 0 : 1 / a.mass, ib = b.fixed ? 0 : 1 / b.mass;
                    const double share = ia + ib;
                    a.centre -= normal * float(least * ia / share);
                    b.centre += normal * float(least * ib / share);
                    const double closing = QVector3D::dotProduct(b.velocity - a.velocity, normal);
                    if (closing < 0) {
                        const double impulse = -(1 + std::min(a.bounce, b.bounce)) * closing / share;
                        a.velocity -= normal * float(impulse * ia);
                        b.velocity += normal * float(impulse * ib);
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
        bodies << body;
    }
    const double floorY = -scene.raw.value("floor").toDouble(scene.height);
    auto ground = [&](double x, double z) {
        return world ? World::groundAt(scene, assets, x, z) : floorY;
    };
    const QList<QList<QVector3D>> path = simulate(bodies, gravity, ground, dt, frames);
    int moved = 0;
    const Scene before = scene;
    for (int b = 0; b < layers.size(); ++b) {
        if (bodies.at(b).fixed)
            continue;
        const int index = layers.at(b);
        Layer &layer = scene.layers[index];
        const QStringList properties = world ? QStringList{"worldX", "worldY", "worldZ"} : QStringList{"x", "y"};
        QHash<QString, double> start;
        for (const QString &p : properties)
            start[p] = before.ownValue(before.layers.at(index), p, from);
        // A flat layer's move is in its parent's pixels.
        bool invertible = true;
        const QTransform back = world ? QTransform() : parentTransform(before, assets, index, from).inverted(&invertible);
        if (!invertible)
            continue;
        for (const QString &p : properties) {
            QList<Key> keys;
            for (const Key &key : layer.keys.value(p))
                if (key.t < from - 1e-9 || key.t > to + 1e-9)
                    keys << key;
            for (int f = 0; f < frames; ++f) {
                const QVector3D shift = path.at(b).at(f) - path.at(b).first();
                double v;
                if (world)
                    v = start[p] + (p == "worldX" ? shift.x() : p == "worldY" ? shift.y() : -shift.z());
                else {
                    const QPointF onCanvas(shift.x(), -shift.y());
                    const QPointF inParent = back.map(onCanvas) - back.map(QPointF());
                    v = start[p] + (p == "x" ? inParent.x() : inParent.y());
                }
                keys << Key{from + f * dt, std::round(v * 1e4) / 1e4, "linear"};
            }
            std::sort(keys.begin(), keys.end(), [](const Key &a, const Key &k) { return a.t < k.t; });
            layer.keys[p] = keys;
        }
        ++moved;
    }
    return moved;
}

}
