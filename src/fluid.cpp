#include "fluid.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QRandomGenerator>
#include <cmath>
#include <memory>
#include <vector>

namespace Fluid {

bool isFluid(const QString &kind) { return kind == "smoke" || kind == "fire"; }

namespace {

struct Settings {
    bool fire = false;
    double size = 30, speed = 80, rate = 1, life = 3, wind = 0, swirl = .5;
    QPointF point;          // On the canvas, when it has no layer to come from.
    QString from;           // The layer it comes from, and the point of it.
    QPointF at;
    bool atSet = false;
    QColor colour;
    quint32 seed = 1;
};

Settings settingsOf(const Scene &scene, const QJsonObject &p) {
    Settings s;
    s.fire = p.value("kind").toString() == "fire";
    s.size = std::max(2.0, p.value("size").toDouble(s.fire ? 24 : 30));
    s.speed = p.value("speed").toDouble(s.fire ? 140 : 80);
    s.rate = std::max(0.0, p.value("rate").toDouble(1));
    s.life = std::max(.1, p.value("life").toDouble(s.fire ? 1.2 : 3));
    s.wind = p.value("wind").toDouble(0);
    s.swirl = std::clamp(p.value("swirl").toDouble(.5), 0.0, 2.0);
    s.point = QPointF(p.value("x").toDouble(scene.width / 2.0), p.value("y").toDouble(scene.height * .85));
    s.from = p.value("from").toString();
    const QJsonArray at = p.value("at").toArray();
    s.atSet = at.size() == 2;
    if (s.atSet)
        s.at = QPointF(at.at(0).toDouble(), at.at(1).toDouble());
    s.colour = QColor(p.value("color").toString(s.fire ? "#ffa040" : "#d8d8e0"));
    s.seed = quint32(p.value("seed").toInt(1));
    return s;
}

// The air on its grid: velocity in cells a second, smoke, heat.
struct Field {
    int w = 0, h = 0;
    double cell = 1;
    std::vector<float> u, v, d, heat;
    int at(int x, int y) const { return std::clamp(y, 0, h - 1) * w + std::clamp(x, 0, w - 1); }
    float sample(const std::vector<float> &f, double x, double y) const {
        x = std::clamp(x, 0.0, w - 1.001);
        y = std::clamp(y, 0.0, h - 1.001);
        const int ix = int(x), iy = int(y);
        const float fx = float(x - ix), fy = float(y - iy);
        const float *row = &f[size_t(iy) * w + ix], *below = row + w;
        return (row[0] * (1 - fx) + row[1] * fx) * (1 - fy) + (below[0] * (1 - fx) + below[1] * fx) * fy;
    }
    // Carried along by the air: each cell takes what was where the air
    // there came from, a step ago.
    std::vector<float> carried(const std::vector<float> &f, double dt) const {
        std::vector<float> out(f.size());
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const int i = y * w + x;
                out[i] = sample(f, x - u[i] * dt, y - v[i] * dt);
            }
        return out;
    }
    // Air neither piles up nor thins out: the part of its flow that would
    // is taken away (a pressure worked out by relaxation).
    void project() {
        std::vector<float> spread(size_t(w) * h), pressure(size_t(w) * h, 0.f);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                spread[size_t(y) * w + x] = -.5f * (u[at(x + 1, y)] - u[at(x - 1, y)] + v[at(x, y + 1)] - v[at(x, y - 1)]);
        std::vector<float> next(pressure.size());
        for (int pass = 0; pass < 24; ++pass) {
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    next[size_t(y) * w + x] = (spread[size_t(y) * w + x] + pressure[at(x + 1, y)] + pressure[at(x - 1, y)] +
                                               pressure[at(x, y + 1)] + pressure[at(x, y - 1)]) / 4;
            std::swap(pressure, next);
        }
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const int i = y * w + x;
                u[i] -= .5f * (pressure[at(x + 1, y)] - pressure[at(x - 1, y)]);
                v[i] -= .5f * (pressure[at(x, y + 1)] - pressure[at(x, y - 1)]);
            }
    }
};

void step(Field &f, const Settings &s, const QPointF &source, double t, double dt) {
    const int n = f.w * f.h;
    // The source, wandering a little so it isn't a still column.
    QRandomGenerator random(s.seed * 2654435761u + quint32(t * 1000));
    const double radius = std::max(1.5, s.size / f.cell);
    const double sx = source.x() / f.cell + (random.generateDouble() - .5) * radius * .6;
    const double sy = source.y() / f.cell + (random.generateDouble() - .5) * radius * .3;
    const double rise = s.speed / f.cell;
    const int reach = int(std::ceil(radius * 2));
    for (int y = int(sy) - reach; y <= int(sy) + reach; ++y)
        for (int x = int(sx) - reach; x <= int(sx) + reach; ++x) {
            if (x < 0 || y < 0 || x >= f.w || y >= f.h)
                continue;
            const double r2 = ((x - sx) * (x - sx) + (y - sy) * (y - sy)) / (radius * radius);
            const float weight = float(std::exp(-r2 * 2));
            const int i = y * f.w + x;
            f.d[i] += float(s.rate * weight * dt * 3);
            if (s.fire)
                f.heat[i] += float(s.rate * weight * dt * 5);
            f.v[i] -= float(rise * weight * dt * 4);
            // Puffs: the air at the source kicked about, so it breaks up.
            f.u[i] += float((random.generateDouble() - .5) * rise * weight * dt * 30 * s.swirl);
        }
    // Smoke rises (warm air, and heat more); the wind takes it along.
    const double lift = rise * 1.5;
    for (int i = 0; i < n; ++i) {
        f.v[i] -= float(lift * (f.d[i] * .4 + f.heat[i]) * dt);
        f.u[i] += float((s.wind / f.cell - f.u[i]) * std::min(1.0, double(f.d[i] + f.heat[i])) * dt * .8);
    }
    // Swirls kept going (vorticity confinement): pushed round the way the
    // air already turns, so it curls rather than smearing out.
    if (s.swirl > 0) {
        std::vector<float> turn(n);
        for (int y = 0; y < f.h; ++y)
            for (int x = 0; x < f.w; ++x)
                turn[size_t(y) * f.w + x] = .5f * (f.v[f.at(x + 1, y)] - f.v[f.at(x - 1, y)] - f.u[f.at(x, y + 1)] + f.u[f.at(x, y - 1)]);
        for (int y = 1; y < f.h - 1; ++y)
            for (int x = 1; x < f.w - 1; ++x) {
                const int i = y * f.w + x;
                const float gx = .5f * (std::abs(turn[i + 1]) - std::abs(turn[i - 1])), gy = .5f * (std::abs(turn[i + f.w]) - std::abs(turn[i - f.w]));
                const float length = std::hypot(gx, gy) + 1e-5f;
                f.u[i] += float(s.swirl * 10 * (gy / length) * turn[i] * dt);
                f.v[i] -= float(s.swirl * 10 * (gx / length) * turn[i] * dt);
            }
    }
    f.u = f.carried(f.u, dt);
    f.v = f.carried(f.v, dt);
    f.project();
    f.d = f.carried(f.d, dt);
    f.heat = f.carried(f.heat, dt);
    // Flames cool quickly into smoke: short tongues under a long plume.
    const float fade = float(std::exp(-dt / s.life)), cool = float(std::exp(-dt * 6 / s.life));
    for (int i = 0; i < n; ++i) {
        f.d[i] *= fade;
        f.heat[i] *= cool;
    }
}

// A run from the start: each frame's smoke and heat, kept small, and where
// its source was, to tell when its source has moved since.
struct Run {
    Field field;
    QList<QPointF> sources;
    QList<std::vector<quint8>> smoke, heat;
};

QMutex runsLock;
QHash<QString, std::shared_ptr<Run>> runs;
constexpr int substeps = 2;

QPointF sourceAt(const Scene &scene, AssetCache &assets, const Settings &s, double t) {
    const int layer = s.from.isEmpty() ? -1 : scene.indexOf(s.from);
    if (layer < 0)
        return s.point;
    const Layer &from = scene.layers.at(layer);
    const QSizeF size = assets.size(from.source);
    const QPointF point = s.atSet ? s.at : QPointF(from.anchor.x() * size.width(), from.anchor.y() * size.height());
    return layerTransform(scene, assets, layer, t).map(point);
}

quint8 pack(float v) { return quint8(std::clamp(v * 128.f, 0.f, 255.f)); }
}

namespace {
QImage frameAt(const Scene &scene, AssetCache &assets, const Layer &layer, double t, QRectF *onCanvas) {
    const Settings s = settingsOf(scene, layer.particles);
    const double fps = std::max(1.0, scene.fps);
    const int frame = std::max(0, int(std::floor(t * fps + 1e-6)));
    const QString key = layer.name + "|" + QString::fromUtf8(QJsonDocument(layer.particles).toJson(QJsonDocument::Compact)) + "|" +
                        QString::number(scene.width) + "x" + QString::number(scene.height) + "|" + QString::number(fps);
    QMutexLocker lock(&runsLock);
    std::shared_ptr<Run> &run = runs[key];
    // Its source moved differently since: worked out again from there on.
    if (run)
        for (int k = 0; k < run->sources.size() && k <= frame; ++k)
            if (QLineF(run->sources.at(k), sourceAt(scene, assets, s, k / fps)).length() > .01) {
                run.reset();
                break;
            }
    if (!run) {
        run = std::make_shared<Run>();
        Field &f = run->field;
        f.cell = std::max(scene.width, scene.height) / 160.0;
        f.w = int(std::ceil(scene.width / f.cell));
        f.h = int(std::ceil(scene.height / f.cell));
        const size_t n = size_t(f.w) * f.h;
        f.u.assign(n, 0);
        f.v.assign(n, 0);
        f.d.assign(n, 0);
        f.heat.assign(n, 0);
    }
    Field &f = run->field;
    const double dt = 1 / fps / substeps;
    // Not worked out further than a few minutes in.
    if (frame > 20000)
        return {};
    while (run->smoke.size() <= frame) {
        const int k = int(run->smoke.size());
        const QPointF source = sourceAt(scene, assets, s, k / fps);
        if (k > 0)
            for (int sub = 0; sub < substeps; ++sub)
                step(f, s, source, (k - 1) / fps + sub * dt, dt);
        run->sources << source;
        std::vector<quint8> smoke(f.d.size()), heat(f.heat.size());
        for (size_t i = 0; i < f.d.size(); ++i) {
            smoke[i] = pack(f.d[i]);
            heat[i] = pack(f.heat[i]);
        }
        run->smoke << smoke;
        run->heat << heat;
    }
    // Old runs (settings since changed) go, so they don't pile up.
    const std::shared_ptr<Run> kept = run;
    if (runs.size() > 32) {
        runs.clear();
        runs.insert(key, kept);
    }
    const std::vector<quint8> &smoke = kept->smoke.at(frame), &heat = kept->heat.at(frame);
    // As a picture: smoke in its colour, thicker more opaque; fire from
    // its colour through yellow to white as it's hotter, over dark smoke.
    QImage image(f.w, f.h, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < f.h; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < f.w; ++x) {
            const int i = y * f.w + x;
            const double density = smoke[i] / 128.0, hot = heat[i] / 128.0;
            double r, g, b, a;
            if (s.fire) {
                const double glow = 1 - std::exp(-hot * 2.2);
                const double white = std::clamp(hot - .8, 0.0, 1.0);
                r = std::min(1.0, s.colour.redF() * 1.2 + white);
                g = std::min(1.0, s.colour.greenF() * (.5 + glow) + white);
                b = std::min(1.0, s.colour.blueF() * .6 + white * .8);
                const double soot = (1 - std::exp(-density * .8)) * .35 * (1 - glow);
                a = std::clamp(glow + soot, 0.0, 1.0);
                // Soot is dark, flame bright.
                const double bright = a > 0 ? glow / a : 0;
                r *= bright, g *= bright, b *= bright;
            } else {
                a = 1 - std::exp(-density * 1.2);
                r = s.colour.redF(), g = s.colour.greenF(), b = s.colour.blueF();
            }
            a *= s.colour.alphaF();
            line[x] = qRgba(int(r * a * 255), int(g * a * 255), int(b * a * 255), int(a * 255));
        }
    }
    if (onCanvas)
        *onCanvas = QRectF(0, 0, f.w * f.cell, f.h * f.cell);
    return image;
}
}

QImage at(const Scene &scene, AssetCache &assets, const Layer &layer, double t, QRectF *onCanvas) {
    // Looping with the scene, as the other particles do: the smoke a
    // scene's length on, fading into the smoke now as the scene goes, so
    // its end is its start. It starts already going, too.
    const double length = scene.duration;
    if (!layer.particles.value("loop").toBool(true) || length <= 0)
        return frameAt(scene, assets, layer, t, onCanvas);
    const double into = std::clamp(t / length, 0.0, 1.0);
    const QImage later = frameAt(scene, assets, layer, t + length, onCanvas);
    if (into <= 0)
        return later;
    const QImage now = frameAt(scene, assets, layer, t, onCanvas);
    if (later.isNull() || now.size() != later.size())
        return now;
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
