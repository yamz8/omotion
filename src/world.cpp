#include "world.h"
#include "fluid.h"
#include "liquid.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <functional>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineF>
#include <QMutex>
#include <QRecursiveMutex>
#include <QPainter>
#include <QSaveFile>
#include <QImageWriter>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>
#include <cstring>

#include "ai.h"

namespace World {

namespace {
// The caches below are shared by the window and work done in the
// background; one lock keeps them whole.
QRecursiveMutex cacheLock;
}

quint16 encodeDepth(double meters) {
    if (!(meters < Far))
        return Sky;
    const double v = std::log(std::max(meters, Near) / Near) / std::log(Far / Near);
    return quint16(std::clamp(std::lround(v * (Sky - 1)), 0L, long(Sky - 1)));
}

double decodeDepth(quint16 code) {
    if (code >= Sky)
        return Far;
    return Near * std::pow(Far / Near, code / double(Sky - 1));
}

Info info(const Scene &scene) {
    Info info;
    const QJsonObject world = scene.raw.value("world").toObject();
    info.picture = world.value("picture").toString();
    info.depth = world.value("depth").toString();
    info.behind = world.value("behind").toString();
    info.focal = world.value("focal").toDouble();
    const QJsonObject ground = world.value("ground").toObject();
    const QJsonArray up = ground.value("up").toArray();
    if (up.size() == 3) {
        const QVector3D v(up.at(0).toDouble(), up.at(1).toDouble(), up.at(2).toDouble());
        if (v.length() > 1e-6)
            info.up = v.normalized();
    }
    info.height = ground.value("height").toDouble(info.height);
    info.scale = world.value("scale").toDouble(1);
    const QJsonObject haze = world.value("haze").toObject();
    if (QColor(haze.value("color").toString()).isValid())
        info.haze = QColor(haze.value("color").toString());
    info.hazeDensity = std::max(0.0, haze.value("density").toDouble(info.hazeDensity));
    if (!(info.scale > 0))
        info.scale = 1;
    info.wide = world.value("wide").toString();
    const QJsonObject far = world.value("far").toObject();
    info.farDepth = far.value("depth").toString();
    info.farPicture = far.value("picture").toString();
    const QJsonArray margin = world.value("margin").toArray();
    info.margin = QSize(margin.at(0).toInt(), margin.at(1).toInt());
    info.valid = scene.indexOf(info.picture) >= 0 && !info.depth.isEmpty() && !info.behind.isEmpty() && info.focal > 0;
    return info;
}

int rootOf(const Scene &scene, int index) {
    QSet<int> seen;
    while (index >= 0 && index < scene.layers.size() && !seen.contains(index)) {
        seen.insert(index);
        const QString from = scene.layers.at(index).raw.value("cut").toObject().value("from").toString();
        const int parent = from.isEmpty() ? -1 : scene.indexOf(from);
        if (parent < 0)
            return index;
        index = parent;
    }
    return index;
}

QList<int> layers(const Scene &scene) {
    const int root = scene.indexOf(info(scene).picture);
    QList<int> list;
    if (root < 0)
        return list;
    for (int i = 0; i < scene.layers.size(); ++i)
        if (scene.layers.at(i).particles.isEmpty() && !scene.layers.at(i).source.isEmpty() && rootOf(scene, i) == root)
            list.append(i);
    return list;
}

QPointF offsetInPicture(const Scene &scene, int index) {
    // A piece's image is the rectangle of its source's image at its cut's
    // origin; a piece of a piece adds up the way.
    QPointF offset;
    QSet<int> seen;
    while (index >= 0 && index < scene.layers.size() && !seen.contains(index)) {
        seen.insert(index);
        const QJsonObject cut = scene.layers.at(index).raw.value("cut").toObject();
        if (cut.isEmpty())
            break;
        const QJsonArray origin = cut.value("origin").toArray();
        offset += QPointF(origin.at(0).toDouble(), origin.at(1).toDouble());
        index = scene.indexOf(cut.value("from").toString());
    }
    return offset;
}

QMatrix4x4 pictureToWorld(const Info &info) {
    // Up is the ground's normal; ahead is the camera's view laid flat on the
    // ground; right makes the three a right-handed set.
    const QVector3D y = info.up.normalized();
    QVector3D ahead = QVector3D(0, 0, 1) - QVector3D::dotProduct(QVector3D(0, 0, 1), y) * y;
    if (ahead.length() < 1e-6)
        ahead = QVector3D(0, 0, 1);
    ahead.normalize();
    const QVector3D z = -ahead;
    const QVector3D x = QVector3D::crossProduct(y, z).normalized();
    QMatrix4x4 m;
    m.setRow(0, QVector4D(x, 0));
    m.setRow(1, QVector4D(y, info.height * info.scale));
    m.setRow(2, QVector4D(z, 0));
    m.setRow(3, QVector4D(0, 0, 0, 1));
    return m;
}

namespace {

// The picture a world was made from: its pixels before any cuts.
QString pictureFile(const Scene &scene, int root) {
    const Layer &layer = scene.layers.at(root);
    return layer.raw.value("uncut").toString(layer.source);
}

struct DepthMap {
    QImage image;
    QSize picture;
    double focal = 0;
    // What part of the picture the image covers: all of it unless said, or
    // more, for one carried on past its edges.
    QPointF origin;
    QSizeF span;
    double scale = 1;
    // Distance at a point of the picture, nearest cell.
    double at(double x, double y) const {
        const QSizeF covers = span.isEmpty() ? QSizeF(picture) : span;
        const int gx = std::clamp(int((x - origin.x()) / covers.width() * image.width()), 0, image.width() - 1);
        const int gy = std::clamp(int((y - origin.y()) / covers.height() * image.height()), 0, image.height() - 1);
        const quint16 code = reinterpret_cast<const quint16 *>(image.constScanLine(gy))[gx];
        // The sky scales too, so it never looks nearer than what's in it.
        return decodeDepth(code) * scale;
    }
    bool isSky(double distance) const { return distance >= Far * scale * .999; }
    QVector3D point(double x, double y, double distance) const {
        return QVector3D((x - picture.width() / 2.0) * distance / focal, (y - picture.height() / 2.0) * distance / focal,
                         distance);
    }
};

// Depth images are read once per file and kept, as meshes ask often.
DepthMap depthMap(AssetCache &assets, const QString &file, const QSize &picture, double focal, double scale = 1) {
    static QHash<QString, QImage> cache;
    QMutexLocker lock(&cacheLock);
    const QString path = QDir(assets.directory()).filePath(file);
    const QString key = path + "|" + QString::number(QFileInfo(path).lastModified().toMSecsSinceEpoch());
    if (!cache.contains(key))
        cache.insert(key, QImage(path).convertToFormat(QImage::Format_Grayscale16));
    DepthMap map{cache.value(key), picture, focal};
    map.scale = scale;
    return map;
}

bool writeDepth(const QImage &image, const QString &path, QString *error) {
    QSaveFile file(path);
    QImageWriter writer(&file, "png");
    if (file.open(QIODevice::WriteOnly) && writer.write(image) && file.commit())
        return true;
    if (error)
        *error = QString("Couldn't write %1: %2").arg(path, file.errorString());
    return false;
}

// The direction a set of points spreads least in: the normal of the plane
// they lie closest to (the covariance's smallest eigenvector, by Jacobi).
QVector3D planeNormal(const QList<QVector3D> &points, const QVector3D &centre) {
    double a[3][3] = {}, v[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (const QVector3D &p : points) {
        const QVector3D d = p - centre;
        const double c[3] = {d.x(), d.y(), d.z()};
        for (int r = 0; r < 3; ++r)
            for (int k = 0; k < 3; ++k)
                a[r][k] += c[r] * c[k];
    }
    for (int sweep = 0; sweep < 32; ++sweep)
        for (int p = 0; p < 2; ++p)
            for (int q = p + 1; q < 3; ++q) {
                if (std::abs(a[p][q]) < 1e-12)
                    continue;
                const double theta = (a[q][q] - a[p][p]) / (2 * a[p][q]);
                const double t = (theta >= 0 ? 1 : -1) / (std::abs(theta) + std::sqrt(theta * theta + 1));
                const double c = 1 / std::sqrt(t * t + 1), s = t * c;
                for (int k = 0; k < 3; ++k) {
                    const double akp = a[k][p], akq = a[k][q];
                    a[k][p] = c * akp - s * akq;
                    a[k][q] = s * akp + c * akq;
                }
                for (int k = 0; k < 3; ++k) {
                    const double apk = a[p][k], aqk = a[q][k];
                    a[p][k] = c * apk - s * aqk;
                    a[q][k] = s * apk + c * aqk;
                }
                for (int k = 0; k < 3; ++k) {
                    const double vkp = v[k][p], vkq = v[k][q];
                    v[k][p] = c * vkp - s * vkq;
                    v[k][q] = s * vkp + c * vkq;
                }
            }
    int least = 0;
    for (int k = 1; k < 3; ++k)
        if (a[k][k] < a[least][least])
            least = k;
    return QVector3D(v[0][least], v[1][least], v[2][least]).normalized();
}

// A shape as stored: the model's vertices (x, y, z) and triangles, and which
// quarter turn about up faces it the way the cut-out shows it.
struct StoredShape {
    std::vector<float> positions;
    std::vector<float> colors;  // r, g, b a vertex; empty in old files.
    std::vector<quint32> triangles;
    int quarter = 0;
    bool isNull() const { return positions.empty(); }
};

StoredShape readShape(const QString &path) {
    static QHash<QString, StoredShape> cache;
    QMutexLocker lock(&cacheLock);
    const QString key = path + "|" + QString::number(QFileInfo(path).lastModified().toMSecsSinceEpoch());
    if (cache.contains(key))
        return cache.value(key);
    StoredShape shape;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return shape;
    const QByteArray bytes = file.readAll();
    // Version 1: positions and triangles; 2 adds a colour for each vertex.
    const bool coloured = bytes.startsWith("OMSHAPE2");
    if (bytes.size() < 16 || (!bytes.startsWith("OMSHAPE1") && !coloured))
        return shape;
    const quint32 vertices = *reinterpret_cast<const quint32 *>(bytes.constData() + 8);
    const quint32 indices = *reinterpret_cast<const quint32 *>(bytes.constData() + 12);
    const qsizetype colourBytes = coloured ? qsizetype(vertices) * 12 : 0;
    if (bytes.size() != qsizetype(16 + vertices * 12 + colourBytes + indices * 4))
        return shape;
    const float *p = reinterpret_cast<const float *>(bytes.constData() + 16);
    shape.positions.assign(p, p + vertices * 3);
    if (coloured)
        shape.colors.assign(p + vertices * 3, p + vertices * 6);
    const quint32 *t = reinterpret_cast<const quint32 *>(bytes.constData() + 16 + vertices * 12 + colourBytes);
    shape.triangles.assign(t, t + indices);
    cache.insert(key, shape);
    return shape;
}

// The model's vertex turned by quarter turns about up.
QVector3D quarterTurn(const float *p, int quarter) {
    float x = p[0], z = p[2];
    for (int i = 0; i < ((quarter % 4) + 4) % 4; ++i) {
        const float nx = z, nz = -x;
        x = nx, z = nz;
    }
    return QVector3D(x, p[1], z);
}

// The see-through-or-not of a cut-out's image, and the box of what shows.
QRectF shownBox(const QImage &image) {
    QRect box;
    for (int y = 0; y < image.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x)
            if (qAlpha(line[x]) > 128)
                box |= QRect(x, y, 1, 1);
    }
    return QRectF(box);
}

}

QQuaternion turn(double yaw, double pitch, double roll) {
    // Yaw to the right about up, then pitch up about the right, then roll
    // clockwise about ahead, as a driver or a camera turns.
    return QQuaternion::fromAxisAndAngle(0, 1, 0, float(-yaw)) * QQuaternion::fromAxisAndAngle(1, 0, 0, float(pitch)) *
           QQuaternion::fromAxisAndAngle(0, 0, 1, float(-roll));
}

// A piece's own distances: the picture's depth cells under its rectangle,
// keeping only those it covers (its edge cells mix in what's behind it, as
// far as the sky), held between the 5th and 95th percentile of them, and
// the rest filled from those, so its edges stay on it.
// The picture's depth as one piece sees it: where another piece was cut
// out over it (a car in front of painted dust, say), what's behind that
// piece, not the piece itself.
DepthMap depthUnderOthers(const Scene &scene, AssetCache &assets, int index, const DepthMap &depth, const DepthMap &behind) {
    DepthMap seen = depth;
    seen.image = depth.image.copy();
    const double toCell = double(depth.image.width()) / depth.picture.width();
    bool changed = false;
    for (const int other : layers(scene)) {
        if (other == index || other == scene.indexOf(info(scene).picture) ||
            !scene.layers.at(other).raw.value("cut").toObject().value("punch").toBool(true))
            continue;
        double factor = 1;
        const QSizeF size = assets.size(scene.layers.at(other).source);
        const QImage image = assets.mip(scene.layers.at(other).source, std::min(1.0, 256 / std::max(1.0, size.width())), &factor);
        if (image.isNull())
            continue;
        const QPointF offset = offsetInPicture(scene, other);
        const double k = size.width() / image.width();
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) {
                if (qAlpha(image.pixel(x, y)) < 128)
                    continue;
                const double px = offset.x() + (x + .5) * k, py = offset.y() + (y + .5) * k;
                const int cx = int(px * toCell), cy = int(py * toCell);
                if (cx < 0 || cy < 0 || cx >= seen.image.width() || cy >= seen.image.height())
                    continue;
                const double under = behind.at(px, py) / std::max(1e-9, behind.scale) ;
                reinterpret_cast<quint16 *>(seen.image.scanLine(cy))[cx] = encodeDepth(under);
                changed = true;
            }
    }
    return changed ? seen : depth;
}

DepthMap pieceDepth(const DepthMap &picture, const QImage &piece, const QPointF &offset, const QSizeF &size,
                    double *middleOut = nullptr) {
    const double cell = double(picture.picture.width()) / picture.image.width();
    const int w = std::max(2, int(std::ceil(size.width() / cell))), h = std::max(2, int(std::ceil(size.height() / cell)));
    QImage local(w, h, QImage::Format_Grayscale16), hole(w, h, QImage::Format_Grayscale8);
    QList<double> inside;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double px = (x + .5) * cell, py = (y + .5) * cell;
            const int ix = std::clamp(int(px / size.width() * piece.width()), 0, piece.width() - 1);
            const int iy = std::clamp(int(py / size.height() * piece.height()), 0, piece.height() - 1);
            const double d = picture.at(offset.x() + px, offset.y() + py);
            const bool covered = qAlpha(piece.pixel(ix, iy)) > 200 && !picture.isSky(d);
            // Kept in meters as they are here; the local map isn't scaled again.
            reinterpret_cast<quint16 *>(local.scanLine(y))[x] = encodeDepth(d);
            hole.scanLine(y)[x] = covered ? 0 : 255;
            if (covered)
                inside.append(d);
        }
    if (inside.isEmpty())
        return picture;
    // A solid thing is about as deep as it is wide: its relief is held to
    // its middle distance give or take 40% of its width there, so gaps in it
    // that see through to the ground don't push parts of it meters back.
    std::sort(inside.begin(), inside.end());
    const double middle = inside.at(inside.size() / 2), width = size.width() / picture.focal * middle;
    if (middleOut)
        *middleOut = middle;
    const double low = std::max(inside.at(inside.size() / 20), middle - .4 * width);
    const double high = std::min(inside.at(inside.size() * 19 / 20), middle + .4 * width);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (!hole.scanLine(y)[x]) {
                quint16 &code = reinterpret_cast<quint16 *>(local.scanLine(y))[x];
                code = encodeDepth(std::clamp(decodeDepth(code), low, high));
            }
    fillDepth(local, hole);
    // And smoothed, more the bigger it is, so its surface has no folds: a
    // flat cut-out is a gently curved card, not a relief of every speck.
    const int passes = std::clamp(std::max(w, h) / 6, 3, 40);
    for (int pass = 0; pass < passes; ++pass) {
        const QImage before = local.copy();
        for (int y = 1; y + 1 < h; ++y)
            for (int x = 1; x + 1 < w; ++x) {
                double sum = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                        sum += std::log(decodeDepth(reinterpret_cast<const quint16 *>(before.constScanLine(y + dy))[x + dx]));
                reinterpret_cast<quint16 *>(local.scanLine(y))[x] = encodeDepth(std::exp(sum / 9));
            }
    }
    // Laid out as a picture of its own: the piece's rectangle, cell for cell.
    return DepthMap{local, QSize(int(std::round(w * cell)), int(std::round(h * cell))), picture.focal};
}

namespace {

// Where a shaped piece's model goes, from the picture's camera: centred on
// what shows of the cut-out, as wide and tall as it at its middle distance,
// facing the camera as the cut-out does. The model's x and y turn half
// round about the view, as it was seen from its -z.
struct ShapeFit {
    StoredShape model;
    QVector3D centre;      // In the picture's camera, meters.
    QVector3D modelMiddle;
    float scale = 1;
    QPointF offset;
    QSizeF size;
    bool valid = false;
    QVector3D place(const float *p) const {
        const QVector3D v = quarterTurn(p, model.quarter) - modelMiddle;
        return centre + scale * QVector3D(-v.x(), -v.y(), v.z());
    }
};

ShapeFit computeFit(const Scene &scene, AssetCache &assets, int index);

// Fitting reads the piece's depth closely, and every frame asks where a
// piece is: fits are kept until what they depend on changes.
QByteArray layerKey(const Scene &scene, AssetCache &assets, int index) {
    if (index == FarLayer)
        return QJsonDocument(QJsonObject{{"world", scene.raw.value("world")}, {"directory", assets.directory()}, {"index", index}})
            .toJson(QJsonDocument::Compact);
    const Layer &layer = scene.layers.at(index);
    return QJsonDocument(QJsonObject{{"world", scene.raw.value("world")},
                                     {"directory", assets.directory()},
                                     {"index", index},
                                     {"source", layer.source},
                                     {"shape", layer.raw.value("shape")},
                                     {"cut", layer.raw.value("cut")},
                                     {"anchor", QJsonArray{layer.anchor.x(), layer.anchor.y()}}})
        .toJson(QJsonDocument::Compact);
}

ShapeFit fitShape(const Scene &scene, AssetCache &assets, int index) {
    static QHash<QByteArray, ShapeFit> cache;
    if (index < 0 || index >= scene.layers.size())
        return {};
    const QByteArray key = layerKey(scene, assets, index);
    {
        QMutexLocker lock(&cacheLock);
        if (cache.contains(key))
            return cache.value(key);
    }
    const ShapeFit fit = computeFit(scene, assets, index);
    QMutexLocker lock(&cacheLock);
    if (cache.size() > 64)
        cache.clear();
    cache.insert(key, fit);
    return fit;
}

// A grid's normals, from across a few cells either way, so the lights
// see the lie of the land rather than every little step in the AI's
// distances.
void addNormals(Mesh &mesh) {
    if (mesh.columns <= 0)
        return;
    const float *v = reinterpret_cast<const float *>(mesh.vertices.constData());
    const int columns = mesh.columns, rows = int(mesh.vertices.size() / qsizetype(mesh.stride * sizeof(float))) / columns;
    auto at = [&](int r, int c) {
        r = std::clamp(r, 0, rows - 1), c = std::clamp(c, 0, columns - 1);
        const float *p = v + (size_t(r) * columns + c) * mesh.stride;
        return QVector3D(p[0], p[1], p[2]);
    };
    mesh.normals.resize(qsizetype(columns) * rows * 3 * sizeof(float));
    float *n = reinterpret_cast<float *>(mesh.normals.data());
    constexpr int reach = 2;
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < columns; ++c) {
            const QVector3D across = at(r, c + reach) - at(r, c - reach), down = at(r + reach, c) - at(r - reach, c);
            const QVector3D normal = QVector3D::crossProduct(down, across).normalized();
            *n++ = normal.x(), *n++ = normal.y(), *n++ = normal.z();
        }
}

// A shape's mesh modifiers, as Blender's, in order: "smooth" relaxes it
// (Taubin's way, so it doesn't shrink) "times" over; "simplify" merges
// points closer than a "cells"th of its size into one.
void modifyShape(StoredShape &shape, const QJsonArray &modifiers) {
    for (const QJsonValue &value : modifiers) {
        const QJsonObject m = value.toObject();
        const size_t count = shape.positions.size() / 3;
        if (count == 0)
            return;
        if (m.value("type").toString() == "smooth") {
            std::vector<std::vector<quint32>> around(count);
            for (size_t t = 0; t + 2 < shape.triangles.size(); t += 3)
                for (int k = 0; k < 3; ++k) {
                    const quint32 a = shape.triangles[t + k], b = shape.triangles[t + (k + 1) % 3];
                    around[a].push_back(b);
                    around[b].push_back(a);
                }
            const int times = std::clamp(m.value("times").toInt(2), 1, 50);
            std::vector<float> next = shape.positions;
            for (int pass = 0; pass < times * 2; ++pass) {
                // In towards its neighbours, then out a little more.
                const float step = pass % 2 ? -.53f : .5f;
                for (size_t i = 0; i < count; ++i) {
                    if (around[i].empty())
                        continue;
                    float mean[3] = {0, 0, 0};
                    for (const quint32 j : around[i])
                        for (int k = 0; k < 3; ++k)
                            mean[k] += shape.positions[j * 3 + k];
                    for (int k = 0; k < 3; ++k)
                        next[i * 3 + k] = shape.positions[i * 3 + k] + step * (mean[k] / float(around[i].size()) - shape.positions[i * 3 + k]);
                }
                shape.positions = next;
            }
        } else if (m.value("type").toString() == "simplify") {
            float low[3] = {1e30f, 1e30f, 1e30f}, high[3] = {-1e30f, -1e30f, -1e30f};
            for (size_t i = 0; i < count; ++i)
                for (int k = 0; k < 3; ++k)
                    low[k] = std::min(low[k], shape.positions[i * 3 + k]), high[k] = std::max(high[k], shape.positions[i * 3 + k]);
            const float span = std::max({high[0] - low[0], high[1] - low[1], high[2] - low[2], 1e-6f});
            const int cells = std::clamp(m.value("cells").toInt(48), 4, 1024);
            const float cell = span / float(cells);
            QHash<quint64, quint32> merged;
            std::vector<quint32> into(count);
            std::vector<float> positions, colors;
            std::vector<int> counts;
            const bool coloured = shape.colors.size() == shape.positions.size();
            for (size_t i = 0; i < count; ++i) {
                quint64 key = 0;
                for (int k = 0; k < 3; ++k)
                    key = key * 2048 + quint64(std::clamp(int((shape.positions[i * 3 + k] - low[k]) / cell), 0, 2047));
                auto found = merged.constFind(key);
                if (found == merged.constEnd()) {
                    found = merged.insert(key, quint32(counts.size()));
                    counts.push_back(0);
                    positions.insert(positions.end(), {0, 0, 0});
                    if (coloured)
                        colors.insert(colors.end(), {0, 0, 0});
                }
                const quint32 j = *found;
                into[i] = j;
                ++counts[j];
                for (int k = 0; k < 3; ++k) {
                    positions[j * 3 + k] += shape.positions[i * 3 + k];
                    if (coloured)
                        colors[j * 3 + k] += shape.colors[i * 3 + k];
                }
            }
            for (size_t j = 0; j < counts.size(); ++j)
                for (int k = 0; k < 3; ++k) {
                    positions[j * 3 + k] /= float(counts[j]);
                    if (coloured)
                        colors[j * 3 + k] /= float(counts[j]);
                }
            std::vector<quint32> triangles;
            for (size_t t = 0; t + 2 < shape.triangles.size(); t += 3) {
                const quint32 a = into[shape.triangles[t]], b = into[shape.triangles[t + 1]], c = into[shape.triangles[t + 2]];
                if (a != b && b != c && a != c)
                    triangles.insert(triangles.end(), {a, b, c});
            }
            shape.positions = positions;
            shape.colors = colors;
            shape.triangles = triangles;
        }
    }
}

ShapeFit computeFit(const Scene &scene, AssetCache &assets, int index) {
    ShapeFit fit;
    const Info world = info(scene);
    const int root = scene.indexOf(world.picture);
    const Layer &layer = scene.layers.at(index);
    const QString file = layer.raw.value("shape").toObject().value("file").toString();
    if (!world.valid || index == root || file.isEmpty())
        return fit;
    fit.model = readShape(QDir(assets.directory()).filePath(file));
    fit.model.quarter = layer.raw.value("shape").toObject().value("quarter").toInt();
    modifyShape(fit.model, layer.raw.value("shape").toObject().value("modifiers").toArray());
    fit.size = assets.size(layer.source);
    const QSizeF picture = assets.size(pictureFile(scene, root));
    const DepthMap depth = depthMap(assets, world.depth, picture.toSize(), world.focal, world.scale);
    double factor = 1;
    const QImage image = assets.mip(layer.source, std::min(1.0, 512 / fit.size.width()), &factor);
    if (fit.model.isNull() || depth.image.isNull() || image.isNull())
        return fit;
    fit.offset = offsetInPicture(scene, index);
    double middle = 0;
    pieceDepth(depth, image, fit.offset, fit.size, &middle);
    const QRectF shown = shownBox(image);
    if (!(middle > 0) || shown.isEmpty())
        return fit;
    const double k = fit.size.width() / image.width();
    const QRectF box(fit.offset + shown.topLeft() * k, shown.size() * k);
    QVector3D low(1e9f, 1e9f, 1e9f), high = -low;
    for (size_t i = 0; i < fit.model.positions.size(); i += 3) {
        const QVector3D v = quarterTurn(&fit.model.positions[i], fit.model.quarter);
        low = QVector3D(std::min(low.x(), v.x()), std::min(low.y(), v.y()), std::min(low.z(), v.z()));
        high = QVector3D(std::max(high.x(), v.x()), std::max(high.y(), v.y()), std::max(high.z(), v.z()));
    }
    fit.modelMiddle = (low + high) / 2;
    const double perPixel = middle / world.focal;
    fit.scale = float((box.width() * perPixel / std::max(1e-6f, high.x() - low.x()) +
                       box.height() * perPixel / std::max(1e-6f, high.y() - low.y())) / 2);
    fit.centre = depth.point(box.center().x(), box.center().y(), middle);
    fit.valid = true;
    return fit;
}

}

namespace {
Mesh buildMesh(const Scene &scene, AssetCache &assets, int index);
}

Mesh mesh(const Scene &scene, AssetCache &assets, int index) {
    static QHash<QByteArray, Mesh> cache;
    if ((index < 0 || index >= scene.layers.size()) && !(index == FarLayer && !info(scene).farDepth.isEmpty()))
        return {};
    const QByteArray key = layerKey(scene, assets, index);
    {
        QMutexLocker lock(&cacheLock);
        if (cache.contains(key))
            return cache.value(key);
    }
    const Mesh made = buildMesh(scene, assets, index);
    QMutexLocker lock(&cacheLock);
    if (cache.size() > 32)
        cache.clear();
    cache.insert(key, made);
    return made;
}

namespace {
Mesh buildMesh(const Scene &scene, AssetCache &assets, int index) {
    const Info world = info(scene);
    const int root = scene.indexOf(world.picture);
    if (index == FarLayer) {
        // Laid out as the widened picture is, on its own distances.
        if (!world.valid || world.farDepth.isEmpty())
            return {};
        const QSizeF picture = assets.size(pictureFile(scene, root));
        DepthMap depth = depthMap(assets, world.farDepth, picture.toSize(), world.focal, world.scale);
        if (depth.image.isNull() || picture.isEmpty())
            return {};
        const QPointF offset = -QPointF(world.margin.width(), world.margin.height());
        const QSizeF size = picture + QSizeF(2 * world.margin.width(), 2 * world.margin.height());
        depth.origin = offset;
        depth.span = size;
        const QMatrix4x4 toWorld = pictureToWorld(world);
        // A vertex for each depth cell, as the picture has, so the two meet.
        const int cols = std::clamp(depth.image.width() - 1, 2, 1600), rows = std::clamp(depth.image.height() - 1, 2, 1000);
        Mesh mesh;
        mesh.columns = cols + 1;
        mesh.vertices.resize(qsizetype(cols + 1) * (rows + 1) * 5 * sizeof(float));
        float *v = reinterpret_cast<float *>(mesh.vertices.data());
        for (int r = 0; r <= rows; ++r)
            for (int c = 0; c <= cols; ++c) {
                const double x = offset.x() + size.width() * c / cols, y = offset.y() + size.height() * r / rows;
                const QVector3D p = toWorld.map(depth.point(x, y, depth.at(x, y)));
                *v++ = p.x(), *v++ = p.y(), *v++ = p.z();
                *v++ = float(c) / cols, *v++ = 1 - float(r) / rows;
            }
        mesh.indices.resize(qsizetype(cols) * rows * 6 * sizeof(quint32));
        quint32 *i = reinterpret_cast<quint32 *>(mesh.indices.data());
        for (int r = 0; r < rows; ++r)
            for (int c = 0; c < cols; ++c) {
                const quint32 a = r * (cols + 1) + c, b = a + 1, d = a + cols + 1, e = d + 1;
                *i++ = a, *i++ = d, *i++ = b, *i++ = b, *i++ = d, *i++ = e;
            }
        addNormals(mesh);
        mesh.minimum = QVector3D(-1e4f, -1e4f, -1e4f);
        mesh.maximum = QVector3D(1e4f, 1e4f, 1e4f);
        return mesh;
    }
    if (!world.valid || index < 0 || index >= scene.layers.size() || rootOf(scene, index) != root)
        return {};
    const Layer &layer = scene.layers.at(index);
    QSizeF size = assets.size(layer.source);
    const QSizeF picture = assets.size(pictureFile(scene, root));
    if (size.isEmpty() || picture.isEmpty())
        return {};

    // A shaped piece: its model, placed on the cut-out, wearing the picture
    // as seen from its camera (the side away from it wears it too, through).
    const ShapeFit fit = fitShape(scene, assets, index);
    if (fit.valid) {
        const QMatrix4x4 toWorld = pictureToWorld(world);
        const size_t count = fit.model.positions.size() / 3;
        std::vector<QVector3D> seen(count), normals(count);
        for (size_t i = 0; i < count; ++i)
            seen[i] = fit.place(&fit.model.positions[i * 3]);
        // Held inside the cut-out's outline: from the picture's camera, a
        // point of the shape that falls outside what the picture shows of it
        // moves in to the nearest point that's inside, at its own distance,
        // so at rest it covers just what the picture does.
        {
            double mipFactor = 1;
            const QImage outline = assets.mip(layer.source, std::min(1.0, 512 / fit.size.width()), &mipFactor);
            const int w = outline.width(), h = outline.height();
            std::vector<qint32> nearest(size_t(w) * h, -1);
            QList<qint32> queue;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    if (qAlpha(outline.pixel(x, y)) > 128)
                        nearest[size_t(y) * w + x] = y * w + x, queue.append(y * w + x);
            // Spread outward from the inside: each pixel learns its nearest
            // inside pixel from a neighbour that already knows one.
            for (qsizetype q = 0; q < queue.size(); ++q) {
                const int at = queue.at(q), x = at % w, y = at / w;
                for (const auto [dx, dy] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}}) {
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h || nearest[size_t(ny) * w + nx] >= 0)
                        continue;
                    nearest[size_t(ny) * w + nx] = nearest[size_t(at)];
                    queue.append(ny * w + nx);
                }
            }
            const double toOutline = w / fit.size.width();
            if (!queue.isEmpty())
                for (QVector3D &p : seen) {
                    const double px = picture.width() / 2.0 + world.focal * p.x() / p.z() - fit.offset.x();
                    const double py = picture.height() / 2.0 + world.focal * p.y() / p.z() - fit.offset.y();
                    const int ox = std::clamp(int(px * toOutline), 0, w - 1), oy = std::clamp(int(py * toOutline), 0, h - 1);
                    const bool outside = px < 0 || py < 0 || px >= fit.size.width() || py >= fit.size.height() ||
                                         qAlpha(outline.pixel(ox, oy)) <= 128;
                    const qint32 in = nearest[size_t(oy) * w + ox];
                    if (!outside || in < 0)
                        continue;
                    const double ix = (in % w + .5) / toOutline + fit.offset.x(), iy = (in / w + .5) / toOutline + fit.offset.y();
                    p = QVector3D(float((ix - picture.width() / 2.0) * p.z() / world.focal),
                                  float((iy - picture.height() / 2.0) * p.z() / world.focal), p.z());
                }
        }
        for (size_t t = 0; t + 2 < fit.model.triangles.size(); t += 3) {
            const quint32 a = fit.model.triangles[t], b = fit.model.triangles[t + 1], c = fit.model.triangles[t + 2];
            const QVector3D n = QVector3D::crossProduct(seen[b] - seen[a], seen[c] - seen[a]);
            normals[a] += n, normals[b] += n, normals[c] += n;
        }
        // What the picture saw of it: the side facing its camera, inside
        // the cut-out. Sticking out past the cut-out but facing the camera
        // would show at rest where the picture shows none of it: there it's
        // see-through. Elsewhere it wears the shape's own colour.
        double factor = 1;
        const QImage cut = assets.mip(layer.source, std::min(1.0, 1024 / fit.size.width()), &factor);
        const float cutScale = float(cut.width() / fit.size.width());
        Mesh mesh;
        // Then two as a texture coordinate (worn, and nothing) and the normal.
        mesh.stride = 14;
        mesh.vertices.resize(qsizetype(count) * mesh.stride * sizeof(float));
        float *v = reinterpret_cast<float *>(mesh.vertices.data());
        mesh.minimum = QVector3D(1e9f, 1e9f, 1e9f);
        mesh.maximum = -mesh.minimum;
        for (size_t i = 0; i < count; ++i) {
            const QVector3D p = toWorld.map(seen[i]);
            const double px = picture.width() / 2.0 + world.focal * seen[i].x() / seen[i].z();
            const double py = picture.height() / 2.0 + world.focal * seen[i].y() / seen[i].z();
            const double u = (px - fit.offset.x()) / fit.size.width(), w = (py - fit.offset.y()) / fit.size.height();
            const double facing = -QVector3D::dotProduct(normals[i].normalized(), seen[i].normalized());
            const int cx = int((px - fit.offset.x()) * cutScale), cy = int((py - fit.offset.y()) * cutScale);
            const bool inside = cx >= 0 && cy >= 0 && cx < cut.width() && cy < cut.height() && qAlpha(cut.pixel(cx, cy)) > 128;
            const float worn = float(std::clamp((facing - .1) / .3, 0.0, 1.0)) * (inside ? 1.f : 0.f);
            const float alpha = facing > .1 && !inside ? 0.f : 1.f;
            *v++ = p.x(), *v++ = p.y(), *v++ = p.z();
            *v++ = float(u), *v++ = float(1 - w);
            if (fit.model.colors.size() == fit.model.positions.size())
                *v++ = fit.model.colors[i * 3], *v++ = fit.model.colors[i * 3 + 1], *v++ = fit.model.colors[i * 3 + 2];
            else
                *v++ = .5f, *v++ = .5f, *v++ = .5f;
            *v++ = alpha;
            *v++ = worn;
            *v++ = 0;
            const QVector3D normal = toWorld.mapVector(normals[i]).normalized();
            *v++ = normal.x(), *v++ = normal.y(), *v++ = normal.z();
            mesh.minimum = QVector3D(std::min(mesh.minimum.x(), p.x()), std::min(mesh.minimum.y(), p.y()), std::min(mesh.minimum.z(), p.z()));
            mesh.maximum = QVector3D(std::max(mesh.maximum.x(), p.x()), std::max(mesh.maximum.y(), p.y()), std::max(mesh.maximum.z(), p.z()));
        }
        mesh.indices = QByteArray(reinterpret_cast<const char *>(fit.model.triangles.data()),
                                  qsizetype(fit.model.triangles.size() * sizeof(quint32)));
        mesh.pivot = toWorld.map(fit.centre);
        return mesh;
    }

    // The picture lies on what's behind its pieces; pieces on the picture's
    // own depth. Carried on past its edges, the picture covers more.
    DepthMap depth = depthMap(assets, index == root ? world.behind : world.depth, picture.toSize(), world.focal, world.scale);
    if (depth.image.isNull())
        return {};
    QPointF offset = index == root ? QPointF() : offsetInPicture(scene, index);
    if (index == root && !world.wide.isEmpty() && !world.margin.isEmpty()) {
        offset = -QPointF(world.margin.width(), world.margin.height());
        size = picture + QSizeF(2 * world.margin.width(), 2 * world.margin.height());
        depth.origin = offset;
        depth.span = size;
    }
    const QMatrix4x4 toWorld = pictureToWorld(world);

    // About four picture pixels a cell, at most 320 across, at least 2. The
    // picture itself, torn where its distance jumps, takes a vertex for each
    // of its depth cells, so it tears just where the layer behind is made.
    int cols = std::clamp(int(size.width() / 4), 2, 320), rows = std::clamp(int(size.height() / 4), 2, 320);
    if (index == root && !world.farDepth.isEmpty()) {
        cols = std::clamp(depth.image.width() - 1, 2, 1600);
        rows = std::clamp(depth.image.height() - 1, 2, 1000);
    }
    // Its box counts only what shows: a piece's see-through margin reads
    // whatever lies behind it, as far as the sky.
    double factor = 1;
    const QImage image = index == root ? QImage() : assets.mip(layer.source, std::min(1.0, 4 * cols / size.width()), &factor);
    DepthMap own = depth;
    DepthMap behind;
    if (index != root && !image.isNull()) {
        behind = depthMap(assets, world.behind, picture.toSize(), world.focal, world.scale);
        if (!world.wide.isEmpty() && !world.margin.isEmpty()) {
            behind.origin = -QPointF(world.margin.width(), world.margin.height());
            behind.span = picture + QSizeF(2 * world.margin.width(), 2 * world.margin.height());
        }
        own = pieceDepth(behind.image.isNull() ? depth : depthUnderOthers(scene, assets, index, depth, behind), image, offset, size);
    }
    // A piece lies in front of what's behind it, never in it.
    auto inFront = [&](double x, double y, double distance) {
        if (behind.image.isNull())
            return distance;
        const double back = behind.at(x, y);
        return !behind.isSky(back) ? std::min(distance, back * .97) : distance;
    };
    Mesh mesh;
    mesh.vertices.resize(qsizetype(cols + 1) * (rows + 1) * 5 * sizeof(float));
    float *v = reinterpret_cast<float *>(mesh.vertices.data());
    mesh.minimum = QVector3D(1e9f, 1e9f, 1e9f);
    mesh.maximum = -mesh.minimum;
    for (int r = 0; r <= rows; ++r)
        for (int c = 0; c <= cols; ++c) {
            const double px = size.width() * c / cols, py = size.height() * r / rows;
            const double x = offset.x() + px, y = offset.y() + py;
            const double distance = index == root ? depth.at(x, y) : inFront(x, y, own.at(px, py));
            const QVector3D p = toWorld.map(depth.point(x, y, distance));
            *v++ = p.x();
            *v++ = p.y();
            *v++ = p.z();
            *v++ = float(c) / cols;
            *v++ = 1 - float(r) / rows;
            if (!image.isNull()) {
                const int ix = std::clamp(int(double(c) / cols * image.width()), 0, image.width() - 1);
                const int iy = std::clamp(int(double(r) / rows * image.height()), 0, image.height() - 1);
                if (qAlpha(image.pixel(ix, iy)) < 16)
                    continue;
            }
            mesh.minimum = QVector3D(std::min(mesh.minimum.x(), p.x()), std::min(mesh.minimum.y(), p.y()),
                                     std::min(mesh.minimum.z(), p.z()));
            mesh.maximum = QVector3D(std::max(mesh.maximum.x(), p.x()), std::max(mesh.maximum.y(), p.y()),
                                     std::max(mesh.maximum.z(), p.z()));
        }
    mesh.columns = cols + 1;
    // With a layer behind its near edges, the picture doesn't stretch across
    // a jump in distance: those triangles go, and a moving camera sees the
    // layer behind through the gap.
    const bool tear = index == root && !world.farDepth.isEmpty();
    const QVector3D eye = toWorld.map(QVector3D(0, 0, 0));
    const float *vertex = reinterpret_cast<const float *>(mesh.vertices.constData());
    auto distanceOf = [&](quint32 k) { return (QVector3D(vertex[k * 5], vertex[k * 5 + 1], vertex[k * 5 + 2]) - eye).length(); };
    auto stretched = [&](quint32 a, quint32 b, quint32 c) {
        const float da = distanceOf(a), db = distanceOf(b), dc = distanceOf(c);
        return std::max({da, db, dc}) > 1.15f * std::min({da, db, dc});
    };
    QList<quint32> kept;
    kept.reserve(cols * rows * 6);
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            const quint32 a = r * (cols + 1) + c, b = a + 1, d = a + cols + 1, e = d + 1;
            if (!tear || !stretched(a, d, b))
                kept << a << d << b;
            if (!tear || !stretched(b, d, e))
                kept << b << d << e;
        }
    mesh.indices = QByteArray(reinterpret_cast<const char *>(kept.constData()), kept.size() * qsizetype(sizeof(quint32)));

    mesh.pivot = pivot(scene, assets, index);
    addNormals(mesh);
    return mesh;
}

}

QVector3D pivot(const Scene &scene, AssetCache &assets, int index) {
    const Info world = info(scene);
    const int root = scene.indexOf(world.picture);
    if (!world.valid || index < 0 || index >= scene.layers.size())
        return {};
    const Layer &layer = scene.layers.at(index);
    const QSizeF size = assets.size(layer.source);
    const QSizeF picture = assets.size(pictureFile(scene, root));
    const DepthMap depth = depthMap(assets, index == root ? world.behind : world.depth, picture.toSize(), world.focal, world.scale);
    if (depth.image.isNull() || size.isEmpty())
        return {};
    const ShapeFit fit = fitShape(scene, assets, index);
    if (fit.valid)
        return pictureToWorld(world).map(fit.centre);
    const QPointF offset = index == root ? QPointF() : offsetInPicture(scene, index);
    // The anchor's spot at its distance. Where the anchor is on sky or
    // empty, the middle of the piece's own distances stands in.
    const double ax = offset.x() + layer.anchor.x() * size.width(), ay = offset.y() + layer.anchor.y() * size.height();
    double distance = depth.at(ax, ay);
    if (depth.isSky(distance)) {
        QList<double> all;
        for (int r = 0; r <= 8; ++r)
            for (int c = 0; c <= 8; ++c) {
                const double d = depth.at(offset.x() + size.width() * c / 8, offset.y() + size.height() * r / 8);
                if (!depth.isSky(d))
                    all.append(d);
            }
        std::sort(all.begin(), all.end());
        distance = all.isEmpty() ? 10 : all.at(all.size() / 2);
    }
    return pictureToWorld(world).map(depth.point(ax, ay, distance));
}

namespace {
// The light around a piece: the background's average colour in a ring
// round its rectangle, a little brighter, as light does round an edge.
QColor wrapColour(const Scene &scene, AssetCache &assets, int index) {
    static QHash<QByteArray, QColor> cache;
    const QByteArray key = layerKey(scene, assets, index);
    {
        QMutexLocker lock(&cacheLock);
        if (cache.contains(key))
            return cache.value(key);
    }
    const int root = scene.indexOf(info(scene).picture);
    double factor = 1;
    const QSizeF full = assets.size(scene.layers.at(root).source);
    const QImage background = assets.mip(scene.layers.at(root).source, std::min(1.0, 256 / std::max(1.0, full.width())), &factor);
    const double k = background.width() / std::max(1.0, full.width());
    const QSizeF size = assets.size(scene.layers.at(index).source);
    const QRectF piece(offsetInPicture(scene, index) * k, size * k);
    const QRectF ring = piece.adjusted(-piece.width() * .25, -piece.height() * .25, piece.width() * .25, piece.height() * .25);
    double r = 0, g = 0, b = 0;
    int n = 0;
    for (int y = std::max(0, int(ring.top())); y < std::min(background.height(), int(ring.bottom())); ++y)
        for (int x = std::max(0, int(ring.left())); x < std::min(background.width(), int(ring.right())); ++x) {
            if (piece.contains(x, y))
                continue;
            const QRgb p = background.pixel(x, y);
            r += qRed(p), g += qGreen(p), b += qBlue(p), ++n;
        }
    QColor colour = n ? QColor(int(r / n), int(g / n), int(b / n)) : QColor(128, 128, 128);
    colour = colour.lighter(130);
    QMutexLocker lock(&cacheLock);
    cache.insert(key, colour);
    return colour;
}
}

View cameraAt(const Scene &scene, AssetCache &assets, double t) {
    View view;
    const Info world = info(scene);
    if (!world.valid)
        return view;
    const QSizeF picture = assets.size(pictureFile(scene, scene.indexOf(world.picture)));
    const QMatrix4x4 toWorld = pictureToWorld(world);
    // The picture's camera, turned to look as Qt Quick 3D's do (down -z, up
    // +y), then moved and turned by the camera's world properties.
    const Layer &camera = scene.camera;
    QMatrix3x3 axes;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            axes(r, c) = toWorld(r, c) * (c == 0 ? 1 : -1);
    const QQuaternion picturedTurn = QQuaternion::fromRotationMatrix(axes);
    const QQuaternion yaw = QQuaternion::fromAxisAndAngle(0, 1, 0, float(-scene.value(camera, "yaw", t)));
    const QQuaternion pitchRoll = QQuaternion::fromAxisAndAngle(1, 0, 0, float(scene.value(camera, "pitch", t))) *
                                  QQuaternion::fromAxisAndAngle(0, 0, 1, float(-scene.value(camera, "rotation", t)));
    view.rotation = yaw * picturedTurn * pitchRoll;
    view.position = toWorld.map(QVector3D(0, 0, 0)) +
                           QVector3D(scene.value(camera, "worldX", t), scene.value(camera, "worldY", t),
                                     -scene.value(camera, "worldZ", t));
    const double zoom = std::max(1e-3, scene.value(camera, "zoom", t));
    const double half = std::atan(picture.height() / 2 / world.focal) ;
    view.fov = 2 * std::atan(std::tan(half) / zoom) * 180 / M_PI;

    return view;
}

Frame frame(const Scene &scene, AssetCache &assets, double t) {
    Frame frame;
    const Info world = info(scene);
    if (!world.valid)
        return frame;
    const int root = scene.indexOf(world.picture);
    const QSizeF picture = assets.size(pictureFile(scene, root));
    const QMatrix4x4 toWorld = pictureToWorld(world);

    const View camera = cameraAt(scene, assets, t);
    frame.cameraRotation = camera.rotation;
    frame.cameraPosition = camera.position;
    frame.fov = camera.fov;

    frame.time = t;
    frame.haze = world.haze;
    for (const int index : modelLayers(scene)) {
        const Layer &layer = scene.layers.at(index);
        const QJsonObject model = layer.raw.value("model").toObject();
        QVector3D low, high;
        const QString file = QDir(assets.directory()).filePath(model.value("file").toString());
        if (!modelBounds(file, &low, &high))
            continue;
        Placement place;
        place.layer = index;
        place.model = file;
        place.visible = layer.visible;
        place.opacity = float(std::clamp(scene.value(layer, "opacity", t), 0.0, 1.0));
        // Standing on its point: the middle of its bottom there, as tall as
        // its size says.
        const float tallest = std::max({high.x() - low.x(), high.y() - low.y(), high.z() - low.z(), 1e-6f});
        place.scale = float(std::max(.001, model.value("size").toDouble(1))) / tallest;
        place.offset = -QVector3D((low.x() + high.x()) / 2, low.y(), (low.z() + high.z()) / 2);
        place.rotation = turn(scene.value(layer, "yaw", t), scene.value(layer, "pitch", t), scene.value(layer, "roll", t));
        place.position = lightPosition(scene, index, t);
        const QJsonObject material = layer.raw.value("material").toObject();
        place.shine = float(std::clamp(material.value("shine").toDouble(0), 0.0, 1.0));
        place.roughness = float(std::clamp(material.value("roughness").toDouble(.5), 0.0, 1.0));
        frame.models << place;
    }
    for (const int index : lightLayers(scene)) {
        const Layer &layer = scene.layers.at(index);
        if (!layer.visible)
            continue;
        const QJsonObject settings = layer.raw.value("light").toObject();
        const QColor colour(settings.value("color").toString("#fff2dd"));
        const float power = float(std::max(0.0, settings.value("power").toDouble(1)) * std::clamp(scene.value(layer, "opacity", t), 0.0, 1.0));
        if (power <= 0)
            continue;
        Light light;
        light.layer = index;
        light.position = lightPosition(scene, index, t);
        light.colour = QVector3D(float(colour.redF()), float(colour.greenF()), float(colour.blueF())) * power;
        light.reach = float(std::max(.1, settings.value("reach").toDouble(10)));
        frame.lights << light;
    }
    for (const int index : layers(scene)) {
        const Layer &layer = scene.layers.at(index);
        const QVector3D centre = pivot(scene, assets, index);
        // Where it touched the ground and when, for the dust it throws up.
        auto placedAt = [&](double when) {
            return centre + QVector3D(scene.value(layer, "worldX", when), scene.value(layer, "worldY", when),
                                      -scene.value(layer, "worldZ", when));
        };
        for (const QString &kind : {QString("landings"), QString("takeoffs")})
            for (const QJsonValue &value : layer.raw.value(kind).toArray()) {
                const double when = value.toDouble();
                const QVector3D here = placedAt(when);
                const double ground = groundAt(scene, assets, here.x(), here.z());
                if (std::isnan(ground))
                    continue;
                QVector3D going = placedAt(when + .05) - placedAt(when - .05);
                going.setY(0);
                Burst burst;
                burst.position = QVector3D(here.x(), float(ground), here.z());
                burst.away = going.length() > 1e-4 ? -going.normalized() : QVector3D();
                burst.time = when;
                burst.strength = kind == "landings" ? 1 : .5f;
                frame.bursts.append(burst);
            }
        Placement place;
        place.layer = index;
        place.visible = layer.visible;
        const QJsonObject material = layer.raw.value("material").toObject();
        place.shine = float(std::clamp(material.value("shine").toDouble(0), 0.0, 1.0));
        place.roughness = float(std::clamp(material.value("roughness").toDouble(.5), 0.0, 1.0));
        const QJsonObject warp = layer.raw.value("warp").toObject();
        const QSizeF own = assets.size(layer.source);
        if (warp.value("amount").toDouble() > 0 && !own.isEmpty()) {
            // As the flat drawing does: in the picture's pixels, looping.
            const double scale = std::max(1.0, warp.value("scale").toDouble(80)), speed = warp.value("speed").toDouble(.3);
            const double turnAround = 2 * M_PI * t / std::max(.01, scene.duration), radius = speed * std::max(.01, scene.duration) / (2 * M_PI);
            place.warpAmount = float(warp.value("amount").toDouble() / own.width());
            place.warpScale = QVector2D(float(own.width() / scale), float(own.height() / scale));
            place.warpDrift = QVector2D(float(std::cos(turnAround) * radius), float(std::sin(turnAround) * radius));
        }
        place.opacity = float(std::clamp(scene.value(layer, "opacity", t), 0.0, 1.0));
        // Sizes in a world come from distances: a layer's 2D scale (how its
        // picture fits the canvas, or a flat animation) doesn't apply.
        place.scale = 1;
        place.rotation = turn(scene.value(layer, "yaw", t), scene.value(layer, "pitch", t), scene.value(layer, "roll", t));
        const QVector3D move(scene.value(layer, "worldX", t), scene.value(layer, "worldY", t), -scene.value(layer, "worldZ", t));
        // Turned and scaled about its pivot, then moved.
        place.position = centre + move - place.rotation.rotatedVector(centre * place.scale);
        if (index != root) {
            const double farther = (centre + move - frame.cameraPosition).length() - (centre - toWorld.map(QVector3D(0, 0, 0))).length();
            place.haze = float(farther > 0 ? 1 - std::exp(-world.hazeDensity * farther) : 0);
            place.wrap = wrapColour(scene, assets, index);
        }
        frame.layers.append(place);

        // Shaped pieces stand on the ground: a shadow under them.
        if (layer.raw.value("shape").toObject().isEmpty() || !place.visible || place.opacity <= 0)
            continue;
        const Mesh body = mesh(scene, assets, index);
        if (body.isNull())
            continue;
        const QVector3D at = centre + move;
        // Where it stands in the picture, its own bottom is the ground: the
        // depth read behind a cut-out is what was filled in there, which
        // can be far off.
        auto floorAt = [&](double x, double z) {
            double floor = groundAt(scene, assets, x, z);
            if (x >= body.minimum.x() && x <= body.maximum.x() && z >= body.minimum.z() && z <= body.maximum.z())
                floor = std::isnan(floor) ? body.minimum.y() : std::max(floor, double(body.minimum.y()));
            return floor;
        };
        const double ground = floorAt(at.x(), at.z());
        if (std::isnan(ground))
            continue;
        const double lowest = at.y() - (centre.y() - body.minimum.y());
        const double height = std::max(0.0, lowest - ground);
        Shadow shadow;
        shadow.position = QVector3D(at.x(), float(ground + .02), at.z());
        const float spread = float(1 + .35 * height);
        shadow.width = (body.maximum.x() - body.minimum.x()) * spread;
        shadow.length = (body.maximum.z() - body.minimum.z()) * spread;
        shadow.yaw = float(scene.value(layer, "yaw", t));
        shadow.opacity = float(.75 * place.opacity / (1 + 1.2 * height * height));
        frame.shadows.append(shadow);

        // Each added light above it throws its shadow away from it along
        // the ground: as far as the light is low, as dark as it's bright
        // there, and longer the more it slants.
        const double tall = body.maximum.y() - body.minimum.y();
        const double middle = height + tall / 2;
        for (const Light &light : frame.lights) {
            const double above = light.position.y() - ground;
            if (above <= middle * 1.05)
                continue;
            const QVector2D away(at.x() - light.position.x(), at.z() - light.position.z());
            const double along = away.length();
            const double reach = along * middle / (above - middle);
            const double brightness = (light.colour.x() + light.colour.y() + light.colour.z()) / 3;
            const double distance = (light.position - at).length() / light.reach;
            const double strength = std::min(.7, .55 * brightness / (1 + distance * distance)) * place.opacity;
            if (strength < .02)
                continue;
            const QVector2D way = along > 1e-4 ? away / float(along) : QVector2D(0, 1);
            const double footprint = std::max(body.maximum.x() - body.minimum.x(), body.maximum.z() - body.minimum.z());
            // Its middle where the light through its middle meets the
            // ground, no farther than a few times its size.
            const double shift = std::min(reach, 4.0 * footprint + tall);
            Shadow cast;
            const double x = at.x() + way.x() * shift, z = at.z() + way.y() * shift;
            const double there = floorAt(x, z);
            cast.position = QVector3D(float(x), float((std::isnan(there) ? ground : there) + .021), float(z));
            cast.width = float(footprint);
            cast.length = float(footprint + std::min(2 * shift, tall * along / std::max(.1, above - middle)));
            cast.yaw = float(qRadiansToDegrees(std::atan2(way.x(), -way.y())));
            cast.opacity = float(strength / (1 + .3 * height));
            frame.shadows.append(cast);
        }
    }
    // The dust: each burst throws up puffs, every one worked out from its
    // own seed and the time since, so any frame can be drawn on its own.
    // They're kicked back from the way the piece went, slowed by the air,
    // rise as they spread, and fade.
    const QJsonObject look = scene.raw.value("world").toObject();
    if (QColor(look.value("dust").toString()).isValid())
        frame.dust = QColor(look.value("dust").toString());
    for (int b = 0; b < frame.bursts.size(); ++b) {
        const Burst &burst = frame.bursts.at(b);
        const int count = int(std::lround(70 * burst.strength));
        for (int i = 0; i < count; ++i) {
            quint32 state = quint32(b * 7919 + i * 104729 + 17);
            auto random = [&state] {
                state = state * 1664525u + 1013904223u;
                return (state >> 8) / double(1 << 24);
            };
            const double delay = random() * .25, life = 1.8 + random() * 1.4;
            const double age = t - burst.time - delay;
            const double r1 = random(), r2 = random(), r3 = random(), r4 = random(), r5 = random(), r6 = random();
            if (age < 0 || age > life)
                continue;
            const double angle = r1 * 2 * M_PI, reach = std::sqrt(r2) * .9;
            const QVector3D start = burst.position + QVector3D(float(std::cos(angle) * reach), .05f, float(std::sin(angle) * reach));
            const QVector3D sideways(float((r3 - .5) * 2.4), 0, float((r4 - .5) * 2.4));
            const QVector3D kick = burst.away * float(.8 + 2 * r5) + sideways + QVector3D(0, float(.4 + 1.4 * r6), 0);
            const double drag = 1.8, travelled = (1 - std::exp(-drag * age)) / drag;
            Puff puff;
            puff.position = start + kick * float(travelled) + QVector3D(0, float(.15 * age), 0);
            const double grown = age / life;
            puff.size = float((.35 + .3 * r1) * (1 + 2.2 * grown));
            puff.alpha = float(std::min(1.0, age / .12) * std::pow(1 - grown, 1.3) * .5 * burst.strength);
            frame.puffs.append(puff);
        }
    }
    // Particles thrown from its pieces, drawn among them, so what's nearer
    // hides them.
    for (const Layer &layer : scene.layers) {
        if (!layer.visible || !throwsIntoWorld(scene, layer))
            continue;
        const double opacity = std::clamp(scene.value(layer, "opacity", t), 0.0, 1.0);
        for (const WorldSpark &spark : sparksInWorld(scene, assets, layer, t)) {
            Puff puff;
            puff.position = spark.position;
            puff.size = spark.radius * 2;
            puff.alpha = float(spark.colour.alphaF() * opacity);
            puff.colour = spark.colour;
            if (puff.alpha > .004f)
                frame.puffs.append(puff);
        }
    }
    return frame;
}

bool fluidInWorld(const Scene &scene, const Layer &layer) {
    if (layer.particles.isEmpty() || !info(scene).valid)
        return false;
    const QString kind = layer.particles.value("kind").toString();
    if (kind != "smoke" && kind != "fire" && kind != "water")
        return false;
    const int source = scene.indexOf(layer.particles.value("from").toString());
    return source >= 0 && layers(scene).contains(source) && source != scene.indexOf(info(scene).picture);
}

QList<Sheet> sheets(const Scene &scene, AssetCache &assets, double t) {
    QList<Sheet> list;
    const Info world = info(scene);
    for (int index = 0; index < scene.layers.size(); ++index) {
        const Layer &layer = scene.layers.at(index);
        if (!layer.visible || !fluidInWorld(scene, layer))
            continue;
        const int source = scene.indexOf(layer.particles.value("from").toString());
        const Layer &from = scene.layers.at(source);
        // The piece's point in the world at a time: its pivot, or "at" on
        // its picture, as far off the pivot as that is at its distance.
        const QVector3D centre = pivot(scene, assets, source);
        const QSizeF picture = assets.size(from.source);
        const QJsonArray at = layer.particles.value("at").toArray();
        const QPointF anchor(from.anchor.x() * picture.width(), from.anchor.y() * picture.height());
        const QPointF off = at.size() == 2 ? QPointF(at.at(0).toDouble(), at.at(1).toDouble()) - anchor : QPointF();
        const double metersAPixel = (centre - pictureToWorld(world).map(QVector3D())).length() / std::max(1.0, world.focal);
        auto pointAt = [&](double when) {
            const QVector3D move(float(scene.value(from, "worldX", when)), float(scene.value(from, "worldY", when)), float(-scene.value(from, "worldZ", when)));
            return centre + move +
                   turn(scene.value(from, "yaw", when), scene.value(from, "pitch", when), scene.value(from, "roll", when))
                       .rotatedVector(QVector3D(float(off.x() * metersAPixel), float(-off.y() * metersAPixel), 0));
        };
        // The sheet: upright, square to the camera, round the point; in
        // centimeters, the source low in its middle. The source moves over
        // it as the point moves, as far as a third of the sheet each way;
        // past that the sheet goes along with it.
        const View now = cameraAt(scene, assets, t);
        QVector3D facing = now.position - pointAt(t);
        facing.setY(0);
        facing = facing.length() > 1e-4f ? facing.normalized() : QVector3D(0, 0, 1);
        const QVector3D right = QVector3D::crossProduct(QVector3D(0, 1, 0), facing).normalized();
        const QString kind = layer.particles.value("kind").toString();
        const double size = std::max(2.0, layer.particles.value("size").toDouble(kind == "water" ? 4 : 30));
        const double wide = std::max(kind == "water" ? 600.0 : 300.0, size * 16), tall = wide * (kind == "water" ? .6 : 1.6);
        const QPointF start(wide / 2, tall * (kind == "water" ? .25 : .85));
        auto overSheet = [&](double when) {
            const QVector3D moved = pointAt(std::min(when, scene.duration)) - pointAt(0);
            return QPointF(std::clamp(double(QVector3D::dotProduct(moved, right)) * 100, -wide / 3, wide / 3),
                           std::clamp(-double(moved.y()) * 100, -start.y() * .8, (tall - start.y()) * .8));
        };
        // The fluid's own little scene: the sheet as its canvas, a mark
        // keyed to where the source is on it, each frame.
        Scene own;
        own.width = int(std::ceil(wide));
        own.height = int(std::ceil(tall));
        own.fps = scene.fps;
        own.duration = scene.duration;
        Layer mark;
        mark.name = "source";
        mark.raw["empty"] = true;
        const double step = 1 / std::max(1.0, scene.fps);
        QList<Key> xs, ys;
        for (double when = 0; when <= 2 * scene.duration + step; when += step) {
            const QPointF over = overSheet(when);
            xs << Key{when, start.x() + over.x(), "linear"};
            ys << Key{when, start.y() + over.y(), "linear"};
        }
        mark.keys["x"] = xs;
        mark.keys["y"] = ys;
        own.layers << mark;
        Layer fluid = layer;
        fluid.name = layer.name;
        fluid.particles["from"] = "source";
        fluid.particles.remove("at");
        if (kind == "water") {
            // Its floor is the ground under the point.
            const double ground = groundAt(scene, assets, pointAt(t).x(), pointAt(t).z());
            if (!std::isnan(ground))
                fluid.particles["floor"] = std::round(start.y() + overSheet(t).y() + (pointAt(t).y() - ground) * 100);
        }
        own.layers << fluid;
        Sheet sheet;
        sheet.layer = index;
        QRectF covers;
        sheet.image = kind == "water" ? Liquid::at(own, assets, own.layers.last(), t, .5, &covers)
                                      : Fluid::at(own, assets, own.layers.last(), t, &covers);
        if (sheet.image.isNull())
            continue;
        sheet.size = QSizeF(covers.width() / 100, covers.height() / 100);
        sheet.rotation = QQuaternion::fromAxes(right, QVector3D(0, 1, 0), facing);
        // Its middle: from the point now, back by where the source is on
        // it, to the sheet's middle.
        const QPointF spot = start + overSheet(t);
        sheet.centre = pointAt(t) + right * float((covers.center().x() - spot.x()) / 100) +
                       QVector3D(0, float((spot.y() - covers.center().y()) / 100), 0);
        sheet.opacity = float(std::clamp(scene.value(layer, "opacity", t), 0.0, 1.0));
        list << sheet;
    }
    return list;
}

QList<int> lightLayers(const Scene &scene) {
    QList<int> list;
    for (int i = 0; i < scene.layers.size(); ++i)
        if (scene.layers.at(i).raw.contains("light"))
            list << i;
    return list;
}

QList<int> modelLayers(const Scene &scene) {
    QList<int> list;
    for (int i = 0; i < scene.layers.size(); ++i)
        if (scene.layers.at(i).raw.contains("model"))
            list << i;
    return list;
}

bool modelBounds(const QString &path, QVector3D *low, QVector3D *high) {
    static QHash<QString, QPair<QVector3D, QVector3D>> cache;
    const QString key = path + "|" + QString::number(QFileInfo(path).lastModified().toMSecsSinceEpoch());
    {
        QMutexLocker lock(&cacheLock);
        if (cache.contains(key)) {
            *low = cache.value(key).first;
            *high = cache.value(key).second;
            return true;
        }
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    QVector3D a(1e30f, 1e30f, 1e30f), b(-1e30f, -1e30f, -1e30f);
    auto grow = [&](const QVector3D &p) {
        for (int k = 0; k < 3; ++k)
            a[k] = std::min(a[k], p[k]), b[k] = std::max(b[k], p[k]);
    };
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == "obj") {
        while (!file.atEnd()) {
            const QByteArray line = file.readLine().trimmed();
            if (!line.startsWith("v "))
                continue;
            const QList<QByteArray> parts = line.simplified().split(' ');
            if (parts.size() >= 4)
                grow(QVector3D(parts.at(1).toFloat(), parts.at(2).toFloat(), parts.at(3).toFloat()));
        }
    } else {
        // glTF: each mesh's position box, through the nodes above it.
        QByteArray text = file.readAll();
        if (suffix == "glb") {
            if (text.size() < 20 || !text.startsWith("glTF"))
                return false;
            const quint32 length = *reinterpret_cast<const quint32 *>(text.constData() + 12);
            text = text.mid(20, length);
        }
        const QJsonObject gltf = QJsonDocument::fromJson(text).object();
        const QJsonArray nodes = gltf.value("nodes").toArray(), meshes = gltf.value("meshes").toArray(),
                         accessors = gltf.value("accessors").toArray();
        std::function<void(int, const QMatrix4x4 &)> visit = [&](int index, const QMatrix4x4 &above) {
            const QJsonObject node = nodes.at(index).toObject();
            QMatrix4x4 local;
            const QJsonArray m = node.value("matrix").toArray();
            if (m.size() == 16) {
                float values[16];
                for (int k = 0; k < 16; ++k)
                    values[k] = float(m.at(k).toDouble());
                local = QMatrix4x4(values).transposed();  // glTF's are column by column.
            } else {
                const QJsonArray tr = node.value("translation").toArray(), r = node.value("rotation").toArray(), sc = node.value("scale").toArray();
                if (tr.size() == 3)
                    local.translate(float(tr.at(0).toDouble()), float(tr.at(1).toDouble()), float(tr.at(2).toDouble()));
                if (r.size() == 4)
                    local.rotate(QQuaternion(float(r.at(3).toDouble()), float(r.at(0).toDouble()), float(r.at(1).toDouble()), float(r.at(2).toDouble())));
                if (sc.size() == 3)
                    local.scale(float(sc.at(0).toDouble()), float(sc.at(1).toDouble()), float(sc.at(2).toDouble()));
            }
            const QMatrix4x4 world = above * local;
            if (node.contains("mesh"))
                for (const QJsonValue &primitive : meshes.at(node.value("mesh").toInt()).toObject().value("primitives").toArray()) {
                    const QJsonObject accessor = accessors.at(primitive.toObject().value("attributes").toObject().value("POSITION").toInt(-1)).toObject();
                    const QJsonArray mn = accessor.value("min").toArray(), mx = accessor.value("max").toArray();
                    if (mn.size() != 3 || mx.size() != 3)
                        continue;
                    for (int c = 0; c < 8; ++c)
                        grow(world.map(QVector3D(float((c & 1 ? mx : mn).at(0).toDouble()), float((c & 2 ? mx : mn).at(1).toDouble()),
                                                 float((c & 4 ? mx : mn).at(2).toDouble()))));
                }
            for (const QJsonValue &child : node.value("children").toArray())
                visit(child.toInt(), world);
        };
        const QJsonArray scenes = gltf.value("scenes").toArray();
        const QJsonArray roots = scenes.at(gltf.value("scene").toInt(0)).toObject().value("nodes").toArray();
        for (const QJsonValue &root : roots)
            visit(root.toInt(), QMatrix4x4());
    }
    if (a.x() > b.x())
        return false;
    *low = a;
    *high = b;
    QMutexLocker lock(&cacheLock);
    cache.insert(key, qMakePair(a, b));
    return true;
}

QVector3D lightPosition(const Scene &scene, int index, double t) {
    const Layer &layer = scene.layers.at(index);
    return pictureToWorld(info(scene)).map(QVector3D(0, 0, 0)) +
           QVector3D(float(scene.value(layer, "worldX", t)), float(scene.value(layer, "worldY", t)), float(-scene.value(layer, "worldZ", t)));
}

View cameraView(const Frame &frame) { return View{frame.cameraPosition, frame.cameraRotation, frame.fov}; }

bool project(const View &view, const QSizeF &size, const QVector3D &point, QPointF *onScreen) {
    const QVector3D v = view.rotation.inverted().rotatedVector(point - view.position);
    if (v.z() > -Near)
        return false;
    const double k = size.height() / 2.0 / std::tan(view.fov * M_PI / 360);
    *onScreen = QPointF(size.width() / 2.0 + k * v.x() / -v.z(), size.height() / 2.0 - k * v.y() / -v.z());
    return true;
}

void ray(const View &view, const QSizeF &size, const QPointF &onScreen, QVector3D *origin, QVector3D *direction) {
    const double k = size.height() / 2.0 / std::tan(view.fov * M_PI / 360);
    const QVector3D local(float((onScreen.x() - size.width() / 2.0) / k), float(-(onScreen.y() - size.height() / 2.0) / k), -1);
    *origin = view.position;
    *direction = view.rotation.rotatedVector(local).normalized();
}

namespace {
// Möller–Trumbore: how far along the ray it meets the triangle, and where
// in it (u, v toward b and c); false if it misses.
bool hitTriangle(const QVector3D &origin, const QVector3D &direction, const QVector3D &a, const QVector3D &b,
                 const QVector3D &c, float *distance, float *u, float *v) {
    const QVector3D ab = b - a, ac = c - a, p = QVector3D::crossProduct(direction, ac);
    const float det = QVector3D::dotProduct(ab, p);
    if (std::abs(det) < 1e-12f)
        return false;
    const QVector3D s = origin - a;
    *u = QVector3D::dotProduct(s, p) / det;
    if (*u < 0 || *u > 1)
        return false;
    const QVector3D q = QVector3D::crossProduct(s, ab);
    *v = QVector3D::dotProduct(direction, q) / det;
    if (*v < 0 || *u + *v > 1)
        return false;
    *distance = QVector3D::dotProduct(ac, q) / det;
    return *distance > Near;
}
}

int pick(const Scene &scene, AssetCache &assets, double t, const View &view, const QSizeF &size, const QPointF &onScreen) {
    QVector3D origin, direction;
    ray(view, size, onScreen, &origin, &direction);
    const Frame now = frame(scene, assets, t);
    int found = -1;
    float nearest = std::numeric_limits<float>::max();
    // Models by their boxes: the ray brought into a model's own units.
    for (const Placement &model : now.models) {
        QVector3D low, high;
        if (!model.visible || model.opacity <= 0 || !modelBounds(model.model, &low, &high))
            continue;
        const QQuaternion back = model.rotation.inverted();
        const QVector3D o = back.rotatedVector(origin - model.position) / model.scale - model.offset;
        const QVector3D d = back.rotatedVector(direction) / model.scale;
        float enter = -1e30f, leave = 1e30f;
        bool miss = false;
        for (int k = 0; k < 3 && !miss; ++k) {
            if (std::abs(d[k]) < 1e-12f) {
                miss = o[k] < low[k] || o[k] > high[k];
                continue;
            }
            float a = (low[k] - o[k]) / d[k], b = (high[k] - o[k]) / d[k];
            if (a > b)
                std::swap(a, b);
            enter = std::max(enter, a), leave = std::min(leave, b);
        }
        if (miss || enter > leave || leave < 0)
            continue;
        const float distance = std::max(0.f, enter);
        if (distance < nearest)
            nearest = distance, found = model.layer;
    }
    for (const Placement &place : now.layers) {
        if (!place.visible || place.opacity <= 0)
            continue;
        const Mesh shape = mesh(scene, assets, place.layer);
        if (shape.isNull())
            continue;
        // The ray brought into the mesh's own space, where it rests.
        const QQuaternion back = place.rotation.inverted();
        const QVector3D o = back.rotatedVector(origin - place.position) / place.scale,
                        d = back.rotatedVector(direction).normalized();
        const float *vertex = reinterpret_cast<const float *>(shape.vertices.constData());
        const quint32 *index = reinterpret_cast<const quint32 *>(shape.indices.constData());
        const qsizetype triangles = shape.indices.size() / qsizetype(3 * sizeof(quint32));
        // A flat piece is a grid over its whole rectangle: what's see-through
        // in its picture doesn't count. A shape is solid.
        const bool solid = shape.columns == 0;
        double factor = 1;
        const QImage picture = solid ? QImage() : assets.mip(scene.layers.at(place.layer).source, .25, &factor);
        auto at = [&](quint32 k) { return QVector3D(vertex[k * shape.stride], vertex[k * shape.stride + 1], vertex[k * shape.stride + 2]); };
        for (qsizetype i = 0; i < triangles; ++i) {
            const quint32 a = index[3 * i], b = index[3 * i + 1], c = index[3 * i + 2];
            float distance, u, v;
            if (!hitTriangle(o, d, at(a), at(b), at(c), &distance, &u, &v))
                continue;
            distance *= place.scale;
            if (distance >= nearest)
                continue;
            if (!solid && !picture.isNull()) {
                auto uv = [&](quint32 k) { return QPointF(vertex[k * shape.stride + 3], vertex[k * shape.stride + 4]); };
                const QPointF texel = uv(a) * (1 - u - v) + uv(b) * u + uv(c) * v;
                const QPoint pixel(std::clamp(int(texel.x() * picture.width()), 0, picture.width() - 1),
                                   std::clamp(int((1 - texel.y()) * picture.height()), 0, picture.height() - 1));
                if (qAlpha(picture.pixel(pixel)) <= 16)
                    continue;
            }
            nearest = distance;
            found = place.layer;
        }
    }
    return found;
}

bool box(const Scene &scene, AssetCache &assets, int index, double t, QList<QVector3D> *corners, QVector3D *pivot) {
    const Frame now = frame(scene, assets, t);
    for (const Placement &model : now.models) {
        if (model.layer != index)
            continue;
        QVector3D low, high;
        if (!modelBounds(model.model, &low, &high))
            return false;
        corners->clear();
        for (int k = 0; k < 8; ++k) {
            const QVector3D own(k & 1 ? high.x() : low.x(), k & 2 ? high.y() : low.y(), k & 4 ? high.z() : low.z());
            corners->append(model.position + model.rotation.rotatedVector((own + model.offset) * model.scale));
        }
        *pivot = model.position;
        return true;
    }
    for (const Placement &place : now.layers) {
        if (place.layer != index)
            continue;
        const Mesh shape = mesh(scene, assets, index);
        if (shape.isNull())
            return false;
        // The background reaches out to the sky; its box is kept to a
        // hundred meters so it stays something to look at.
        QVector3D low = shape.minimum, high = shape.maximum;
        const QVector3D reach(100, 100, 100);
        low = QVector3D(std::max(low.x(), shape.pivot.x() - reach.x()), std::max(low.y(), shape.pivot.y() - reach.y()),
                        std::max(low.z(), shape.pivot.z() - reach.z()));
        high = QVector3D(std::min(high.x(), shape.pivot.x() + reach.x()), std::min(high.y(), shape.pivot.y() + reach.y()),
                         std::min(high.z(), shape.pivot.z() + reach.z()));
        corners->clear();
        for (int k = 0; k < 8; ++k) {
            const QVector3D rest(k & 1 ? high.x() : low.x(), k & 2 ? high.y() : low.y(), k & 4 ? high.z() : low.z());
            corners->append(place.position + place.rotation.rotatedVector(rest * place.scale));
        }
        *pivot = place.position + place.rotation.rotatedVector(shape.pivot * place.scale);
        return true;
    }
    return false;
}

double motion(const Scene &scene, AssetCache &assets, double from, double to, const QSize &size) {
    const Frame a = frame(scene, assets, from), b = frame(scene, assets, to);
    if (a.layers.size() != b.layers.size() || size.isEmpty())
        return 0;
    // Points well off screen at both ends don't show, however far they go.
    const QRectF screen = QRectF(QPointF(), QSizeF(size)).adjusted(-size.width() * .1, -size.height() * .1,
                                                                  size.width() * .1, size.height() * .1);
    double most = 0;
    for (int i = 0; i < a.layers.size(); ++i) {
        const Placement &p = a.layers.at(i), &q = b.layers.at(i);
        if ((!p.visible || p.opacity <= 0) && (!q.visible || q.opacity <= 0))
            continue;
        const Mesh shape = mesh(scene, assets, p.layer);
        if (shape.isNull())
            continue;
        const float *vertex = reinterpret_cast<const float *>(shape.vertices.constData());
        const qsizetype count = shape.vertices.size() / qsizetype(shape.stride * sizeof(float));
        // A few hundred of its points tell well enough how far it goes.
        const qsizetype step = std::max<qsizetype>(1, count / 400);
        for (qsizetype k = 0; k < count; k += step) {
            const QVector3D rest(vertex[k * shape.stride], vertex[k * shape.stride + 1], vertex[k * shape.stride + 2]);
            QPointF start, end;
            if (!project(cameraView(a), size, p.position + p.rotation.rotatedVector(rest * p.scale), &start) ||
                !project(cameraView(b), size, q.position + q.rotation.rotatedVector(rest * q.scale), &end))
                continue;
            if (screen.contains(start) || screen.contains(end))
                most = std::max(most, QLineF(start, end).length());
        }
    }
    return most;
}

namespace {

// How steeply unseen ground behind a crest is taken to fall away.
constexpr double Fall = .08;

// The ground as heights on a grid over (x, ahead), half a meter a cell.
struct Heightfield {
    double x0 = -80, ahead0 = -20, cell = .5;
    int w = 320, h = 340;
    std::vector<float> height;
};

const Heightfield &heightfield(const Scene &scene, AssetCache &assets) {
    static QHash<QByteArray, Heightfield> cache;
    QMutexLocker lock(&cacheLock);
    const QByteArray key = QJsonDocument(scene.raw.value("world").toObject()).toJson(QJsonDocument::Compact) +
                           assets.directory().toUtf8();
    if (cache.contains(key))
        return cache[key];
    Heightfield field;
    const Info world = info(scene);
    const int root = scene.indexOf(world.picture);
    const Mesh ground = mesh(scene, assets, root);
    std::vector<float> top(size_t(field.w) * field.h, -1e9f);
    const float *v = reinterpret_cast<const float *>(ground.vertices.constData());
    const size_t count = size_t(ground.vertices.size()) / (5 * sizeof(float));
    // What's seen of the ground: from the bottom of the picture up, the
    // surfaces that face up and carry on without a jump in distance (the
    // road and its verges up to a crest's lip; not hills beyond, nor the
    // face of a dust cloud), the highest in each cell.
    const int columns = std::max(2, ground.columns);
    const int rowsOf = int(count / columns);
    auto vertex = [&](size_t i) { return QVector3D(v[i * 5], v[i * 5 + 1], v[i * 5 + 2]); };
    const QVector3D eyeAt = pictureToWorld(world).map(QVector3D(0, 0, 0));
    std::vector<uchar> isGround(count, 0), reached(count, 0);
    for (size_t i = 0; i < count; ++i) {
        const QVector3D p = vertex(i);
        if (p.length() > 1000)
            continue;
        const size_t right = (i % columns) + 1 < size_t(columns) ? i + 1 : i - 1, below = i + columns < count ? i + columns : i - columns;
        QVector3D normal = QVector3D::crossProduct(vertex(right) - p, vertex(below) - p).normalized();
        if (normal.y() < 0)
            normal = -normal;
        isGround[i] = normal.y() > .5f;
    }
    // From the lowest ground in each column, in the bottom quarter: the
    // very bottom rows, where the mesh meets its edge, can be too steep.
    QList<size_t> queue;
    for (int c = 0; c < columns; ++c)
        for (int r = rowsOf - 1; r >= rowsOf * 3 / 4; --r) {
            const size_t i = size_t(r) * columns + c;
            if (isGround[i]) {
                reached[i] = 1;
                queue.append(i);
                break;
            }
        }
    while (!queue.isEmpty()) {
        const size_t i = queue.takeLast();
        const double here = (vertex(i) - eyeAt).length();
        const int r = int(i / columns), c = int(i % columns);
        for (const auto [dr, dc] : {std::pair{-1, 0}, std::pair{1, 0}, std::pair{0, -1}, std::pair{0, 1}}) {
            if (r + dr < 0 || r + dr >= rowsOf || c + dc < 0 || c + dc >= columns)
                continue;
            const size_t j = size_t(r + dr) * columns + c + dc;
            if (reached[j] || !isGround[j])
                continue;
            const double there = (vertex(j) - eyeAt).length();
            if (std::abs(there - here) / std::min(here, there) > .12)
                continue;
            reached[j] = 1;
            queue.append(j);
        }
    }
    for (size_t i = 0; i < count; ++i) {
        if (!reached[i])
            continue;
        const QVector3D p = vertex(i);
        const int cx = int((p.x() - field.x0) / field.cell), cz = int((-p.z() - field.ahead0) / field.cell);
        if (cx >= 0 && cx < field.w && cz >= 0 && cz < field.h)
            top[size_t(cz) * field.w + cx] = std::max(top[size_t(cz) * field.w + cx], p.y());
    }
    // The gaps between, filled from around (push-pull, as for depth).
    QImage fakeDepth(field.w, field.h, QImage::Format_Grayscale16), hole(field.w, field.h, QImage::Format_Grayscale8);
    const double base = 500;  // Heights carried as distances above a floor, so they stay positive.
    for (int z = 0; z < field.h; ++z)
        for (int x = 0; x < field.w; ++x) {
            const float t = top[size_t(z) * field.w + x];
            reinterpret_cast<quint16 *>(fakeDepth.scanLine(z))[x] = encodeDepth(t > -1e8f ? base + t : base);
            hole.scanLine(z)[x] = t > -1e8f ? 0 : 255;
        }
    fillDepth(fakeDepth, hole);
    field.height.resize(size_t(field.w) * field.h);
    for (int z = 0; z < field.h; ++z)
        for (int x = 0; x < field.w; ++x)
            field.height[size_t(z) * field.w + x] = float(decodeDepth(reinterpret_cast<const quint16 *>(fakeDepth.constScanLine(z))[x]) - base);
    // Ground nobody sees lies below the line of sight past what hides it:
    // walking out from the camera, it stays under the steepest view so far.
    const double eye = world.height * world.scale;
    for (int z = 0; z < field.h; ++z)
        for (int x = 0; x < field.w; ++x) {
            if (!hole.constScanLine(z)[x])
                continue;
            const double px = field.x0 + (x + .5) * field.cell, pz = field.ahead0 + (z + .5) * field.cell;
            const double distance = std::hypot(px, pz);
            double steepest = -1e9, lastSeen = -1;
            for (double d = field.cell; d < distance; d += field.cell) {
                const int sx = int((px * d / distance - field.x0) / field.cell), sz = int((pz * d / distance - field.ahead0) / field.cell);
                if (sx < 0 || sx >= field.w || sz < 0 || sz >= field.h || hole.constScanLine(sz)[sx])
                    continue;
                steepest = std::max(steepest, (top[size_t(sz) * field.w + sx] - eye) / d);
                lastSeen = d;
            }
            if (steepest > -1e8) {
                // Behind what hides it, the ground falls away gently (as a
                // road past a crest), and always under the line of sight.
                float &here = field.height[size_t(z) * field.w + x];
                const double fallen = double(here) - Fall * std::max(0.0, distance - lastSeen);
                here = float(std::min(fallen, eye + steepest * distance));
            }
        }
    return cache[key] = field;
}

}

double groundAt(const Scene &scene, AssetCache &assets, double x, double z) {
    if (!info(scene).valid)
        return std::numeric_limits<double>::quiet_NaN();
    const Heightfield &field = heightfield(scene, assets);
    const double fx = (x - field.x0) / field.cell - .5, fz = (-z - field.ahead0) / field.cell - .5;
    if (fx < 0 || fz < 0 || fx >= field.w - 1 || fz >= field.h - 1)
        return std::numeric_limits<double>::quiet_NaN();
    const int ix = int(fx), iz = int(fz);
    const double tx = fx - ix, tz = fz - iz;
    auto at = [&](int a, int b) { return double(field.height[size_t(b) * field.w + a]); };
    return (at(ix, iz) * (1 - tx) + at(ix + 1, iz) * tx) * (1 - tz) + (at(ix, iz + 1) * (1 - tx) + at(ix + 1, iz + 1) * tx) * tz;
}

double heading(const Scene &scene, AssetCache &assets, int index) {
    const Info world = info(scene);
    const QVector3D pivot = World::pivot(scene, assets, index);
    const QVector3D camera = pictureToWorld(world).map(QVector3D(0, 0, 0));
    QVector3D toCamera = camera - pivot;
    toCamera.setY(0);
    QVector3D facing = toCamera.normalized();
    const Mesh shape = mesh(scene, assets, index);
    if (!scene.layers.at(index).raw.value("shape").toObject().isEmpty() && !shape.isNull()) {
        // The long axis of its footprint: the main direction its points
        // spread in across the ground.
        const float *v = reinterpret_cast<const float *>(shape.vertices.constData());
        const int k = shape.stride;
        const size_t count = size_t(shape.vertices.size()) / (k * sizeof(float));
        double sxx = 0, szz = 0, sxz = 0, mx = 0, mz = 0;
        for (size_t i = 0; i < count; ++i)
            mx += v[i * k], mz += v[i * k + 2];
        mx /= count, mz /= count;
        for (size_t i = 0; i < count; ++i) {
            const double dx = v[i * k] - mx, dz = v[i * k + 2] - mz;
            sxx += dx * dx, szz += dz * dz, sxz += dx * dz;
        }
        const double angle = .5 * std::atan2(2 * sxz, sxx - szz);
        QVector3D axis(float(std::cos(angle)), 0, float(std::sin(angle)));
        if (QVector3D::dotProduct(axis, toCamera) < 0)
            axis = -axis;
        facing = axis;
    }
    // Yaw 0 looks away from the camera (-z); right is positive.
    return std::atan2(facing.x(), -facing.z()) * 180 / M_PI;
}

bool drive(Scene &scene, AssetCache &assets, int index, const QPointF &from, const QPointF &to, double speed, double t,
           QList<double> *landings, QString *error, bool face) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (!info(scene).valid || !layers(scene).contains(index) || index == scene.indexOf(info(scene).picture))
        return fail("Driving is for pieces of a world.");
    if (!(speed > 0))
        return fail("The speed must be more than 0.");
    const QVector3D rest = World::pivot(scene, assets, index);
    const Mesh shape = mesh(scene, assets, index);
    const double clearance = rest.y() - shape.minimum.y();  // From its lowest point up to its pivot.
    // World points: x right, z negative ahead.
    const QVector3D start(float(from.x()), 0, float(-from.y())), end(float(to.x()), 0, float(-to.y()));
    const double length = (end - start).length();
    if (length < .1)
        return fail("From and to are the same place.");
    const QVector3D direction = (end - start) / float(length);
    // The ground along the way, smoothed over about a wheelbase, as a car
    // rides over the grain of it rather than into every dip.
    const double step = .25;
    const int samples = int(length / step) + 1;
    std::vector<double> ground(samples);
    for (int i = 0; i < samples; ++i) {
        const QVector3D p = start + direction * float(i * step);
        const double g = groundAt(scene, assets, p.x(), p.z());
        if (std::isnan(g))
            return fail(QString("The way leaves the part of the world the picture shows (at %1 m right, %2 m ahead).")
                            .arg(p.x(), 0, 'f', 1).arg(-p.z(), 0, 'f', 1));
        ground[i] = g;
    }
    std::vector<double> smooth(samples);
    for (int i = 0; i < samples; ++i) {
        double sum = 0;
        int n = 0;
        for (int k = std::max(0, i - 6); k <= std::min(samples - 1, i + 6); ++k)
            sum += ground[k], ++n;
        smooth[i] = sum / n + clearance;
    }
    auto groundAlong = [&](double s) {
        const double f = std::clamp(s / step, 0.0, double(samples - 1));
        const int i = std::min(int(f), samples - 2);
        return smooth[i] + (smooth[i + 1] - smooth[i]) * (f - i);
    };

    // Along the way at a steady speed; up and down, held to the ground
    // until the ground falls away faster than gravity pulls, then thrown.
    const double gravity = 9.81, dt = 1 / (scene.fps * 4), duration = length / speed;
    struct Sample {
        double t, s, y, vy;
    };
    std::vector<Sample> path;
    double s = 0, y = groundAlong(0), vy = (groundAlong(speed * dt) - groundAlong(0)) / dt;
    bool flying = false;
    if (landings)
        landings->clear();
    QJsonArray landed, tookOff;
    for (double time = 0; time <= duration + dt / 2; time += dt) {
        path.push_back({time, s, y, vy});
        const double next = s + speed * dt, below = groundAlong(next);
        if (!flying) {
            const double vyNext = (below - y) / dt;
            // Springs soak up a little; past that the ground drops away.
            if ((vyNext - vy) / dt < -1.2 * gravity) {
                flying = true;
                tookOff.append(std::round((t + time) * 1000) / 1000);
            } else {
                y = below, vy = vyNext;
            }
        }
        if (flying) {
            vy -= gravity * dt;
            y += vy * dt;
            if (y <= below) {
                // A hop over a bump isn't a jump: no dust for it.
                if (!tookOff.isEmpty() && t + time - tookOff.last().toDouble() < .15)
                    tookOff.removeLast();
                // Down: whatever speed it had into the ground is spent.
                if (vy < -1) {
                    if (landings)
                        landings->append(t + time + dt);
                    landed.append(std::round((t + time + dt) * 1000) / 1000);
                }
                y = below;
                vy = (groundAlong(next + speed * dt) - below) / dt;
                flying = false;
            }
        }
        s = next;
    }

    // Keyframes a frame apart, replacing what was there over that time.
    Layer &layer = scene.layers[index];
    double yaw = face ? std::atan2(direction.x(), -direction.z()) * 180 / M_PI - heading(scene, assets, index) : 0;
    yaw = std::remainder(yaw, 360.0);
    const double end_ = t + duration;
    for (const QString &property : {QString("worldX"), QString("worldY"), QString("worldZ"), QString("yaw"), QString("pitch")}) {
        QList<Key> kept;
        for (const Key &key : layer.keys.value(property))
            if (key.t < t - 1e-6 || key.t > end_ + 1e-6)
                kept.append(key);
        layer.keys[property] = kept;
    }
    const int frames = int(std::ceil(duration * scene.fps));
    double pitch = 0;
    for (int f = 0; f <= frames; ++f) {
        const double time = std::min(duration, f / scene.fps);
        const Sample &a = path[std::min(path.size() - 1, size_t(time / dt))];
        const QVector3D p = start + direction * float(a.s);
        // The nose follows the path, a little behind, as a car's weight does.
        const double wanted = std::atan2(a.vy, speed) * 180 / M_PI;
        pitch += (wanted - pitch) * std::min(1.0, 8.0 / scene.fps);
        auto put = [&](const QString &property, double v) {
            QList<Key> &keys = layer.keys[property];
            keys.append(Key{t + time, v, "linear"});
        };
        put("worldX", p.x() - rest.x());
        put("worldY", a.y - rest.y());
        put("worldZ", -(p.z() - rest.z()));
        put("yaw", yaw);
        put("pitch", pitch);
    }
    for (const QString &property : {QString("worldX"), QString("worldY"), QString("worldZ"), QString("yaw"), QString("pitch")})
        std::stable_sort(layer.keys[property].begin(), layer.keys[property].end(), [](const Key &a, const Key &b) { return a.t < b.t; });
    layer.raw["landings"] = landed;
    layer.raw["takeoffs"] = tookOff;
    if (end_ > scene.duration)
        scene.duration = std::ceil(end_ * 10) / 10;
    return true;
}

bool setSize(Scene &scene, AssetCache &assets, int index, double meters, QString *error) {
    const Info world = info(scene);
    if (!world.valid || !layers(scene).contains(index))
        return (error ? *error = "Sizing is for pieces of a world." : QString()), false;
    if (!(meters > 0))
        return (error ? *error = "The size must be more than 0 meters." : QString()), false;
    const Mesh shape = mesh(scene, assets, index);
    const double tall = shape.maximum.y() - shape.minimum.y();
    if (shape.isNull() || !(tall > 1e-3))
        return (error ? *error = "That piece has no height to measure." : QString()), false;
    QJsonObject json = scene.raw.value("world").toObject();
    json["scale"] = std::round(world.scale * meters / tall * 10000) / 10000;
    scene.raw["world"] = json;
    return true;
}

bool shapeObject(Scene &scene, AssetCache &assets, int index, QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    const Info world = info(scene);
    if (!world.valid)
        return fail("Make the world first (omotion world), then give its pieces shapes.");
    if (index < 0 || index >= scene.layers.size() || index == scene.indexOf(world.picture) ||
        !layers(scene).contains(index))
        return fail("Shapes are for pieces cut out of the world's picture.");
    const QImage image = assets.image(scene.layers.at(index).source);
    if (image.isNull())
        return fail(QString("Couldn't read %1.").arg(scene.layers.at(index).source));
    const Ai::Shape shape = Ai::instance().objectShape(image, error);
    if (shape.isNull())
        return false;

    // Which way round: the quarter turn whose outline, seen from the model's
    // -z, best covers the cut-out's (the model's own way round wins ties).
    const QRectF shown = shownBox(image);
    constexpr int Grid = 96;
    QImage target(Grid, Grid, QImage::Format_Grayscale8);
    {
        target.fill(0);
        QPainter painter(&target);
        painter.drawImage(QRectF(0, 0, Grid, Grid), image.copy(shown.toRect()).createAlphaMask(), QRectF());
    }
    auto coverage = [&](int quarter) {
        QVector3D low(1e9f, 1e9f, 1e9f), high = -low;
        std::vector<QPointF> flat(shape.positions.size() / 3);
        for (size_t i = 0; i < flat.size(); ++i) {
            const QVector3D v = quarterTurn(&shape.positions[i * 3], quarter);
            flat[i] = QPointF(-v.x(), -v.y());
            low = QVector3D(std::min(low.x(), -v.x()), std::min(low.y(), -v.y()), 0);
            high = QVector3D(std::max(high.x(), -v.x()), std::max(high.y(), -v.y()), 0);
        }
        QImage drawn(Grid, Grid, QImage::Format_Grayscale8);
        drawn.fill(0);
        QPainter painter(&drawn);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::white);
        const double sx = Grid / std::max(1e-6f, high.x() - low.x()), sy = Grid / std::max(1e-6f, high.y() - low.y());
        for (size_t t = 0; t < shape.triangles.size(); t += 3) {
            QPointF corners[3];
            for (int k = 0; k < 3; ++k) {
                const QPointF p = flat[shape.triangles[t + k]];
                corners[k] = QPointF((p.x() - low.x()) * sx, (p.y() - low.y()) * sy);
            }
            painter.drawPolygon(corners, 3);
        }
        painter.end();
        int both = 0, either = 0;
        for (int y = 0; y < Grid; ++y)
            for (int x = 0; x < Grid; ++x) {
                const bool a = drawn.constScanLine(y)[x] > 127, b = target.constScanLine(y)[x] < 128;
                both += a && b;
                either += a || b;
            }
        return either ? double(both) / either : 0.0;
    };
    int quarter = 0;
    double best = coverage(0) + .02;
    for (int q = 1; q < 4; ++q) {
        const double c = coverage(q);
        if (c > best)
            best = c, quarter = q;
    }

    const QDir dir(assets.directory());
    dir.mkpath("assets/world");
    const QString file = "assets/world/" + QFileInfo(scene.layers.at(index).source).completeBaseName() + "-shape.bin";
    QByteArray bytes("OMSHAPE2");
    const quint32 vertices = quint32(shape.positions.size() / 3), indices = quint32(shape.triangles.size());
    bytes.append(reinterpret_cast<const char *>(&vertices), 4);
    bytes.append(reinterpret_cast<const char *>(&indices), 4);
    bytes.append(reinterpret_cast<const char *>(shape.positions.data()), qsizetype(shape.positions.size() * 4));
    std::vector<float> colors = shape.colors;
    colors.resize(shape.positions.size(), .5f);
    bytes.append(reinterpret_cast<const char *>(colors.data()), qsizetype(colors.size() * 4));
    bytes.append(reinterpret_cast<const char *>(shape.triangles.data()), qsizetype(shape.triangles.size() * 4));
    QSaveFile out(dir.filePath(file));
    if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.commit())
        return fail(QString("Couldn't write %1.").arg(file));
    // With what it was made from: the piece's image, and the world it was
    // fitted in.
    QJsonObject made{{"file", file}, {"quarter", quarter}, {"fit", std::round(best * 100) / 100},
                     {"from", cutCode(scene.layers.at(index))}, {"world", scene.raw.value("world").toObject().value("made")}};
    // Made again, it keeps its modifiers.
    const QJsonValue modifiers = scene.layers.at(index).raw.value("shape").toObject().value("modifiers");
    if (!modifiers.isUndefined())
        made["modifiers"] = modifiers;
    scene.layers[index].raw["shape"] = made;
    return true;
}

QString texture(const Scene &scene, AssetCache &assets, int index) {
    if (index == FarLayer)
        return info(scene).farPicture;
    const QString source = scene.layers.at(index).source;
    const QDir dir(assets.directory());
    const QString file = "assets/world/" + QFileInfo(source).completeBaseName() + "-3d.png";
    const QFileInfo made(dir.filePath(file)), from(dir.filePath(source));
    const Info about = info(scene);
    const bool isPicture = index == scene.indexOf(about.picture) && !about.wide.isEmpty();
    const QFileInfo wideFile(dir.filePath(about.wide));
    if (made.exists() && made.lastModified() >= from.lastModified() &&
        (!isPicture || made.lastModified() >= wideFile.lastModified()))
        return file;
    QImage image = assets.image(source).convertToFormat(QImage::Format_ARGB32);
    if (image.isNull())
        return source;
    // The picture the world was made from wears its margins too: the layer
    // in the middle, the painted margins round it, at most 6144 across.
    const Info world = info(scene);
    bool widened = false;
    if (index == scene.indexOf(world.picture) && !world.wide.isEmpty() && !world.margin.isEmpty()) {
        const QImage wide(dir.filePath(world.wide));
        if (!wide.isNull()) {
            const QSizeF full = QSizeF(image.size()) + QSizeF(2 * world.margin.width(), 2 * world.margin.height());
            const double s = std::min(1.0, 6144 / full.width());
            QImage both(int(full.width() * s), int(full.height() * s), QImage::Format_ARGB32);
            QPainter painter(&both);
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.drawImage(both.rect(), wide);
            painter.drawImage(QRectF(world.margin.width() * s, world.margin.height() * s, image.width() * s, image.height() * s), image);
            painter.end();
            image = both;
            widened = true;
        }
    }
    bool seeThrough = false;
    for (int y = 0; y < image.height() && !seeThrough; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width() && !seeThrough; ++x)
            seeThrough = qAlpha(line[x]) < 255;
    }
    if (!seeThrough && !widened)
        return source;
    // Push-pull on colour weighted by alpha: shrink until every cell has some
    // colour, then grow back, filling the empty pixels from the level above.
    struct Level {
        int w, h;
        std::vector<float> rgb, weight;
    };
    Level fine{image.width(), image.height(), {}, {}};
    fine.rgb.resize(size_t(fine.w) * fine.h * 3);
    fine.weight.resize(size_t(fine.w) * fine.h);
    for (int y = 0; y < fine.h; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < fine.w; ++x) {
            const size_t i = size_t(y) * fine.w + x;
            const float a = qAlpha(line[x]) / 255.f;
            fine.weight[i] = a;
            fine.rgb[i * 3] = qRed(line[x]) * a, fine.rgb[i * 3 + 1] = qGreen(line[x]) * a, fine.rgb[i * 3 + 2] = qBlue(line[x]) * a;
        }
    }
    QList<Level> levels{fine};
    while (levels.last().w > 1 || levels.last().h > 1) {
        const Level &f = levels.last();
        Level c{std::max(1, (f.w + 1) / 2), std::max(1, (f.h + 1) / 2), {}, {}};
        c.rgb.assign(size_t(c.w) * c.h * 3, 0);
        c.weight.assign(size_t(c.w) * c.h, 0);
        for (int y = 0; y < f.h; ++y)
            for (int x = 0; x < f.w; ++x) {
                const size_t a = size_t(y) * f.w + x, b = size_t(y / 2) * c.w + x / 2;
                for (int k = 0; k < 3; ++k)
                    c.rgb[b * 3 + k] += f.rgb[a * 3 + k];
                c.weight[b] += f.weight[a];
            }
        levels.append(c);
    }
    for (int l = levels.size() - 2; l >= 0; --l) {
        Level &f = levels[l];
        const Level &c = levels.at(l + 1);
        for (int y = 0; y < f.h; ++y)
            for (int x = 0; x < f.w; ++x) {
                const size_t a = size_t(y) * f.w + x, b = size_t(y / 2) * c.w + x / 2;
                // Colour is kept multiplied by weight; a cell short of a
                // full weight takes the rest from the coarser level's colour.
                const float own = std::min(1.f, f.weight[a]);
                if (own >= 1 || c.weight[b] <= 0)
                    continue;
                for (int k = 0; k < 3; ++k)
                    f.rgb[a * 3 + k] += (1 - own) * c.rgb[b * 3 + k] / c.weight[b];
                f.weight[a] = 1;
            }
    }
    const Level &top = levels.first();
    for (int y = 0; y < top.h; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < top.w; ++x) {
            const size_t i = size_t(y) * top.w + x;
            const float w = std::max(top.weight[i], 1e-6f);
            line[x] = qRgba(std::clamp(int(top.rgb[i * 3] / w), 0, 255), std::clamp(int(top.rgb[i * 3 + 1] / w), 0, 255),
                            std::clamp(int(top.rgb[i * 3 + 2] / w), 0, 255), qAlpha(line[x]));
        }
    }
    dir.mkpath("assets/world");
    QString error;
    if (!writeDepth(image, dir.filePath(file), &error))
        return source;
    return file;
}

namespace {
// Paints in what isn't known of an image (known: 0 where to paint), with
// the AI, a square of 384 at a time: squares with something known in them
// go first, so each carries on from what's there; rounds repeat until
// nothing's left, or nothing more can be reached.
bool paintIn(QImage &work, QImage &known, QString *error) {
    const int tile = std::min({384, work.width(), work.height()}), step = tile * 2 / 3;
    QList<QPoint> corners;
    for (int y = 0;; y += step) {
        const int top = std::min(y, work.height() - tile);
        for (int x = 0;; x += step) {
            const int left = std::min(x, work.width() - tile);
            corners.append(QPoint(left, top));
            if (left == work.width() - tile)
                break;
        }
        if (top == work.height() - tile)
            break;
    }
    for (int round = 0; round < 6; ++round) {
        bool left = false;
        for (const QPoint &corner : corners) {
            const QRect area(corner, QSize(tile, tile));
            QImage mask(area.size(), QImage::Format_Alpha8);
            int unknown = 0, seen = 0;
            for (int y = 0; y < tile; ++y) {
                const uchar *k = known.constScanLine(area.y() + y) + area.x();
                uchar *m = mask.scanLine(y);
                for (int x = 0; x < tile; ++x) {
                    m[x] = k[x] ? 0 : 255;
                    unknown += !k[x];
                    seen += k[x] != 0;
                }
            }
            if (!unknown)
                continue;
            if (seen < tile * tile / 8) {
                left = true;
                continue;
            }
            const QImage filled = Ai::instance().inpaint(work.copy(area), mask, error);
            if (filled.isNull())
                return false;
            const QImage rgb = filled.convertToFormat(QImage::Format_RGB32);
            for (int y = 0; y < tile; ++y) {
                QRgb *to = reinterpret_cast<QRgb *>(work.scanLine(area.y() + y)) + area.x();
                uchar *k = known.scanLine(area.y() + y) + area.x();
                const QRgb *from = reinterpret_cast<const QRgb *>(rgb.constScanLine(y));
                for (int x = 0; x < tile; ++x)
                    if (!k[x]) {
                        to[x] = from[x];
                        k[x] = 255;
                    }
            }
        }
        if (!left)
            break;
    }
    return true;
}
}

QImage outpaint(const QImage &picture, const QSize &margin, QString *error) {
    // Worked at about 768 pixels across the picture, in squares of 384.
    const double s = std::min(1.0, 768.0 / picture.width());
    const QSize inner(std::max(1, int(picture.width() * s)), std::max(1, int(picture.height() * s)));
    const QSize pad(int(margin.width() * s), int(margin.height() * s));
    QImage work(inner.width() + 2 * pad.width(), inner.height() + 2 * pad.height(), QImage::Format_RGB32);
    work.fill(Qt::black);
    QImage known(work.size(), QImage::Format_Grayscale8);
    known.fill(0);
    {
        QPainter painter(&work);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(QRect(QPoint(pad.width(), pad.height()), inner), picture);
        QPainter mark(&known);
        mark.fillRect(QRect(QPoint(pad.width(), pad.height()), inner), Qt::white);
    }
    if (!paintIn(work, known, error))
        return {};
    // Back at full size: the margins as painted, the picture as it was.
    QImage wide(picture.width() + 2 * margin.width(), picture.height() + 2 * margin.height(), QImage::Format_RGB32);
    QPainter painter(&wide);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(wide.rect(), work);
    painter.drawImage(QPoint(margin.width(), margin.height()), picture.convertToFormat(QImage::Format_RGB32));
    painter.end();
    return wide;
}

void fillDepth(QImage &depth, const QImage &hole) {
    // Push-pull in log distance: the known cells are averaged down a pyramid
    // until no cell is unknown, then the holes are filled back up from it.
    const int w = depth.width(), h = depth.height();
    std::vector<double> value(size_t(w) * h), weight(size_t(w) * h);
    for (int y = 0; y < h; ++y) {
        const quint16 *line = reinterpret_cast<const quint16 *>(depth.constScanLine(y));
        const uchar *gap = hole.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            const size_t i = size_t(y) * w + x;
            weight[i] = gap[x] ? 0 : 1;
            value[i] = std::log(decodeDepth(line[x]));
        }
    }
    struct Level {
        int w, h;
        std::vector<double> value, weight;
    };
    QList<Level> levels{{w, h, value, weight}};
    while (levels.last().w > 1 || levels.last().h > 1) {
        const Level &fine = levels.last();
        Level coarse{std::max(1, (fine.w + 1) / 2), std::max(1, (fine.h + 1) / 2), {}, {}};
        coarse.value.assign(size_t(coarse.w) * coarse.h, 0);
        coarse.weight.assign(size_t(coarse.w) * coarse.h, 0);
        for (int y = 0; y < fine.h; ++y)
            for (int x = 0; x < fine.w; ++x) {
                const size_t f = size_t(y) * fine.w + x, c = size_t(y / 2) * coarse.w + x / 2;
                coarse.value[c] += fine.value[f] * fine.weight[f];
                coarse.weight[c] += fine.weight[f];
            }
        for (size_t i = 0; i < coarse.value.size(); ++i) {
            if (coarse.weight[i] > 0)
                coarse.value[i] /= coarse.weight[i];
            coarse.weight[i] = std::min(1.0, coarse.weight[i]);
        }
        levels.append(coarse);
    }
    for (int l = levels.size() - 2; l >= 0; --l) {
        Level &fine = levels[l];
        const Level &coarse = levels.at(l + 1);
        for (int y = 0; y < fine.h; ++y)
            for (int x = 0; x < fine.w; ++x) {
                const size_t f = size_t(y) * fine.w + x;
                const double from = coarse.value[size_t(y / 2) * coarse.w + x / 2];
                fine.value[f] = fine.weight[f] * fine.value[f] + (1 - fine.weight[f]) * from;
                fine.weight[f] = 1;
            }
    }
    // The pyramid fills in blocks; relaxing the filled cells toward their
    // neighbours' average smooths the blocks into slopes.
    Level &top = levels.first();
    for (int pass = 0; pass < 200; ++pass)
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = size_t(y) * w + x;
                if (weight[i] > 0)
                    continue;
                double sum = 0;
                int n = 0;
                for (const auto [dx, dy] : {std::pair{-1, 0}, std::pair{1, 0}, std::pair{0, -1}, std::pair{0, 1}})
                    if (x + dx >= 0 && x + dx < w && y + dy >= 0 && y + dy < h)
                        sum += top.value[size_t(y + dy) * w + x + dx], ++n;
                top.value[i] = sum / n;
            }
    for (int y = 0; y < h; ++y) {
        quint16 *line = reinterpret_cast<quint16 *>(depth.scanLine(y));
        const uchar *gap = hole.constScanLine(y);
        for (int x = 0; x < w; ++x)
            if (gap[x])
                line[x] = encodeDepth(std::exp(top.value[size_t(y) * w + x]));
    }
}

namespace {
// Dust takes the colour of the ground it comes from, lit: the average of
// the picture's lower part, more saturated and lighter, as dust in the air
// catches the light.
QColor dustColour(const QImage &picture) {
    // The brightest quarter of the picture's lower half: lit ground, or the
    // dust already in it, rather than the shadows between.
    const QImage small = picture.scaled(64, 64, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB32);
    QList<QRgb> lower;
    for (int y = 32; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            lower.append(small.pixel(x, y));
    std::sort(lower.begin(), lower.end(), [](QRgb a, QRgb b) { return qGray(a) > qGray(b); });
    double r = 0, g = 0, b = 0;
    const int n = std::max<qsizetype>(1, lower.size() / 4);
    for (int i = 0; i < n; ++i)
        r += qRed(lower.at(i)), g += qGreen(lower.at(i)), b += qBlue(lower.at(i));
    QColor colour(int(r / n), int(g / n), int(b / n));
    float h, s, l;
    colour.getHslF(&h, &s, &l);
    return QColor::fromHslF(h, std::min(1.f, s * 1.15f), std::min(.85f, l * 1.1f));
}
}

namespace {
// The air's colour: the picture just below its horizon, where the land is
// farthest and most hidden by it.
QColor hazeColour(const QImage &picture, const Ai::Geometry &shape) {
    // The air is the colour of the farthest land the picture shows, where
    // most of it lies between the eye and the land: the farthest tenth of
    // what isn't sky.
    const QImage small = picture.scaled(shape.width, shape.height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                             .convertToFormat(QImage::Format_RGB32);
    QList<std::pair<float, QRgb>> land;
    for (int y = 0; y < shape.height; y += 2)
        for (int x = 0; x < shape.width; x += 2) {
            const size_t i = size_t(y) * shape.width + x;
            if (shape.valid[i])
                land.append({shape.points[i * 3 + 2], small.pixel(x, y)});
        }
    if (land.isEmpty())
        return QColor(200, 140, 160);
    std::sort(land.begin(), land.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    double r = 0, g = 0, b = 0;
    const int n = std::max<qsizetype>(1, land.size() / 10);
    for (int i = 0; i < n; ++i)
        r += qRed(land.at(i).second), g += qGreen(land.at(i).second), b += qBlue(land.at(i).second);
    return QColor(int(r / n), int(g / n), int(b / n));
}
}

bool build(Scene &scene, AssetCache &assets, int index, QString *error, double horizon) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    const int root = rootOf(scene, index);
    if (root < 0 || root >= scene.layers.size() || !scene.layers.at(root).particles.isEmpty())
        return fail("Pick a picture to make a world of.");
    const QString file = pictureFile(scene, root);
    const QImage picture = assets.image(file);
    if (picture.isNull())
        return fail(QString("Couldn't read %1.").arg(file));
    Ai::Geometry shape = Ai::instance().geometry(picture, error);
    if (shape.isNull())
        return false;

    // The picture's distances, cell by cell; the sky where the AI sees none.
    QImage depth(shape.width, shape.height, QImage::Format_Grayscale16);
    for (int y = 0; y < shape.height; ++y) {
        quint16 *line = reinterpret_cast<quint16 *>(depth.scanLine(y));
        for (int x = 0; x < shape.width; ++x) {
            const size_t i = size_t(y) * shape.width + x;
            line[x] = shape.valid[i] ? encodeDepth(shape.points[i * 3 + 2]) : Sky;
        }
    }

    // Behind the pieces that punched holes in it, the picture goes on as its
    // surroundings do: those cells (and a cell's margin, for soft edges) are
    // filled from around them.
    QImage hole(shape.width, shape.height, QImage::Format_Grayscale8);
    hole.fill(0);
    const double toCell = double(shape.width) / picture.width();
    for (int i = 0; i < scene.layers.size(); ++i) {
        if (i == root || rootOf(scene, i) != root || !scene.layers.at(i).raw.value("cut").toObject().value("punch").toBool(true))
            continue;
        const QImage piece = assets.image(scene.layers.at(i).source);
        const QPointF offset = offsetInPicture(scene, i);
        for (int y = 0; y < piece.height(); ++y) {
            const QRgb *line = reinterpret_cast<const QRgb *>(piece.constScanLine(y));
            for (int x = 0; x < piece.width(); ++x) {
                if (qAlpha(line[x]) < 8)
                    continue;
                const int cx = int((offset.x() + x) * toCell), cy = int((offset.y() + y) * toCell);
                for (int dy = -2; dy <= 2; ++dy)
                    for (int dx = -2; dx <= 2; ++dx)
                        if (cx + dx >= 0 && cx + dx < shape.width && cy + dy >= 0 && cy + dy < shape.height)
                            hole.scanLine(cy + dy)[cx + dx] = 255;
            }
        }
    }
    QImage behind = depth.copy();
    fillDepth(behind, hole);

    // The ground: in the lower part of the picture, the cells the AI sees
    // facing up pick out where to look; a plane fitted through their points
    // (the farthest third left out, twice, for rocks and bushes) is the
    // ground. Up is its normal; the camera's height, how far above it is.
    QList<QVector3D> ground;
    for (int y = shape.height * 6 / 10; y < shape.height; y += 2)
        for (int x = 0; x < shape.width; x += 2) {
            const size_t i = size_t(y) * shape.width + x;
            if (shape.valid[i] && shape.normals[i * 3 + 1] < -.6f)
                ground.append(QVector3D(shape.points[i * 3], shape.points[i * 3 + 1], shape.points[i * 3 + 2]));
        }
    QVector3D up(0, -1, 0);
    double height = 1.6;
    for (int round = 0; round < 3 && ground.size() > 100; ++round) {
        QVector3D centre;
        for (const QVector3D &p : ground)
            centre += p;
        centre /= float(ground.size());
        up = planeNormal(ground, centre);
        if (up.y() > 0)
            up = -up;
        height = -QVector3D::dotProduct(up, centre);
        if (round < 2) {
            std::sort(ground.begin(), ground.end(), [&](const QVector3D &a, const QVector3D &b) {
                return std::abs(QVector3D::dotProduct(up, a - centre)) < std::abs(QVector3D::dotProduct(up, b - centre));
            });
            ground = ground.mid(0, ground.size() * 2 / 3);
        }
    }
    height = std::max(.05, height);
    if (horizon > 0 && horizon < 1) {
        // Level is where the horizon says: tilted up by its distance below
        // the middle, not rolled. The camera stands as high above the ground
        // in sight as before, measured that way.
        const double pitch = std::atan((horizon - .5) * picture.height() / shape.focal);
        up = QVector3D(0, float(-std::cos(pitch)), float(std::sin(pitch)));
        QList<double> heights;
        for (const QVector3D &p : ground)
            heights.append(-QVector3D::dotProduct(up, p));
        if (!heights.isEmpty()) {
            std::sort(heights.begin(), heights.end());
            height = std::max(.05, heights.at(heights.size() / 2));
        }
    }

    // Behind a piece cut out low in the picture, there's ground: where a
    // filled-in cell looks down at it, it lies on the ground, not on a blend
    // of the ground before and the hills beyond (which stands up as a wall
    // when the camera moves).
    for (int y = 0; y < shape.height; ++y)
        for (int x = 0; x < shape.width; ++x) {
            if (!hole.constScanLine(y)[x])
                continue;
            const double px = (x + .5) / toCell, py = (y + .5) / toCell;
            const QVector3D ray(float((px - picture.width() / 2.0) / shape.focal), float((py - picture.height() / 2.0) / shape.focal), 1);
            const double down = QVector3D::dotProduct(up, ray);
            if (down < -.01)
                reinterpret_cast<quint16 *>(behind.scanLine(y))[x] = encodeDepth(std::min(Far, height / -down));
        }

    // Past the edges: the picture carried on by the AI, and its distances by
    // the cells around, or the ground where a view down meets it, so a
    // camera can turn a little without looking off the world.
    const QSize margin(int(picture.width() * .3), int(picture.height() * .25));
    const QImage wide = outpaint(picture, margin, error);
    if (wide.isNull())
        return false;
    const QSize cells(int(std::round(margin.width() * toCell)), int(std::round(margin.height() * toCell)));
    QImage around(shape.width + 2 * cells.width(), shape.height + 2 * cells.height(), QImage::Format_Grayscale16);
    QImage unknown(around.size(), QImage::Format_Grayscale8);
    unknown.fill(255);
    for (int y = 0; y < shape.height; ++y) {
        memcpy(around.scanLine(y + cells.height()) + cells.width() * 2, behind.constScanLine(y), size_t(shape.width) * 2);
        memset(unknown.scanLine(y + cells.height()) + cells.width(), 0, size_t(shape.width));
    }
    fillDepth(around, unknown);
    const double cellToPicture = 1 / toCell;
    for (int y = 0; y < around.height(); ++y)
        for (int x = 0; x < around.width(); ++x) {
            if (!unknown.scanLine(y)[x])
                continue;
            const double px = (x - cells.width() + .5) * cellToPicture, py = (y - cells.height() + .5) * cellToPicture;
            const QVector3D ray((px - picture.width() / 2.0) / shape.focal, (py - picture.height() / 2.0) / shape.focal, 1);
            const double down = QVector3D::dotProduct(up, ray);
            if (down < -.01) {
                const double ground = height / -down;
                quint16 &code = reinterpret_cast<quint16 *>(around.scanLine(y))[x];
                code = encodeDepth(std::min(decodeDepth(code), ground));
            }
        }
    behind = around;

    // Jumps in distance: a cell whose neighbour is more than 15% farther is
    // on the near side of one. Into the near thing from there, a few cells
    // deep, the far surface is carried on: that band is what a moving
    // camera can see behind the edge.
    constexpr double Jump = 1.15;
    const int aw = behind.width(), ah = behind.height();
    auto distanceAt = [&](int x, int y) { return decodeDepth(reinterpret_cast<const quint16 *>(behind.constScanLine(y))[x]); };
    std::vector<float> farOf(size_t(aw) * ah, 0);
    std::vector<int> steps(size_t(aw) * ah, -1);
    std::vector<uchar> farEdge(size_t(aw) * ah, 0);
    QList<int> queue;
    static const int around4[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    // Diagonals too: the picture's triangles have diagonal sides.
    static const int around8[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, -1}, {1, -1}, {-1, 1}};
    for (int y = 0; y < ah; ++y)
        for (int x = 0; x < aw; ++x) {
            const double here = distanceAt(x, y);
            for (const auto &d : around8) {
                const int nx = x + d[0], ny = y + d[1];
                if (nx < 0 || ny < 0 || nx >= aw || ny >= ah)
                    continue;
                const double there = distanceAt(nx, ny);
                if (there > here * Jump) {
                    const size_t i = size_t(y) * aw + x;
                    farOf[i] = std::max(farOf[i], float(there));
                    farEdge[size_t(ny) * aw + nx] = 1;
                }
            }
            if (farOf[size_t(y) * aw + x] > 0)
                steps[size_t(y) * aw + x] = 0, queue.append(y * aw + x);
        }
    const int deep = std::max(6, aw / 48);
    for (qsizetype q = 0; q < queue.size(); ++q) {
        const int i = queue.at(q), x = i % aw, y = i / aw;
        if (steps[size_t(i)] >= deep)
            continue;
        for (const auto &d : around4) {
            const int nx = x + d[0], ny = y + d[1];
            if (nx < 0 || ny < 0 || nx >= aw || ny >= ah)
                continue;
            const size_t j = size_t(ny) * aw + nx;
            if (steps[j] >= 0 || distanceAt(nx, ny) * Jump > farOf[size_t(i)])
                continue;
            steps[j] = steps[size_t(i)] + 1;
            farOf[j] = farOf[size_t(i)];
            queue.append(int(j));
        }
    }
    // On the far side it overlaps the edge by two of the picture mesh's cells
    // (a dropped triangle reaches one past it), so no gap is left uncovered.
    {
        const int reach = 2 * int(std::ceil(aw / 320.0));
        std::vector<int> along(size_t(aw) * ah, -1);
        QList<int> ring;
        for (int i = 0; i < aw * ah; ++i)
            if (farEdge[size_t(i)])
                along[size_t(i)] = 0, ring.append(i);
        for (qsizetype q = 0; q < ring.size(); ++q) {
            const int i = ring.at(q), x = i % aw, y = i / aw;
            if (along[size_t(i)] >= reach)
                continue;
            for (const auto &d : around4) {
                const int nx = x + d[0], ny = y + d[1];
                if (nx < 0 || ny < 0 || nx >= aw || ny >= ah || along[size_t(ny) * aw + nx] >= 0 || steps[size_t(ny) * aw + nx] >= 0)
                    continue;
                along[size_t(ny) * aw + nx] = along[size_t(i)] + 1;
                farEdge[size_t(ny) * aw + nx] = 1;
                ring.append(ny * aw + nx);
            }
        }
    }
    QImage farDepthGrid = behind.copy();
    for (int y = 0; y < ah; ++y)
        for (int x = 0; x < aw; ++x)
            if (steps[size_t(y) * aw + x] >= 0)
                reinterpret_cast<quint16 *>(farDepthGrid.scanLine(y))[x] = encodeDepth(farOf[size_t(y) * aw + x]);
    // Its picture: the widened picture with the band painted in, see-through
    // everywhere else (but a cell's overlap on the far side, to join).
    const QSize farSize(std::min(2048, wide.width()), int(std::round(std::min(2048, wide.width()) * double(wide.height()) / wide.width())));
    // Starting from the background as it is now: the pieces cut out of it
    // and filled in behind, not the picture with them in it.
    QImage farWork = wide.scaled(farSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB32);
    {
        const QImage now = assets.image(scene.layers.at(root).source);
        if (!now.isNull()) {
            const double k = double(farSize.width()) / wide.width();
            QPainter painter(&farWork);
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.drawImage(QRectF(margin.width() * k, margin.height() * k, picture.width() * k, picture.height() * k), now);
        }
    }
    QImage farKnown(farSize, QImage::Format_Grayscale8), farAlpha(farSize, QImage::Format_Alpha8);
    farKnown.fill(255);
    farAlpha.fill(0);
    {
        QPainter unknown(&farKnown), shown(&farAlpha);
        const double cw = double(farSize.width()) / aw, ch = double(farSize.height()) / ah;
        for (int y = 0; y < ah; ++y)
            for (int x = 0; x < aw; ++x) {
                const size_t i = size_t(y) * aw + x;
                const QRectF cell(x * cw - cw, y * ch - ch, cw * 3, ch * 3);
                if (steps[i] >= 0) {
                    unknown.fillRect(cell, Qt::black);
                    shown.fillRect(cell, Qt::white);
                } else if (farEdge[i]) {
                    shown.fillRect(cell, Qt::white);
                }
            }
    }
    if (!paintIn(farWork, farKnown, error))
        return false;
    QImage farPicture = farWork.convertToFormat(QImage::Format_ARGB32);
    soften(farAlpha, 2);
    for (int y = 0; y < farPicture.height(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(farPicture.scanLine(y));
        const uchar *a = farAlpha.constScanLine(y);
        for (int x = 0; x < farPicture.width(); ++x)
            line[x] = qRgba(qRed(line[x]), qGreen(line[x]), qBlue(line[x]), a[x]);
    }

    const QDir dir(assets.directory());
    dir.mkpath("assets/world");
    const QString stem = "assets/world/" + scene.layers.at(root).name.toLower().replace(QRegularExpression("[^a-z0-9]+"), "-");
    const QString depthFile = stem + "-depth.png", behindFile = stem + "-behind.png";
    const QString farDepthFile = stem + "-far-depth.png", farPictureFile = stem + "-far.png";
    if (!writeDepth(farDepthGrid, dir.filePath(farDepthFile), error) || !writeDepth(farPicture, dir.filePath(farPictureFile), error))
        return false;
    const QString wideFile = stem + "-wide.jpg";
    QSaveFile wideOut(dir.filePath(wideFile));
    QImageWriter wideWriter(&wideOut, "jpg");
    wideWriter.setQuality(92);
    if (!writeDepth(depth, dir.filePath(depthFile), error) || !writeDepth(behind, dir.filePath(behindFile), error))
        return false;
    // Stored at most 4096 across: only its margins are used, and soft.
    const QImage stored = wide.width() > 4096 ? wide.scaledToWidth(4096, Qt::SmoothTransformation) : wide;
    if (!wideOut.open(QIODevice::WriteOnly) || !wideWriter.write(stored) || !wideOut.commit())
        return fail(QString("Couldn't write %1.").arg(wideFile));
    const double focal = shape.focal;
    const QJsonObject before = scene.raw.value("world").toObject();
    scene.raw["world"] = QJsonObject{
        {"picture", scene.layers.at(root).name},
        {"depth", depthFile},
        {"behind", behindFile},
        {"focal", std::round(focal * 10) / 10},
        {"wide", wideFile},
        {"far", QJsonObject{{"depth", farDepthFile}, {"picture", farPictureFile}}},
        {"dust", dustColour(picture).name()},
        {"haze", QJsonObject{{"color", hazeColour(picture, shape).name()}, {"density", .02}}},
        {"margin", QJsonArray{margin.width(), margin.height()}},
        {"ground", QJsonObject{{"up", QJsonArray{std::round(up.x() * 1e4) / 1e4, std::round(up.y() * 1e4) / 1e4,
                                                   std::round(up.z() * 1e4) / 1e4}},
                               {"height", std::round(height * 1000) / 1000}}}};
    QJsonObject made = scene.raw["world"].toObject();
    // What it was made from, so it's known when that changes; and when, so
    // shapes fitted to it know it's new.
    made["pieces"] = madeFrom(scene, root);
    made["made"] = QDateTime::currentMSecsSinceEpoch();
    if (horizon > 0 && horizon < 1)
        made["horizon"] = horizon;
    // Made again, it keeps what was set by hand: its size, its look.
    if (before.value("picture").toString() == scene.layers.at(root).name)
        for (const QString &kept : {"scale", "grain", "shutter", "haze", "dust", "horizon"})
            if (before.contains(kept) && !(kept == QString("horizon") && horizon > 0 && horizon < 1))
                made[kept] = before.value(kept);
    scene.raw["world"] = made;
    return true;
}

bool setShapeModifier(Scene &scene, int index, const QString &type, int amount, QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (index < 0 || index >= scene.layers.size() || scene.layers.at(index).raw.value("shape").toObject().isEmpty())
        return fail("Give the piece a 3D shape first (S).");
    if (type != "smooth" && type != "simplify" && type != "none")
        return fail("A shape's modifier is smooth, simplify or none.");
    QJsonObject shape = scene.layers.at(index).raw.value("shape").toObject();
    QJsonArray list;
    for (const QJsonValue &m : shape.value("modifiers").toArray())
        if (type != "none" && m.toObject().value("type").toString() != type)
            list.append(m);
    if (type == "smooth")
        list.append(QJsonObject{{"type", "smooth"}, {"times", std::clamp(amount, 1, 50)}});
    else if (type == "simplify")
        list.append(QJsonObject{{"type", "simplify"}, {"cells", std::clamp(amount, 4, 1024)}});
    if (list.isEmpty())
        shape.remove("modifiers");
    else
        shape["modifiers"] = list;
    scene.layers[index].raw["shape"] = shape;
    return true;
}

QJsonArray madeFrom(const Scene &scene, int root) {
    QJsonArray pieces;
    for (int i = 0; i < scene.layers.size(); ++i)
        if (i != root && rootOf(scene, i) == root && scene.layers.at(i).raw.value("cut").toObject().value("punch").toBool(true))
            pieces.append(cutCode(scene.layers.at(i)));
    return pieces;
}

QString cutCode(const Layer &layer) {
    const QJsonObject cut = layer.raw.value("cut").toObject();
    if (cut.isEmpty())
        return layer.source;
    QJsonObject shape;
    for (const QString &key : {"from", "polygon", "mask", "maskOrigin", "feather", "origin", "punch"})
        if (cut.contains(key))
            shape[key] = cut.value(key);
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(shape).toJson(QJsonDocument::Compact), QCryptographicHash::Sha1)
                                   .toHex()
                                   .left(12));
}

}
