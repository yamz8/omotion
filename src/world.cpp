#include "world.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
    if (!(info.scale > 0))
        info.scale = 1;
    info.wide = world.value("wide").toString();
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
        if (scene.layers.at(i).particles.isEmpty() && rootOf(scene, i) == root)
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
        return code >= Sky ? Far : decodeDepth(code) * scale;
    }
    QVector3D point(double x, double y, double distance) const {
        return QVector3D((x - picture.width() / 2.0) * distance / focal, (y - picture.height() / 2.0) * distance / focal,
                         distance);
    }
};

// Depth images are read once per file and kept, as meshes ask often.
DepthMap depthMap(AssetCache &assets, const QString &file, const QSize &picture, double focal, double scale = 1) {
    static QHash<QString, QImage> cache;
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
    std::vector<quint32> triangles;
    int quarter = 0;
    bool isNull() const { return positions.empty(); }
};

StoredShape readShape(const QString &path) {
    static QHash<QString, StoredShape> cache;
    const QString key = path + "|" + QString::number(QFileInfo(path).lastModified().toMSecsSinceEpoch());
    if (cache.contains(key))
        return cache.value(key);
    StoredShape shape;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return shape;
    const QByteArray bytes = file.readAll();
    if (bytes.size() < 16 || !bytes.startsWith("OMSHAPE1"))
        return shape;
    const quint32 vertices = *reinterpret_cast<const quint32 *>(bytes.constData() + 8);
    const quint32 indices = *reinterpret_cast<const quint32 *>(bytes.constData() + 12);
    if (bytes.size() != qsizetype(16 + vertices * 12 + indices * 4))
        return shape;
    const float *p = reinterpret_cast<const float *>(bytes.constData() + 16);
    shape.positions.assign(p, p + vertices * 3);
    const quint32 *t = reinterpret_cast<const quint32 *>(bytes.constData() + 16 + vertices * 12);
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

QQuaternion turn(double yaw, double pitch, double roll) {
    // Yaw to the right about up, then pitch up about the right, then roll
    // clockwise about ahead, as a driver or a camera turns.
    return QQuaternion::fromAxisAndAngle(0, 1, 0, float(-yaw)) * QQuaternion::fromAxisAndAngle(1, 0, 0, float(pitch)) *
           QQuaternion::fromAxisAndAngle(0, 0, 1, float(-roll));
}

}

// A piece's own distances: the picture's depth cells under its rectangle,
// keeping only those it covers (its edge cells mix in what's behind it, as
// far as the sky), held between the 5th and 95th percentile of them, and
// the rest filled from those, so its edges stay on it.
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
            const bool covered = qAlpha(piece.pixel(ix, iy)) > 200 && d < Far;
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
    // And smoothed a little, so its surface has no spikes.
    for (int pass = 0; pass < 3; ++pass) {
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

ShapeFit fitShape(const Scene &scene, AssetCache &assets, int index) {
    ShapeFit fit;
    const Info world = info(scene);
    const int root = scene.indexOf(world.picture);
    const Layer &layer = scene.layers.at(index);
    const QString file = layer.raw.value("shape").toObject().value("file").toString();
    if (!world.valid || index == root || file.isEmpty())
        return fit;
    fit.model = readShape(QDir(assets.directory()).filePath(file));
    fit.model.quarter = layer.raw.value("shape").toObject().value("quarter").toInt();
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

Mesh mesh(const Scene &scene, AssetCache &assets, int index) {
    const Info world = info(scene);
    const int root = scene.indexOf(world.picture);
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
        Mesh mesh;
        mesh.vertices.resize(qsizetype(count) * 5 * sizeof(float));
        float *v = reinterpret_cast<float *>(mesh.vertices.data());
        mesh.minimum = QVector3D(1e9f, 1e9f, 1e9f);
        mesh.maximum = -mesh.minimum;
        for (size_t i = 0; i < count; ++i) {
            const QVector3D seen = fit.place(&fit.model.positions[i * 3]);
            const QVector3D p = toWorld.map(seen);
            const double px = picture.width() / 2.0 + world.focal * seen.x() / seen.z();
            const double py = picture.height() / 2.0 + world.focal * seen.y() / seen.z();
            *v++ = p.x(), *v++ = p.y(), *v++ = p.z();
            *v++ = float((px - fit.offset.x()) / fit.size.width());
            *v++ = float(1 - (py - fit.offset.y()) / fit.size.height());
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

    // About four picture pixels a cell, at most 320 across, at least 2.
    const int cols = std::clamp(int(size.width() / 4), 2, 320), rows = std::clamp(int(size.height() / 4), 2, 320);
    // Its box counts only what shows: a piece's see-through margin reads
    // whatever lies behind it, as far as the sky.
    double factor = 1;
    const QImage image = index == root ? QImage() : assets.mip(layer.source, std::min(1.0, 4 * cols / size.width()), &factor);
    const DepthMap own = index == root || image.isNull() ? depth : pieceDepth(depth, image, offset, size);
    Mesh mesh;
    mesh.vertices.resize(qsizetype(cols + 1) * (rows + 1) * 5 * sizeof(float));
    float *v = reinterpret_cast<float *>(mesh.vertices.data());
    mesh.minimum = QVector3D(1e9f, 1e9f, 1e9f);
    mesh.maximum = -mesh.minimum;
    for (int r = 0; r <= rows; ++r)
        for (int c = 0; c <= cols; ++c) {
            const double px = size.width() * c / cols, py = size.height() * r / rows;
            const double x = offset.x() + px, y = offset.y() + py;
            const double distance = index == root ? depth.at(x, y) : own.at(px, py);
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
    mesh.indices.resize(qsizetype(cols) * rows * 6 * sizeof(quint32));
    quint32 *i = reinterpret_cast<quint32 *>(mesh.indices.data());
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            const quint32 a = r * (cols + 1) + c, b = a + 1, d = a + cols + 1, e = d + 1;
            *i++ = a, *i++ = d, *i++ = b, *i++ = b, *i++ = d, *i++ = e;
        }

    mesh.pivot = pivot(scene, assets, index);
    return mesh;
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
    if (distance >= Far) {
        QList<double> all;
        for (int r = 0; r <= 8; ++r)
            for (int c = 0; c <= 8; ++c) {
                const double d = depth.at(offset.x() + size.width() * c / 8, offset.y() + size.height() * r / 8);
                if (d < Far)
                    all.append(d);
            }
        std::sort(all.begin(), all.end());
        distance = all.isEmpty() ? 10 : all.at(all.size() / 2);
    }
    return pictureToWorld(world).map(depth.point(ax, ay, distance));
}

Frame frame(const Scene &scene, AssetCache &assets, double t) {
    Frame frame;
    const Info world = info(scene);
    if (!world.valid)
        return frame;
    const int root = scene.indexOf(world.picture);
    const QSizeF picture = assets.size(pictureFile(scene, root));
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
    frame.cameraRotation = yaw * picturedTurn * pitchRoll;
    frame.cameraPosition = toWorld.map(QVector3D(0, 0, 0)) +
                           QVector3D(scene.value(camera, "worldX", t), scene.value(camera, "worldY", t),
                                     -scene.value(camera, "worldZ", t));
    const double zoom = std::max(1e-3, scene.value(camera, "zoom", t));
    const double half = std::atan(picture.height() / 2 / world.focal) ;
    frame.fov = 2 * std::atan(std::tan(half) / zoom) * 180 / M_PI;

    for (const int index : layers(scene)) {
        const Layer &layer = scene.layers.at(index);
        const QVector3D centre = pivot(scene, assets, index);
        Placement place;
        place.layer = index;
        place.visible = layer.visible;
        place.opacity = float(std::clamp(scene.value(layer, "opacity", t), 0.0, 1.0));
        // Sizes in a world come from distances: a layer's 2D scale (how its
        // picture fits the canvas, or a flat animation) doesn't apply.
        place.scale = 1;
        place.rotation = turn(scene.value(layer, "yaw", t), scene.value(layer, "pitch", t), scene.value(layer, "roll", t));
        const QVector3D move(scene.value(layer, "worldX", t), scene.value(layer, "worldY", t), -scene.value(layer, "worldZ", t));
        // Turned and scaled about its pivot, then moved.
        place.position = centre + move - place.rotation.rotatedVector(centre * place.scale);
        frame.layers.append(place);
    }
    return frame;
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
    QList<size_t> queue;
    for (int c = 0; c < columns; ++c) {
        const size_t i = size_t(rowsOf - 1) * columns + c;
        if (isGround[i])
            reached[i] = 1, queue.append(i);
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
        const size_t count = size_t(shape.vertices.size()) / (5 * sizeof(float));
        double sxx = 0, szz = 0, sxz = 0, mx = 0, mz = 0;
        for (size_t i = 0; i < count; ++i)
            mx += v[i * 5], mz += v[i * 5 + 2];
        mx /= count, mz /= count;
        for (size_t i = 0; i < count; ++i) {
            const double dx = v[i * 5] - mx, dz = v[i * 5 + 2] - mz;
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
    QJsonArray landed;
    for (double time = 0; time <= duration + dt / 2; time += dt) {
        path.push_back({time, s, y, vy});
        const double next = s + speed * dt, below = groundAlong(next);
        if (!flying) {
            const double vyNext = (below - y) / dt;
            // Springs soak up a little; past that the ground drops away.
            if ((vyNext - vy) / dt < -1.2 * gravity) {
                flying = true;
            } else {
                y = below, vy = vyNext;
            }
        }
        if (flying) {
            vy -= gravity * dt;
            y += vy * dt;
            if (y <= below) {
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
    QByteArray bytes("OMSHAPE1");
    const quint32 vertices = quint32(shape.positions.size() / 3), indices = quint32(shape.triangles.size());
    bytes.append(reinterpret_cast<const char *>(&vertices), 4);
    bytes.append(reinterpret_cast<const char *>(&indices), 4);
    bytes.append(reinterpret_cast<const char *>(shape.positions.data()), qsizetype(shape.positions.size() * 4));
    bytes.append(reinterpret_cast<const char *>(shape.triangles.data()), qsizetype(shape.triangles.size() * 4));
    QSaveFile out(dir.filePath(file));
    if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.commit())
        return fail(QString("Couldn't write %1.").arg(file));
    scene.layers[index].raw["shape"] = QJsonObject{{"file", file}, {"quarter", quarter}, {"fit", std::round(best * 100) / 100}};
    return true;
}

QString texture(const Scene &scene, AssetCache &assets, int index) {
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
    // Squares next to what's known go first, so each one has something to
    // carry on from; rounds repeat until nothing is left unknown.
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
                return {};
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

    const QDir dir(assets.directory());
    dir.mkpath("assets/world");
    const QString stem = "assets/world/" + scene.layers.at(root).name.toLower().replace(QRegularExpression("[^a-z0-9]+"), "-");
    const QString depthFile = stem + "-depth.png", behindFile = stem + "-behind.png";
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
    scene.raw["world"] = QJsonObject{
        {"picture", scene.layers.at(root).name},
        {"depth", depthFile},
        {"behind", behindFile},
        {"focal", std::round(focal * 10) / 10},
        {"wide", wideFile},
        {"margin", QJsonArray{margin.width(), margin.height()}},
        {"ground", QJsonObject{{"up", QJsonArray{std::round(up.x() * 1e4) / 1e4, std::round(up.y() * 1e4) / 1e4,
                                                   std::round(up.z() * 1e4) / 1e4}},
                               {"height", std::round(height * 1000) / 1000}}}};
    return true;
}

}
