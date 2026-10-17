#include "fluid.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QRandomGenerator>
#include <cmath>
#include <initializer_list>
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
    // Carried along by the air: each cell takes what was where the air
    // there came from, a step ago. Several fields carried by the same air
    // go together, so where it came from is worked out once for them all.
    void carry(std::initializer_list<std::vector<float> *> fields, double dt) const {
        std::vector<std::vector<float>> out;
        for (const std::vector<float> *f : fields)
            out.emplace_back(f->size());
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const int i = y * w + x;
                const double fromX = std::clamp(x - u[i] * dt, 0.0, w - 1.001), fromY = std::clamp(y - v[i] * dt, 0.0, h - 1.001);
                const int ix = int(fromX), iy = int(fromY);
                const float fx = float(fromX - ix), fy = float(fromY - iy);
                const size_t at = size_t(iy) * w + ix;
                size_t k = 0;
                for (const std::vector<float> *f : fields) {
                    const float *row = f->data() + at, *below = row + w;
                    out[k++][size_t(i)] = (row[0] * (1 - fx) + row[1] * fx) * (1 - fy) + (below[0] * (1 - fx) + below[1] * fx) * fy;
                }
            }
        size_t k = 0;
        for (std::vector<float> *f : fields)
            f->swap(out[k++]);
    }
    // Air neither piles up nor thins out: the part of its flow that would
    // is taken away (a pressure worked out by relaxation).
    // Runs each(x, y, here, left, right, up, down) for every cell, with the
    // indices of it and its neighbours (the edge's own where there's
    // none, so air doesn't leave the grid). Only the first and last columns need
    // working out; the rest are the next ones along.
    template <typename Each>
    void cells(Each &&each) const {
        for (int y = 0; y < h; ++y) {
            const int row = y * w, up = std::max(y - 1, 0) * w, down = std::min(y + 1, h - 1) * w;
            auto one = [&](int x) { each(x, y, row + x, row + std::max(x - 1, 0), row + std::min(x + 1, w - 1), up + x, down + x); };
            one(0);
            for (int x = 1; x < w - 1; ++x)
                each(x, y, row + x, row + x - 1, row + x + 1, up + x, down + x);
            if (w > 1)
                one(w - 1);
        }
    }
    void project() {
        std::vector<float> spread(size_t(w) * h), pressure(size_t(w) * h, 0.f);
        cells([&](int, int, int i, int left, int right, int up, int down) { spread[i] = -.5f * (u[right] - u[left] + v[down] - v[up]); });
        std::vector<float> next(pressure.size());
        for (int pass = 0; pass < 24; ++pass) {
            const float *p = pressure.data();
            float *out = next.data();
            const float *from = spread.data();
            cells([&](int, int, int i, int left, int right, int up, int down) { out[i] = (from[i] + p[right] + p[left] + p[down] + p[up]) / 4; });
            std::swap(pressure, next);
        }
        cells([&](int, int, int i, int left, int right, int up, int down) {
            u[i] -= .5f * (pressure[right] - pressure[left]);
            v[i] -= .5f * (pressure[down] - pressure[up]);
        });
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
        f.cells([&](int, int, int i, int left, int right, int up, int down) { turn[i] = .5f * (f.v[right] - f.v[left] - f.u[down] + f.u[up]); });
        for (int y = 1; y < f.h - 1; ++y)
            for (int x = 1; x < f.w - 1; ++x) {
                const int i = y * f.w + x;
                const float gx = .5f * (std::abs(turn[i + 1]) - std::abs(turn[i - 1])), gy = .5f * (std::abs(turn[i + f.w]) - std::abs(turn[i - f.w]));
                const float length = std::hypot(gx, gy) + 1e-5f;
                f.u[i] += float(s.swirl * 10 * (gy / length) * turn[i] * dt);
                f.v[i] -= float(s.swirl * 10 * (gx / length) * turn[i] * dt);
            }
    }
    // The air carries itself: all of its flow by the flow as it was.
    f.carry({&f.u, &f.v}, dt);
    f.project();
    // Only fire has heat: smoke's stays nothing.
    if (s.fire)
        f.carry({&f.d, &f.heat}, dt);
    else
        f.carry({&f.d}, dt);
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

namespace {
// The air in three dimensions: velocity in cells a second, smoke, heat.
struct Field3 {
    int w = 0, h = 0, d = 0;
    double cell = 1;
    std::vector<float> u, v, z, smoke, heat;
    // As the flat field's carry().
    void carry(std::initializer_list<std::vector<float> *> fields, double dt) const {
        std::vector<std::vector<float>> out;
        for (const std::vector<float> *f : fields)
            out.emplace_back(f->size());
        const size_t plane = size_t(w) * h;
        for (int k = 0; k < d; ++k)
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const size_t i = (size_t(k) * h + y) * w + x;
                    const double fromX = std::clamp(x - u[i] * dt, 0.0, w - 1.001), fromY = std::clamp(y - v[i] * dt, 0.0, h - 1.001),
                                 fromK = std::clamp(k - z[i] * dt, 0.0, d - 1.001);
                    const int ix = int(fromX), iy = int(fromY), ik = int(fromK);
                    const float fx = float(fromX - ix), fy = float(fromY - iy), fk = float(fromK - ik);
                    const size_t at = (size_t(ik) * h + iy) * w + ix;
                    size_t n = 0;
                    for (const std::vector<float> *f : fields) {
                        auto flat = [&](const float *row) {
                            const float *below = row + w;
                            return (row[0] * (1 - fx) + row[1] * fx) * (1 - fy) + (below[0] * (1 - fx) + below[1] * fx) * fy;
                        };
                        out[n++][i] = flat(f->data() + at) * (1 - fk) + flat(f->data() + at + plane) * fk;
                    }
                }
        size_t n = 0;
        for (std::vector<float> *f : fields)
            f->swap(out[n++]);
    }
    // Each cell and its six neighbours' indices (the edge's own where
    // there's none), as the flat field's cells().
    struct Around {
        size_t i, left, right, below, above, back, front;
    };
    template <typename Each>
    void cells(Each &&each) const {
        for (int k = 0; k < d; ++k)
            for (int y = 0; y < h; ++y) {
                const size_t row = (size_t(k) * h + y) * w, below = (size_t(k) * h + std::max(y - 1, 0)) * w,
                             above = (size_t(k) * h + std::min(y + 1, h - 1)) * w, back = (size_t(std::max(k - 1, 0)) * h + y) * w,
                             front = (size_t(std::min(k + 1, d - 1)) * h + y) * w;
                auto one = [&](int x) {
                    each(Around{row + x, row + std::max(x - 1, 0), row + std::min(x + 1, w - 1), below + x, above + x, back + x, front + x});
                };
                one(0);
                for (int x = 1; x < w - 1; ++x)
                    each(Around{row + x, row + x - 1, row + x + 1, below + x, above + x, back + x, front + x});
                if (w > 1)
                    one(w - 1);
            }
    }
    void project() {
        const size_t n = size_t(w) * h * d;
        std::vector<float> spread(n), pressure(n, 0.f), next(n);
        cells([&](const Around &a) {
            spread[a.i] = -.5f * (u[a.right] - u[a.left] + v[a.above] - v[a.below] + z[a.front] - z[a.back]);
        });
        for (int pass = 0; pass < 16; ++pass) {
            const float *p = pressure.data(), *from = spread.data();
            float *out = next.data();
            cells([&](const Around &a) {
                out[a.i] = (from[a.i] + p[a.right] + p[a.left] + p[a.above] + p[a.below] + p[a.front] + p[a.back]) / 6;
            });
            std::swap(pressure, next);
        }
        cells([&](const Around &a) {
            u[a.i] -= .5f * (pressure[a.right] - pressure[a.left]);
            v[a.i] -= .5f * (pressure[a.above] - pressure[a.below]);
            z[a.i] -= .5f * (pressure[a.front] - pressure[a.back]);
        });
    }
};

struct Run3 {
    Field3 field;
    QList<QVector3D> sources;
    QList<std::vector<quint8>> smoke, heat;
};

QMutex runs3Lock;
QHash<QString, std::shared_ptr<Run3>> runs3;

// A step of the air in the box: y here runs up, as in the world.
void step3(Field3 &f, const Settings &s, const QVector3D &source, double t, double dt) {
    const size_t n = f.smoke.size();
    QRandomGenerator random(s.seed * 2654435761u + quint32(t * 1000));
    const double radius = std::max(1.2, s.size / f.cell);
    const QVector3D at = source / float(f.cell) + QVector3D(float((random.generateDouble() - .5) * radius * .6), 0,
                                                            float((random.generateDouble() - .5) * radius * .6));
    const double rise = s.speed / f.cell;
    const int reach = int(std::ceil(radius * 2));
    for (int k = int(at.z()) - reach; k <= int(at.z()) + reach; ++k)
        for (int y = int(at.y()) - reach; y <= int(at.y()) + reach; ++y)
            for (int x = int(at.x()) - reach; x <= int(at.x()) + reach; ++x) {
                if (x < 0 || y < 0 || k < 0 || x >= f.w || y >= f.h || k >= f.d)
                    continue;
                const double r2 = (QVector3D(float(x), float(y), float(k)) - at).lengthSquared() / (radius * radius);
                const float weight = float(std::exp(-r2 * 2));
                const size_t i = (size_t(k) * f.h + y) * f.w + x;
                f.smoke[i] += float(s.rate * weight * dt * 3);
                if (s.fire)
                    f.heat[i] += float(s.rate * weight * dt * 5);
                f.v[i] += float(rise * weight * dt * 4);
                f.u[i] += float((random.generateDouble() - .5) * rise * weight * dt * 30 * s.swirl);
                f.z[i] += float((random.generateDouble() - .5) * rise * weight * dt * 30 * s.swirl);
            }
    const double lift = rise * 1.5;
    for (size_t i = 0; i < n; ++i) {
        f.v[i] += float(lift * (f.smoke[i] * .4 + f.heat[i]) * dt);
        f.u[i] += float((s.wind / f.cell - f.u[i]) * std::min(1.0, double(f.smoke[i] + f.heat[i])) * dt * .8);
    }
    // Swirls kept going: each cell pushed round the way the air about it
    // turns (vorticity confinement, in three dimensions).
    if (s.swirl > 0) {
        std::vector<QVector3D> curl(n);
        std::vector<float> strength(n);
        f.cells([&](const Field3::Around &a) {
            const QVector3D c(.5f * (f.z[a.above] - f.z[a.below] - f.v[a.front] + f.v[a.back]),
                              .5f * (f.u[a.front] - f.u[a.back] - f.z[a.right] + f.z[a.left]),
                              .5f * (f.v[a.right] - f.v[a.left] - f.u[a.above] + f.u[a.below]));
            curl[a.i] = c;
            strength[a.i] = c.length();
        });
        for (int k = 1; k < f.d - 1; ++k)
            for (int y = 1; y < f.h - 1; ++y)
                for (int x = 1; x < f.w - 1; ++x) {
                    const size_t i = (size_t(k) * f.h + y) * f.w + x;
                    // Inside, so its neighbours are simply the next cells along.
                    const size_t across = size_t(f.w), deep = size_t(f.w) * f.h;
                    QVector3D g(.5f * (strength[i + 1] - strength[i - 1]), .5f * (strength[i + across] - strength[i - across]),
                                .5f * (strength[i + deep] - strength[i - deep]));
                    const float length = g.length() + 1e-5f;
                    const QVector3D push = QVector3D::crossProduct(g / length, curl[i]) * float(s.swirl * 10 * dt);
                    f.u[i] += push.x(), f.v[i] += push.y(), f.z[i] += push.z();
                }
    }
    f.carry({&f.u, &f.v, &f.z}, dt);
    f.project();
    if (s.fire)
        f.carry({&f.smoke, &f.heat}, dt);
    else
        f.carry({&f.smoke}, dt);
    const float fade = float(std::exp(-dt / s.life)), cool = float(std::exp(-dt * 6 / s.life));
    for (size_t i = 0; i < n; ++i)
        f.smoke[i] *= fade, f.heat[i] *= cool;
}

// One frame's smoke and heat, from the start.
std::pair<const std::vector<quint8> *, const std::vector<quint8> *> volumeFrame(std::shared_ptr<Run3> &kept, const QString &key,
                                                                                 const Settings &s, const QVector3D &size, double fps,
                                                                                 const std::function<QVector3D(int)> &sourceAt, int frame) {
    QMutexLocker lock(&runs3Lock);
    std::shared_ptr<Run3> &run = runs3[key];
    if (run)
        for (int k = 0; k < run->sources.size() && k <= frame; ++k)
            if ((run->sources.at(k) - sourceAt(k)).length() > .01f) {
                run.reset();
                break;
            }
    if (!run) {
        run = std::make_shared<Run3>();
        Field3 &f = run->field;
        f.cell = std::max({size.x(), size.y(), size.z()}) / 40.0;
        f.w = std::max(4, int(std::ceil(size.x() / f.cell)));
        f.h = std::max(4, int(std::ceil(size.y() / f.cell)));
        f.d = std::max(4, int(std::ceil(size.z() / f.cell)));
        const size_t n = size_t(f.w) * f.h * f.d;
        f.u.assign(n, 0), f.v.assign(n, 0), f.z.assign(n, 0), f.smoke.assign(n, 0), f.heat.assign(n, 0);
    }
    Field3 &f = run->field;
    const double dt = 1 / fps / substeps;
    while (run->smoke.size() <= frame) {
        const int k = int(run->smoke.size());
        const QVector3D source = sourceAt(k);
        if (k > 0)
            for (int sub = 0; sub < substeps; ++sub)
                step3(f, s, source, (k - 1) / fps + sub * dt, dt);
        run->sources << source;
        std::vector<quint8> smoke(f.smoke.size()), heat(f.heat.size());
        for (size_t i = 0; i < f.smoke.size(); ++i)
            smoke[i] = pack(f.smoke[i]), heat[i] = pack(f.heat[i]);
        run->smoke << smoke;
        run->heat << heat;
    }
    kept = run;
    if (runs3.size() > 16) {
        runs3.clear();
        runs3.insert(key, kept);
    }
    return {&kept->smoke.at(frame), &kept->heat.at(frame)};
}

// A slab of cells across the axis, summed through its thickness, as a
// picture: smoke in its colour, fire hotter toward white.
QImage slab(const Field3 &f, const Settings &s, const std::vector<quint8> &smoke, const std::vector<quint8> &heat, int axis, int from, int to) {
    const int across = axis == 0 ? f.d : f.w;
    QImage image(across, f.h, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < f.h; ++y) {
        // Pictures run down; the box runs up.
        QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(f.h - 1 - y));
        for (int a = 0; a < across; ++a) {
            double density = 0, hot = 0;
            for (int c = from; c < to; ++c) {
                const size_t i = axis == 0 ? (size_t(a) * f.h + y) * f.w + c : (size_t(c) * f.h + y) * f.w + a;
                density += smoke[i] / 128.0, hot += heat[i] / 128.0;
            }
            double r, g, b, alpha;
            if (s.fire) {
                const double glow = 1 - std::exp(-hot * 3);
                const double white = std::clamp(hot / (to - from) - .8, 0.0, 1.0);
                r = std::min(1.0, s.colour.redF() * 1.2 + white);
                g = std::min(1.0, s.colour.greenF() * (.5 + glow) + white);
                b = std::min(1.0, s.colour.blueF() * .6 + white * .8);
                const double soot = (1 - std::exp(-density * .5)) * .35 * (1 - glow);
                alpha = std::clamp(glow + soot, 0.0, 1.0);
                const double bright = alpha > 0 ? glow / alpha : 0;
                r *= bright, g *= bright, b *= bright;
            } else {
                alpha = 1 - std::exp(-density * 2.2);
                r = s.colour.redF(), g = s.colour.greenF(), b = s.colour.blueF();
            }
            alpha *= s.colour.alphaF();
            line[a] = qRgba(int(r * alpha * 255), int(g * alpha * 255), int(b * alpha * 255), int(alpha * 255));
        }
    }
    return image;
}
}

Box volume(const QString &key, const QJsonObject &settings, const QVector3D &size, double fps, double duration,
           const std::function<QVector3D(int frame)> &sourceAt, double t, int axis) {
    Scene stand;
    stand.width = int(size.x());
    stand.height = int(size.y());
    const Settings s = settingsOf(stand, settings);
    fps = std::max(1.0, fps);
    const QString full = key + "|" + QString::fromUtf8(QJsonDocument(settings).toJson(QJsonDocument::Compact)) + "|" +
                         QString::number(size.x()) + "," + QString::number(size.y()) + "," + QString::number(size.z()) + "|" + QString::number(fps);
    auto slicesAt = [&](double when) {
        const int frame = std::clamp(int(std::floor(when * fps + 1e-6)), 0, 20000);
        std::shared_ptr<Run3> kept;
        const auto [smoke, heat] = volumeFrame(kept, full, s, size, fps, sourceAt, frame);
        const Field3 &f = kept->field;
        QList<QImage> slices;
        const int depth = axis == 0 ? f.w : f.d;
        for (int c = 0; c < depth; c += 2)
            slices << slab(f, s, *smoke, *heat, axis, c, std::min(depth, c + 2));
        return slices;
    };
    Box box;
    box.size = size;
    const bool loops = settings.value("loop").toBool(true) && duration > 0;
    const QList<QImage> now = slicesAt(t);
    if (!loops) {
        box.slices = now;
        return box;
    }
    // Looping with the scene, as on the canvas: the smoke a scene's length
    // on fading into the smoke now.
    const double into = std::clamp(t / duration, 0.0, 1.0);
    const QList<QImage> later = slicesAt(t + duration);
    const int weight = int(std::lround(into * 256));
    for (int i = 0; i < now.size() && i < later.size(); ++i) {
        QImage mixed(now.at(i).size(), QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < mixed.height(); ++y) {
            const QRgb *a = reinterpret_cast<const QRgb *>(later.at(i).constScanLine(y)), *b = reinterpret_cast<const QRgb *>(now.at(i).constScanLine(y));
            QRgb *out = reinterpret_cast<QRgb *>(mixed.scanLine(y));
            for (int x = 0; x < mixed.width(); ++x) {
                auto mix = [&](int p, int q) { return (p * (256 - weight) + q * weight) >> 8; };
                out[x] = qRgba(mix(qRed(a[x]), qRed(b[x])), mix(qGreen(a[x]), qGreen(b[x])), mix(qBlue(a[x]), qBlue(b[x])), mix(qAlpha(a[x]), qAlpha(b[x])));
            }
        }
        box.slices << mixed;
    }
    return box;
}

}
