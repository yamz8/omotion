#include "liquid.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QPainter>
#include <QRadialGradient>
#include <QRandomGenerator>
#include <QtMath>
#include <cmath>
#include <memory>

namespace Liquid {

namespace {

struct Settings {
    QPointF point;
    QString from;
    QPointF at;
    bool atSet = false;
    double rate = 60, size = 4, speed = 120, direction = 90, spread = 10, gravity = 900, viscosity = .2;
    double floor = 0;
    QColor colour;
    quint32 seed = 1;
};

Settings settingsOf(const Scene &scene, const QJsonObject &p) {
    Settings s;
    s.point = QPointF(p.value("x").toDouble(scene.width / 2.0), p.value("y").toDouble(scene.height * .2));
    s.from = p.value("from").toString();
    const QJsonArray at = p.value("at").toArray();
    s.atSet = at.size() == 2;
    if (s.atSet)
        s.at = QPointF(at.at(0).toDouble(), at.at(1).toDouble());
    s.rate = std::clamp(p.value("rate").toDouble(150), 0.0, 600.0);
    s.size = std::clamp(p.value("size").toDouble(4), 1.0, 40.0);
    s.speed = p.value("speed").toDouble(120);
    s.direction = p.value("direction").toDouble(90);
    s.spread = p.value("spread").toDouble(10);
    s.gravity = p.value("gravity").toDouble(900);
    s.viscosity = std::clamp(p.value("viscosity").toDouble(.2), 0.0, 1.0);
    s.floor = p.value("floor").toDouble(scene.height);
    s.colour = QColor(p.value("color").toString("#5aa8e8"));
    s.seed = quint32(p.value("seed").toInt(1));
    return s;
}

QPointF sourceAt(const Scene &scene, AssetCache &assets, const Settings &s, double t) {
    const int layer = s.from.isEmpty() ? -1 : scene.indexOf(s.from);
    if (layer < 0)
        return s.point;
    const Layer &from = scene.layers.at(layer);
    const QSizeF size = assets.size(from.source);
    const QPointF point = s.atSet ? s.at : QPointF(from.anchor.x() * size.width(), from.anchor.y() * size.height());
    return layerTransform(scene, assets, layer, t).map(point);
}

struct Drop {
    QPointF p, v;
};

// A run from the start: the drops now, each frame's places, and where its
// source was, to tell when its source has moved since.
struct Run {
    QList<Drop> drops;
    QList<QList<QPointF>> frames;
    QList<QPointF> sources;
    qint64 born = 0;
};

QMutex runsLock;
QHash<QString, std::shared_ptr<Run>> runs;
constexpr int substeps = 3;
constexpr int most = 3000;

void step(Run &run, const Settings &s, const Scene &scene, const QPointF &source, double t, double dt) {
    // New drops, a few a step, as the rate has them: each from the source,
    // poured out its way, scattered a little.
    const qint64 due = qint64(std::floor((t + dt) * s.rate));
    for (; run.born < due; ++run.born) {
        QRandomGenerator random(s.seed * 2654435761u + quint32(run.born) * 40503u);
        const double angle = qDegreesToRadians(s.direction + s.spread * (random.generateDouble() * 2 - 1));
        const double pace = s.speed * (.85 + .3 * random.generateDouble());
        // Side by side across the stream, as wide as it must be for drops
        // to start a drop apart at this rate and speed: more water is a
        // wider pour.
        const QPointF way(std::cos(angle), std::sin(angle)), across(-way.y(), way.x());
        const double gap = s.size * 1.4;
        const int wide = std::max(1, int(std::lround(s.rate * gap / std::max(10.0, std::abs(s.speed)))));
        const double slot = (run.born % wide) - (wide - 1) / 2.0;
        const QPointF jitter = across * ((slot + (random.generateDouble() - .5) * .3) * gap) + way * (random.generateDouble() * s.size * .3);
        run.drops << Drop{source + jitter, way * pace};
        if (run.drops.size() > most)
            run.drops.removeFirst();
    }
    const int n = int(run.drops.size());
    if (n == 0)
        return;
    QList<QPointF> before(n);
    for (int i = 0; i < n; ++i) {
        Drop &d = run.drops[i];
        d.v.ry() += s.gravity * dt;
        before[i] = d.p;
        d.p += d.v * dt;
    }
    // Neighbours within reach, found by the grid cell each is in.
    const double reach = s.size * 3;
    QHash<qint64, QList<int>> cells;
    auto cellOf = [&](const QPointF &p) { return (qint64(std::floor(p.x() / reach)) << 32) ^ (qint64(std::floor(p.y() / reach)) & 0xffffffff); };
    for (int i = 0; i < n; ++i)
        cells[cellOf(run.drops.at(i).p)] << i;
    auto near = [&](int i, auto &&each) {
        const QPointF p = run.drops.at(i).p;
        const qint64 cx = qint64(std::floor(p.x() / reach)), cy = qint64(std::floor(p.y() / reach));
        for (qint64 y = cy - 1; y <= cy + 1; ++y)
            for (qint64 x = cx - 1; x <= cx + 1; ++x) {
                const auto it = cells.constFind((x << 32) ^ (y & 0xffffffff));
                if (it == cells.constEnd())
                    continue;
                for (const int j : *it)
                    if (j != i)
                        each(j);
            }
    };
    // Thickening: drops near each other move together a little.
    if (s.viscosity > 0)
        for (int i = 0; i < n; ++i)
            near(i, [&](int j) {
                if (j < i)
                    return;
                QPointF d = run.drops.at(j).p - run.drops.at(i).p;
                const double r = std::hypot(d.x(), d.y());
                if (r <= 0 || r >= reach)
                    return;
                d /= r;
                const double q = r / reach;
                const double closing = (run.drops.at(i).v.x() - run.drops.at(j).v.x()) * d.x() + (run.drops.at(i).v.y() - run.drops.at(j).v.y()) * d.y();
                if (closing <= 0)
                    return;
                const QPointF impulse = d * (dt * (1 - q) * s.viscosity * closing * .5);
                run.drops[i].v -= impulse;
                run.drops[j].v += impulse;
            });
    // Crowded drops pushed apart, sparse ones drawn together (double
    // density relaxation), so it spreads into a pool and holds as drops.
    const double rest = 4, stiff = .004 * reach * reach, nearStiff = .04 * reach * reach;
    for (int i = 0; i < n; ++i) {
        double density = 0, nearDensity = 0;
        near(i, [&](int j) {
            const QPointF d = run.drops.at(j).p - run.drops.at(i).p;
            const double q = std::hypot(d.x(), d.y()) / reach;
            if (q < 1) {
                density += (1 - q) * (1 - q);
                nearDensity += (1 - q) * (1 - q) * (1 - q);
            }
        });
        // Drawn together only gently, so a stream stays a stream.
        const double pressure = stiff * std::max(density - rest, (density - rest) * .15), nearPressure = nearStiff * nearDensity;
        QPointF moved;
        near(i, [&](int j) {
            QPointF d = run.drops.at(j).p - run.drops.at(i).p;
            const double r = std::hypot(d.x(), d.y());
            if (r <= 1e-6 || r >= reach)
                return;
            const double q = r / reach;
            // No more than a part of a drop's size a step, so it can't burst.
            const double amount = std::clamp(dt * dt * (pressure * (1 - q) + nearPressure * (1 - q) * (1 - q)) * 3600, -s.size * .1, s.size * .3);
            const QPointF push = d / r * amount;
            run.drops[j].p += push / 2;
            moved -= push / 2;
        });
        run.drops[i].p += moved;
    }
    // The floor and the canvas's sides hold it, a little slippery.
    for (int i = 0; i < n; ++i) {
        Drop &d = run.drops[i];
        if (d.p.y() > s.floor - s.size)
            d.p.ry() = s.floor - s.size;
        if (d.p.x() < s.size)
            d.p.rx() = s.size;
        if (d.p.x() > scene.width - s.size)
            d.p.rx() = scene.width - s.size;
        d.v = (d.p - before[i]) / dt;
    }
}

std::shared_ptr<Run> runTo(const Scene &scene, AssetCache &assets, const Layer &layer, int frame) {
    const Settings s = settingsOf(scene, layer.particles);
    const double fps = std::max(1.0, scene.fps);
    const QString key = layer.name + "|" + QString::fromUtf8(QJsonDocument(layer.particles).toJson(QJsonDocument::Compact)) + "|" +
                        QString::number(scene.width) + "x" + QString::number(scene.height) + "|" + QString::number(fps);
    QMutexLocker lock(&runsLock);
    std::shared_ptr<Run> &run = runs[key];
    if (run)
        for (int k = 0; k < run->sources.size() && k <= frame; ++k)
            if (QLineF(run->sources.at(k), sourceAt(scene, assets, s, k / fps)).length() > .01) {
                run.reset();
                break;
            }
    if (!run)
        run = std::make_shared<Run>();
    const double dt = 1 / fps / substeps;
    while (run->frames.size() <= frame) {
        const int k = int(run->frames.size());
        const QPointF source = sourceAt(scene, assets, s, k / fps);
        if (k > 0)
            for (int sub = 0; sub < substeps; ++sub)
                step(*run, s, scene, source, (k - 1) / fps + sub * dt, dt);
        run->sources << source;
        QList<QPointF> places;
        for (const Drop &d : run->drops)
            places << d.p;
        run->frames << places;
    }
    const std::shared_ptr<Run> kept = run;
    if (runs.size() > 32) {
        runs.clear();
        runs.insert(key, kept);
    }
    return kept;
}

QList<QPointF> dropsAt(const Scene &scene, AssetCache &assets, const Layer &layer, double t) {
    const int frame = std::clamp(int(std::floor(t * std::max(1.0, scene.fps) + 1e-6)), 0, 20000);
    return runTo(scene, assets, layer, frame)->frames.at(frame);
}

// The drops as water: each a soft blob into a mask, the mask made sharp
// where the blobs add up past a level (so drops close together run into
// one), then coloured, a little lighter at its top edges.
QImage draw(const Scene &scene, const Settings &s, const QList<QPointF> &places, double scale) {
    const QSize size(std::max(1, int(std::ceil(scene.width * scale))), std::max(1, int(std::ceil(scene.height * scale))));
    QImage mask(size, QImage::Format_ARGB32_Premultiplied);
    mask.fill(Qt::transparent);
    {
        QPainter p(&mask);
        p.setPen(Qt::NoPen);
        p.setCompositionMode(QPainter::CompositionMode_Plus);
        const double r = s.size * 2.6 * scale;
        for (const QPointF &place : places) {
            const QPointF at = place * scale;
            QRadialGradient blob(at, r);
            blob.setColorAt(0, QColor(255, 255, 255, 120));
            blob.setColorAt(1, QColor(255, 255, 255, 0));
            p.setBrush(blob);
            p.drawEllipse(at, r, r);
        }
    }
    QImage water(size, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < size.height(); ++y) {
        const QRgb *in = reinterpret_cast<const QRgb *>(mask.constScanLine(y));
        const QRgb *above = reinterpret_cast<const QRgb *>(mask.constScanLine(std::max(0, y - 2)));
        QRgb *out = reinterpret_cast<QRgb *>(water.scanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            const double level = qAlpha(in[x]) / 255.0;
            const double a = std::clamp((level - .28) / .12, 0.0, 1.0) * s.colour.alphaF() * .9;
            // Lighter where the surface is, from the level rising below.
            const double edge = std::clamp((level - qAlpha(above[x]) / 255.0) * 4, 0.0, 1.0);
            const double r = std::min(1.0, s.colour.redF() + edge * .5), g = std::min(1.0, s.colour.greenF() + edge * .5),
                         b = std::min(1.0, s.colour.blueF() + edge * .4);
            out[x] = qRgba(int(r * a * 255), int(g * a * 255), int(b * a * 255), int(a * 255));
        }
    }
    return water;
}
}

QList<QPointF> drops(const Scene &scene, AssetCache &assets, const Layer &layer, double t) { return dropsAt(scene, assets, layer, t); }

QImage at(const Scene &scene, AssetCache &assets, const Layer &layer, double t, double scale, QRectF *onCanvas) {
    const Settings s = settingsOf(scene, layer.particles);
    if (onCanvas)
        *onCanvas = QRectF(0, 0, scene.width, scene.height);
    // Looping with the scene, as the other particles do: the water a
    // scene's length on, fading into the water now, so its end is its start.
    const double length = scene.duration;
    const QImage now = draw(scene, s, dropsAt(scene, assets, layer, t), scale);
    if (!layer.particles.value("loop").toBool(true) || length <= 0)
        return now;
    const double into = std::clamp(t / length, 0.0, 1.0);
    const QImage later = draw(scene, s, dropsAt(scene, assets, layer, t + length), scale);
    if (into <= 0)
        return later;
    QImage mixed(now.size(), QImage::Format_ARGB32_Premultiplied);
    const int weight = int(std::lround(into * 256));
    for (int y = 0; y < now.height(); ++y) {
        const QRgb *a = reinterpret_cast<const QRgb *>(later.constScanLine(y)), *b = reinterpret_cast<const QRgb *>(now.constScanLine(y));
        QRgb *out = reinterpret_cast<QRgb *>(mixed.scanLine(y));
        for (int x = 0; x < now.width(); ++x) {
            auto mix = [&](int p, int q) { return (p * (256 - weight) + q * weight) >> 8; };
            out[x] = qRgba(mix(qRed(a[x]), qRed(b[x])), mix(qGreen(a[x]), qGreen(b[x])), mix(qBlue(a[x]), qBlue(b[x])), mix(qAlpha(a[x]), qAlpha(b[x])));
        }
    }
    return mixed;
}

}
