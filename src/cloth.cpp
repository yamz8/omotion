#include "cloth.h"

#include <QHash>
#include <QJsonDocument>
#include <QMutex>
#include <QtMath>
#include <cmath>

namespace Cloth {

namespace {

// Smooth noise from -1 to 1 along one line, the same for the same seed.
double wave(double x, quint32 seed) {
    auto at = [seed](qint64 i) {
        quint32 h = quint32(i) * 374761393u + seed * 668265263u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return ((h ^ (h >> 16)) & 0xffff) / 32767.5 - 1;
    };
    const double whole = std::floor(x), f = x - whole;
    const double s = f * f * (3 - 2 * f);
    return at(qint64(whole)) * (1 - s) + at(qint64(whole) + 1) * s;
}

struct Settings {
    QString pin = "left";
    double wind = 300, direction = 0, gust = .5, stiffness = .8, weight = .3;
    quint32 seed = 1;
};

Settings settingsOf(const QJsonObject &cloth) {
    Settings s;
    s.pin = cloth.value("pin").toString("left");
    s.wind = cloth.value("wind").toDouble(300);
    s.direction = qDegreesToRadians(cloth.value("direction").toDouble(0));
    s.gust = std::clamp(cloth.value("gust").toDouble(.5), 0.0, 1.0);
    s.stiffness = std::clamp(cloth.value("stiffness").toDouble(.8), 0.05, 1.0);
    s.weight = std::clamp(cloth.value("weight").toDouble(.3), 0.0, 3.0);
    s.seed = quint32(cloth.value("seed").toInt(1));
    return s;
}

// Which points hold still, by the pin.
bool pinned(const QString &pin, int x, int y, int columns, int rows) {
    const bool left = x == 0, right = x == columns - 1, top = y == 0, bottom = y == rows - 1;
    if (pin == "left")
        return left;
    if (pin == "right")
        return right;
    if (pin == "top")
        return top;
    if (pin == "bottom")
        return bottom;
    if (pin == "top corners")
        return top && (left || right);
    if (pin == "corners")
        return (top || bottom) && (left || right);
    return left;
}

// A run of the cloth from the start: its grid, and each frame's points.
struct Run {
    int columns = 0, rows = 0;
    QSizeF size;
    QList<QList<QVector3D>> frames;
    // Kept going between frames.
    QList<QVector3D> now, before;
};

QMutex runsLock;
QHash<QString, std::shared_ptr<Run>> runs;

constexpr int substeps = 6;

void step(Run &run, const Settings &s, double t, double h) {
    const int columns = run.columns, rows = run.rows;
    const double cw = run.size.width() / (columns - 1), ch = run.size.height() / (rows - 1);
    const QVector3D gravity(0, float(980 * s.weight), 0);
    const QVector3D windWay(float(std::cos(s.direction)), float(std::sin(s.direction)), 0);
    const double span = std::max(run.size.width(), run.size.height());
    // The air: gusts that travel with the wind, and flutter across it.
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < columns; ++x) {
            const int i = y * columns + x;
            if (pinned(s.pin, x, y, columns, rows))
                continue;
            const QVector3D rest(float(x * cw), float(y * ch), 0);
            const double along = QVector3D::dotProduct(rest, windWay) / span;
            const double drift = t * std::abs(s.wind) / span * 1.5;
            const double gusting = 1 + s.gust * wave(along * 2 - drift, s.seed);
            const double flutter = s.gust * wave(along * 4 - drift * 2.3 + y * .37, s.seed + 17);
            // The wind pushes on the cloth as much as it faces it: so a
            // flag flaps rather than stretching flat.
            QVector3D normal(0, 0, 1);
            if (x > 0 && x < columns - 1 && y > 0 && y < rows - 1) {
                const QVector3D across = run.now[i + 1] - run.now[i - 1], down = run.now[i + columns] - run.now[i - columns];
                normal = QVector3D::crossProduct(across, down).normalized();
            }
            const QVector3D air = windWay * float(s.wind * gusting);
            const QVector3D push = normal * float(QVector3D::dotProduct(air, normal) * .6) + air * .25f +
                                   QVector3D(0, 0, float(s.wind * flutter * .8));
            const QVector3D velocity = (run.now[i] - run.before[i]) * .985f;
            run.before[i] = run.now[i];
            // Pushes as accelerations: the air's on a weightless cloth is
            // its speed again each second, against its own lag.
            run.now[i] += velocity + (gravity + (push - velocity / float(h)) * 2.f) * float(h * h);
        }
    // Each pair of neighbours (and of next-but-one, for stiffness against
    // folding) kept their distance apart, a few times over.
    auto keep = [&](int a, int b, double length, double strength) {
        QVector3D d = run.now[b] - run.now[a];
        const float l = d.length();
        if (l < 1e-6f)
            return;
        const float off = float((l - length) / l * strength);
        const bool fa = pinned(s.pin, a % columns, a / columns, columns, rows), fb = pinned(s.pin, b % columns, b / columns, columns, rows);
        if (fa && fb)
            return;
        if (fa)
            run.now[b] -= d * off;
        else if (fb)
            run.now[a] += d * off;
        else {
            run.now[a] += d * (off / 2);
            run.now[b] -= d * (off / 2);
        }
    };
    const double diagonal = std::hypot(cw, ch);
    for (int pass = 0; pass < 8; ++pass)
        for (int y = 0; y < rows; ++y)
            for (int x = 0; x < columns; ++x) {
                const int i = y * columns + x;
                if (x + 1 < columns)
                    keep(i, i + 1, cw, 1);
                if (y + 1 < rows)
                    keep(i, i + columns, ch, 1);
                if (x + 1 < columns && y + 1 < rows) {
                    keep(i, i + columns + 1, diagonal, .5 * s.stiffness);
                    keep(i + 1, i + columns, diagonal, .5 * s.stiffness);
                }
                if (x + 2 < columns)
                    keep(i, i + 2, 2 * cw, .3 * s.stiffness);
                if (y + 2 < rows)
                    keep(i, i + 2 * columns, 2 * ch, .3 * s.stiffness);
            }
}

std::shared_ptr<Run> runFor(const Scene &scene, const Layer &layer, const QSizeF &size, int frame) {
    const QJsonObject cloth = layer.raw.value("cloth").toObject();
    const QString key = layer.name + "|" + QString::fromUtf8(QJsonDocument(cloth).toJson(QJsonDocument::Compact)) + "|" +
                        QString::number(size.width()) + "x" + QString::number(size.height()) + "|" + QString::number(scene.fps);
    QMutexLocker lock(&runsLock);
    std::shared_ptr<Run> &run = runs[key];
    if (!run) {
        run = std::make_shared<Run>();
        // About 16 points along its longer side.
        const double longest = std::max(size.width(), size.height());
        run->columns = std::clamp(int(std::lround(16 * size.width() / longest)), 4, 16);
        run->rows = std::clamp(int(std::lround(16 * size.height() / longest)), 4, 16);
        run->size = size;
        for (int y = 0; y < run->rows; ++y)
            for (int x = 0; x < run->columns; ++x)
                run->now << QVector3D(float(x * size.width() / (run->columns - 1)), float(y * size.height() / (run->rows - 1)), 0);
        run->before = run->now;
        // Settled a while first, in still air, so it hangs at the start
        // rather than dropping into place.
        Settings still = settingsOf(cloth);
        still.wind = 0;
        const double h = 1 / std::max(1.0, scene.fps) / substeps;
        for (int k = 0; k < substeps * int(std::max(1.0, scene.fps)); ++k)
            step(*run, still, 0, h);
        run->before = run->now;
        run->frames << run->now;
    }
    const Settings s = settingsOf(cloth);
    const double h = 1 / std::max(1.0, scene.fps) / substeps;
    while (run->frames.size() <= frame) {
        const double t = (run->frames.size() - 1) / std::max(1.0, scene.fps);
        for (int k = 0; k < substeps; ++k)
            step(*run, s, t + k * h, h);
        run->frames << run->now;
    }
    // Old runs (settings since changed) go, so they don't pile up.
    if (runs.size() > 64)
        runs.clear();
    return run;
}

}

State at(const Scene &scene, const Layer &layer, const QSizeF &size, double t) {
    State state;
    if (layer.raw.isEmpty() || !layer.raw.contains("cloth") || size.width() < 2 || size.height() < 2)
        return state;
    const double frame = std::max(0.0, t) * std::max(1.0, scene.fps);
    const int whole = int(std::floor(frame));
    const std::shared_ptr<Run> run = runFor(scene, layer, size, whole + 1);
    state.columns = run->columns;
    state.rows = run->rows;
    state.size = run->size;
    // Between frames, for motion blur's in-between times.
    const float f = float(frame - whole);
    const QList<QVector3D> &a = run->frames.at(whole), &b = run->frames.at(whole + 1);
    for (int i = 0; i < a.size(); ++i)
        state.points << a.at(i) * (1 - f) + b.at(i) * f;
    return state;
}

namespace {
// The grid cell a point of the picture falls in, and how far across it.
void cellOf(const State &state, const QPointF &point, int *x, int *y, double *fx, double *fy) {
    const double gx = std::clamp(point.x() / state.size.width() * (state.columns - 1), 0.0, state.columns - 1.0001);
    const double gy = std::clamp(point.y() / state.size.height() * (state.rows - 1), 0.0, state.rows - 1.0001);
    *x = int(gx);
    *y = int(gy);
    *fx = gx - *x;
    *fy = gy - *y;
}
}

QPointF bend(const State &state, const QPointF &point) {
    if (state.isNull())
        return point;
    int x, y;
    double fx, fy;
    cellOf(state, point, &x, &y, &fx, &fy);
    const int i = y * state.columns + x;
    const QVector3D p = (state.points.at(i) * float(1 - fx) + state.points.at(i + 1) * float(fx)) * float(1 - fy) +
                        (state.points.at(i + state.columns) * float(1 - fx) + state.points.at(i + state.columns + 1) * float(fx)) * float(fy);
    // Past its edge (a glow's halo), carried along with the nearest part.
    const QPointF rest(x * state.size.width() / (state.columns - 1) + fx * state.size.width() / (state.columns - 1),
                       y * state.size.height() / (state.rows - 1) + fy * state.size.height() / (state.rows - 1));
    return QPointF(p.x(), p.y()) + (point - rest);
}

double fold(const State &state, const QPointF &point) {
    if (state.isNull())
        return 0;
    // Each grid point's facing, from its neighbours either way, blended
    // between them so the shading runs smoothly over the folds.
    auto facing = [&](int x, int y) {
        const int columns = state.columns, rows = state.rows;
        const QVector3D across = state.points.at(y * columns + std::min(x + 1, columns - 1)) - state.points.at(y * columns + std::max(x - 1, 0));
        const QVector3D down = state.points.at(std::min(y + 1, rows - 1) * columns + x) - state.points.at(std::max(y - 1, 0) * columns + x);
        return std::abs(QVector3D::crossProduct(across, down).normalized().z());
    };
    int x, y;
    double fx, fy;
    cellOf(state, point, &x, &y, &fx, &fy);
    const double z = (facing(x, y) * (1 - fx) + facing(x + 1, y) * fx) * (1 - fy) + (facing(x, y + 1) * (1 - fx) + facing(x + 1, y + 1) * fx) * fy;
    return std::clamp(1 - z, 0.0, 1.0);
}

bool set(Scene &scene, int index, const QJsonObject &cloth, QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (index < 0 || index >= scene.layers.size())
        return fail("There's no such layer.");
    Layer &layer = scene.layers[index];
    if (!layer.particles.isEmpty() || layer.source.isEmpty())
        return fail(QString("%1 has no picture to be cloth.").arg(layer.name));
    if (cloth.isEmpty()) {
        layer.raw.remove("cloth");
        return true;
    }
    static const QStringList pins{"left", "right", "top", "bottom", "top corners", "corners"};
    if (cloth.contains("pin") && !pins.contains(cloth.value("pin").toString()))
        return fail("Cloth is pinned at left, right, top, bottom, top corners or corners.");
    for (auto it = cloth.begin(); it != cloth.end(); ++it)
        if (it.key() != "pin" && !it.value().isDouble())
            return fail(QString("A cloth's %1 is a number.").arg(it.key()));
    layer.raw["cloth"] = cloth;
    return true;
}

}
