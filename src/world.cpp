#include "world.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QSaveFile>
#include <QImageWriter>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <cmath>

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
    m.setRow(1, QVector4D(y, info.height));
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
    // Distance at a point of the picture, nearest cell.
    double at(double x, double y) const {
        const int gx = std::clamp(int(x / picture.width() * image.width()), 0, image.width() - 1);
        const int gy = std::clamp(int(y / picture.height() * image.height()), 0, image.height() - 1);
        return decodeDepth(reinterpret_cast<const quint16 *>(image.constScanLine(gy))[gx]);
    }
    QVector3D point(double x, double y, double distance) const {
        return QVector3D((x - picture.width() / 2.0) * distance / focal, (y - picture.height() / 2.0) * distance / focal,
                         distance);
    }
};

// Depth images are read once per file and kept, as meshes ask often.
DepthMap depthMap(AssetCache &assets, const QString &file, const QSize &picture, double focal) {
    static QHash<QString, QImage> cache;
    const QString path = QDir(assets.directory()).filePath(file);
    const QString key = path + "|" + QString::number(QFileInfo(path).lastModified().toMSecsSinceEpoch());
    if (!cache.contains(key))
        cache.insert(key, QImage(path).convertToFormat(QImage::Format_Grayscale16));
    return DepthMap{cache.value(key), picture, focal};
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

QQuaternion turn(double yaw, double pitch, double roll) {
    // Yaw to the right about up, then pitch up about the right, then roll
    // clockwise about ahead, as a driver or a camera turns.
    return QQuaternion::fromAxisAndAngle(0, 1, 0, float(-yaw)) * QQuaternion::fromAxisAndAngle(1, 0, 0, float(pitch)) *
           QQuaternion::fromAxisAndAngle(0, 0, 1, float(-roll));
}

}

Mesh mesh(const Scene &scene, AssetCache &assets, int index) {
    const Info world = info(scene);
    const int root = scene.indexOf(world.picture);
    if (!world.valid || index < 0 || index >= scene.layers.size() || rootOf(scene, index) != root)
        return {};
    const Layer &layer = scene.layers.at(index);
    const QSizeF size = assets.size(layer.source);
    const QSizeF picture = assets.size(pictureFile(scene, root));
    if (size.isEmpty() || picture.isEmpty())
        return {};
    // The picture lies on what's behind its pieces; pieces on the picture's own depth.
    const DepthMap depth = depthMap(assets, index == root ? world.behind : world.depth, picture.toSize(), world.focal);
    if (depth.image.isNull())
        return {};
    const QPointF offset = index == root ? QPointF() : offsetInPicture(scene, index);
    const QMatrix4x4 toWorld = pictureToWorld(world);

    // About four picture pixels a cell, at most 320 across, at least 2.
    const int cols = std::clamp(int(size.width() / 4), 2, 320), rows = std::clamp(int(size.height() / 4), 2, 320);
    Mesh mesh;
    mesh.vertices.resize(qsizetype(cols + 1) * (rows + 1) * 5 * sizeof(float));
    float *v = reinterpret_cast<float *>(mesh.vertices.data());
    mesh.minimum = QVector3D(1e9f, 1e9f, 1e9f);
    mesh.maximum = -mesh.minimum;
    for (int r = 0; r <= rows; ++r)
        for (int c = 0; c <= cols; ++c) {
            const double px = size.width() * c / cols, py = size.height() * r / rows;
            const double x = offset.x() + px, y = offset.y() + py;
            const QVector3D p = toWorld.map(depth.point(x, y, depth.at(x, y)));
            *v++ = p.x();
            *v++ = p.y();
            *v++ = p.z();
            *v++ = float(c) / cols;
            *v++ = 1 - float(r) / rows;
            mesh.minimum = QVector3D(std::min(mesh.minimum.x(), p.x()), std::min(mesh.minimum.y(), p.y()),
                                     std::min(mesh.minimum.z(), p.z()));
            mesh.maximum = QVector3D(std::max(mesh.maximum.x(), p.x()), std::max(mesh.maximum.y(), p.y()),
                                     std::max(mesh.maximum.z(), p.z()));
        }
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
    const DepthMap depth = depthMap(assets, index == root ? world.behind : world.depth, picture.toSize(), world.focal);
    if (depth.image.isNull() || size.isEmpty())
        return {};
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

bool build(Scene &scene, AssetCache &assets, int index, QString *error) {
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

    const QDir dir(assets.directory());
    dir.mkpath("assets/world");
    const QString stem = "assets/world/" + scene.layers.at(root).name.toLower().replace(QRegularExpression("[^a-z0-9]+"), "-");
    const QString depthFile = stem + "-depth.png", behindFile = stem + "-behind.png";
    if (!writeDepth(depth, dir.filePath(depthFile), error) || !writeDepth(behind, dir.filePath(behindFile), error))
        return false;
    const double focal = shape.focal;
    scene.raw["world"] = QJsonObject{
        {"picture", scene.layers.at(root).name},
        {"depth", depthFile},
        {"behind", behindFile},
        {"focal", std::round(focal * 10) / 10},
        {"ground", QJsonObject{{"up", QJsonArray{std::round(up.x() * 1e4) / 1e4, std::round(up.y() * 1e4) / 1e4,
                                                   std::round(up.z() * 1e4) / 1e4}},
                               {"height", std::round(height * 1000) / 1000}}}};
    return true;
}

}
