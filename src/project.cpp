#include "project.h"
#include "ai.h"
#include "select.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QPainterPath>
#include <QSaveFile>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <vector>

namespace {

// Big photos make a canvas no screen or encoder wants; fit the long side.
constexpr int MaxCanvas = 1920;

QString slug(const QString &text) {
    QString out;
    for (const QChar c : text.toLower())
        out += c.isLetterOrNumber() ? c : QChar('-');
    while (out.contains("--"))
        out.replace("--", "-");
    out = out.mid(out.startsWith('-') ? 1 : 0);
    if (out.endsWith('-'))
        out.chop(1);
    return out.isEmpty() ? QString("layer") : out;
}

QString uniqueFile(const QDir &dir, const QString &stem, const QString &suffix) {
    QString name = stem + "." + suffix;
    for (int n = 2; dir.exists(name); ++n)
        name = QString("%1-%2.%3").arg(stem).arg(n).arg(suffix);
    return name;
}

int even(double v) { return std::max(2, int(std::lround(v / 2)) * 2); }

Scene sceneForSize(const QSize &size, double *fit) {
    Scene scene;
    const double s = std::min(1.0, double(MaxCanvas) / std::max(size.width(), size.height()));
    scene.width = even(size.width() * s);
    scene.height = even(size.height() * s);
    *fit = s;
    return scene;
}

Layer centeredLayer(const Scene &scene, const QString &name, const QString &source, double scale) {
    Layer layer;
    layer.name = scene.uniqueName(name);
    layer.source = source;
    for (const QString &property : Scene::properties())
        layer.base.insert(property, Scene::defaultValue(property));
    layer.base["x"] = scene.width / 2.0;
    layer.base["y"] = scene.height / 2.0;
    layer.base["scale"] = scale;
    return layer;
}

bool writeImage(const QImage &image, const QString &path, QString *error) {
    QSaveFile file(path);
    // Light compression: big images save in half the time for files a
    // fifth bigger, and every cut saves at least one.
    QImageWriter writer(&file, "png");
    writer.setQuality(60);
    if (file.open(QIODevice::WriteOnly) && writer.write(image) && file.commit())
        return true;
    if (error)
        *error = QString("Couldn't write %1: %2").arg(path, file.errorString());
    return false;
}

// Keeps the file a layer already has when the pixels haven't changed, so
// redrawing a layer's pieces doesn't write every one of them again;
// otherwise writes a new file named after stem.
bool storeImage(const QImage &image, const QString &current, AssetCache &assets, const QString &stem, QString *file,
                QString *error) {
    const QDir dir(assets.directory());
    if (!current.isEmpty() && QFileInfo::exists(dir.filePath(current))) {
        const QImage existing = assets.image(current);
        if (!existing.isNull() && existing == image.convertToFormat(existing.format())) {
            *file = current;
            return true;
        }
    }
    dir.mkpath("assets");
    *file = "assets/" + uniqueFile(QDir(dir.filePath("assets")), stem, "png");
    return writeImage(image, dir.filePath(*file), error);
}

}

namespace Project {

QString sceneFile(const QString &directory) { return QDir(directory).filePath("scene.json"); }

bool isImage(const QString &path) {
    static const QStringList suffixes{"png", "jpg", "jpeg", "webp", "bmp", "gif", "tif", "tiff"};
    return suffixes.contains(QFileInfo(path).suffix().toLower());
}

QString open(const QString &path, QString *error) {
    const QFileInfo info(path);
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return QString();
    };
    if (!info.exists())
        return fail(QString("%1 doesn't exist.").arg(path));

    if (info.isFile() && info.fileName() == "scene.json")
        return info.absolutePath();

    if (info.isDir()) {
        const QDir dir(info.absoluteFilePath());
        if (QFileInfo::exists(sceneFile(dir.path())))
            return dir.path();

        QStringList images;
        for (const QString &name : dir.entryList(QDir::Files, QDir::Name))
            if (isImage(name))
                images.append(name);
        if (images.isEmpty())
            return fail(QString("%1 has no scene.json and no images to start one from.").arg(dir.path()));

        const QSize first = QImageReader(dir.filePath(images.first())).size();
        if (!first.isValid())
            return fail(QString("Couldn't read %1.").arg(dir.filePath(images.first())));
        double fit = 1;
        Scene scene = sceneForSize(first, &fit);
        for (const QString &name : images)
            scene.layers.append(centeredLayer(scene, QFileInfo(name).completeBaseName(), name, fit));
        if (!save(dir.path(), scene, error))
            return {};
        return dir.path();
    }

    if (!isImage(path))
        return fail(QString("%1 isn't an image or a project folder.").arg(path));

    const QDir parent = info.absoluteDir();
    const QString folder = info.completeBaseName() + ".omotion";
    const QString directory = parent.filePath(folder);
    if (QFileInfo::exists(sceneFile(directory)))
        return directory;

    const QSize size = QImageReader(path).size();
    if (!size.isValid())
        return fail(QString("Couldn't read %1 as an image.").arg(path));
    if (!QDir().mkpath(QDir(directory).filePath("assets")))
        return fail(QString("Couldn't create %1.").arg(directory));
    const QString source = "assets/" + info.fileName();
    if (!QFile::exists(QDir(directory).filePath(source)) &&
        !QFile::copy(path, QDir(directory).filePath(source)))
        return fail(QString("Couldn't copy %1 into %2.").arg(path, directory));

    double fit = 1;
    Scene scene = sceneForSize(size, &fit);
    scene.layers.append(centeredLayer(scene, info.completeBaseName(), source, fit));
    if (!save(directory, scene, error))
        return {};
    return directory;
}

Scene load(const QString &directory, QString *error, QByteArray *bytes) {
    QFile file(sceneFile(directory));
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QString("Couldn't read %1: %2").arg(file.fileName(), file.errorString());
        return {};
    }
    const QByteArray data = file.readAll();
    if (bytes)
        *bytes = data;

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) {
            const int line = data.left(parseError.offset).count('\n') + 1;
            *error = document.isNull() ? QString("scene.json line %1: %2").arg(line).arg(parseError.errorString())
                                       : QString("scene.json must be a JSON object.");
        }
        return {};
    }
    QString sceneError;
    Scene scene = Scene::fromJson(document.object(), &sceneError);
    if (error)
        *error = sceneError.isEmpty() ? QString() : "scene.json: " + sceneError;
    return scene;
}

QByteArray serialize(const Scene &scene) { return QJsonDocument(scene.toJson()).toJson(QJsonDocument::Indented); }

bool save(const QString &directory, const Scene &scene, QString *error, QByteArray *bytes) {
    const QByteArray data = serialize(scene);
    QSaveFile file(sceneFile(directory));
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        if (error)
            *error = QString("Couldn't save %1: %2").arg(file.fileName(), file.errorString());
        return false;
    }
    if (bytes)
        *bytes = data;
    return true;
}

namespace {

QJsonArray polygonJson(const QPolygonF &polygon) {
    QJsonArray array;
    for (const QPointF &point : polygon)
        array.append(QJsonArray{std::round(point.x() * 100) / 100, std::round(point.y() * 100) / 100});
    return array;
}

QPolygonF polygonFrom(const QJsonValue &value) {
    QPolygonF polygon;
    for (const QJsonValue &point : value.toArray()) {
        const QJsonArray xy = point.toArray();
        if (xy.size() == 2)
            polygon.append(QPointF(xy.at(0).toDouble(), xy.at(1).toDouble()));
    }
    return polygon;
}

// Canvas pixels per image pixel for a layer at a moment: how big its pixels look.
double pixelScale(const QTransform &transform) { return std::sqrt(std::abs(transform.determinant())); }

// The polygon as an 8-bit coverage mask over an area of the image, softened
// by feather pixels: three box blurs come close to a Gaussian.
QImage maskFor(const QPolygonF &polygon, double feather, const QRect &area) {
    QImage mask(area.size(), QImage::Format_Alpha8);
    mask.fill(0);
    {
        QPainter painter(&mask);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.translate(-area.topLeft());
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::black);
        painter.drawPolygon(polygon);
    }
    soften(mask, feather);
    return mask;
}

// Fills the hole a punched piece left in image (straight ARGB) inside area,
// from the pixels around it. before is area as it was before the punch; a
// hole is wherever the alpha went down. Push-pull: the known colours are
// averaged down a pyramid until they've flowed in from every side, then
// pulled back up into the gaps, each level filling what the finer one
// couldn't. Big holes are worked out smaller, as the fill is smooth anyway.
void fillHole(QImage &image, const QImage &before, const QRect &area) {
    QRect hole;
    for (int y = 0; y < area.height(); ++y) {
        const QRgb *now = reinterpret_cast<const QRgb *>(image.constScanLine(area.y() + y)) + area.x();
        const QRgb *was = reinterpret_cast<const QRgb *>(before.constScanLine(y));
        for (int x = 0; x < area.width(); ++x)
            if (qAlpha(now[x]) < qAlpha(was[x]))
                hole |= QRect(area.x() + x, area.y() + y, 1, 1);
    }
    if (hole.isEmpty())
        return;

    const int margin = std::max(16, std::max(hole.width(), hole.height()) / 2);
    const QRect crop = hole.adjusted(-margin, -margin, margin, margin) & image.rect();
    const double s = std::min(1.0, std::sqrt(2e6 / (double(crop.width()) * crop.height())));
    const QSize work(std::max(1, int(crop.width() * s)), std::max(1, int(crop.height() * s)));

    // What's known: everything around that isn't hole, weighted by alpha.
    QImage holeMask(crop.size(), QImage::Format_Alpha8);
    holeMask.fill(0);
    for (int y = hole.top(); y <= hole.bottom(); ++y) {
        const QRgb *now = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        uchar *maskLine = holeMask.scanLine(y - crop.y());
        for (int x = hole.left(); x <= hole.right(); ++x) {
            const QPoint inArea(x - area.x(), y - area.y());
            if (area.contains(x, y) && qAlpha(now[x]) < qAlpha(before.pixel(inArea)))
                maskLine[x - crop.x()] = 255;
        }
    }
    QImage known = image.copy(crop).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (work != crop.size()) {
        known = known.scaled(work, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        holeMask = holeMask.scaled(work, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }

    struct Level {
        int w, h;
        std::vector<float> c, wt;  // Colour times weight (r, g, b), and weight.
    };
    std::vector<Level> levels;
    levels.push_back({work.width(), work.height(), std::vector<float>(3 * work.width() * work.height()),
                      std::vector<float>(work.width() * work.height())});
    for (int y = 0; y < work.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(known.constScanLine(y));
        const uchar *maskLine = holeMask.constScanLine(y);
        for (int x = 0; x < work.width(); ++x) {
            if (maskLine[x] > 0)
                continue;
            const int i = y * work.width() + x;
            levels[0].c[3 * i] = qRed(line[x]) / 255.f;  // Premultiplied is already colour times alpha.
            levels[0].c[3 * i + 1] = qGreen(line[x]) / 255.f;
            levels[0].c[3 * i + 2] = qBlue(line[x]) / 255.f;
            levels[0].wt[i] = qAlpha(line[x]) / 255.f;
        }
    }
    while (levels.back().w > 1 || levels.back().h > 1) {
        const Level &fine = levels.back();
        Level coarse{(fine.w + 1) / 2, (fine.h + 1) / 2, {}, {}};
        coarse.c.assign(3 * coarse.w * coarse.h, 0);
        coarse.wt.assign(coarse.w * coarse.h, 0);
        for (int y = 0; y < fine.h; ++y)
            for (int x = 0; x < fine.w; ++x) {
                const int i = y * fine.w + x, j = (y / 2) * coarse.w + x / 2;
                for (int k = 0; k < 3; ++k)
                    coarse.c[3 * j + k] += fine.c[3 * i + k];
                coarse.wt[j] += fine.wt[i];
            }
        for (int j = 0; j < coarse.w * coarse.h; ++j)
            if (coarse.wt[j] > 1) {
                for (int k = 0; k < 3; ++k)
                    coarse.c[3 * j + k] /= coarse.wt[j];
                coarse.wt[j] = 1;
            }
        levels.push_back(std::move(coarse));
    }
    if (levels.back().wt[0] <= 0)
        return;  // Nothing around to fill from.
    for (int k = 0; k < 3; ++k)
        levels.back().c[k] /= levels.back().wt[0];
    levels.back().wt[0] = 1;

    for (int l = int(levels.size()) - 2; l >= 0; --l) {
        Level &fine = levels[l];
        const Level &coarse = levels[l + 1];
        for (int y = 0; y < fine.h; ++y) {
            const float cy = std::clamp((y + .5f) / 2 - .5f, 0.f, float(coarse.h - 1));
            const int y0 = int(cy), y1 = std::min(y0 + 1, coarse.h - 1);
            const float fy = cy - y0;
            for (int x = 0; x < fine.w; ++x) {
                const int i = y * fine.w + x;
                const float rest = 1 - std::min(1.f, fine.wt[i]);
                if (rest <= 0) {
                    for (int k = 0; k < 3; ++k)
                        fine.c[3 * i + k] /= fine.wt[i];
                    fine.wt[i] = 1;
                    continue;
                }
                const float cx = std::clamp((x + .5f) / 2 - .5f, 0.f, float(coarse.w - 1));
                const int x0 = int(cx), x1 = std::min(x0 + 1, coarse.w - 1);
                const float fx = cx - x0;
                for (int k = 0; k < 3; ++k) {
                    const float top = coarse.c[3 * (y0 * coarse.w + x0) + k] * (1 - fx) + coarse.c[3 * (y0 * coarse.w + x1) + k] * fx;
                    const float bottom = coarse.c[3 * (y1 * coarse.w + x0) + k] * (1 - fx) + coarse.c[3 * (y1 * coarse.w + x1) + k] * fx;
                    fine.c[3 * i + k] = fine.c[3 * i + k] + rest * (top * (1 - fy) + bottom * fy);
                }
                fine.wt[i] = 1;
            }
        }
    }

    // Lay the fill into the hole, bringing the alpha back to what it was.
    const Level &fill = levels[0];
    for (int y = hole.top(); y <= hole.bottom(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        const QRgb *was = reinterpret_cast<const QRgb *>(before.constScanLine(y - area.y()));
        const float wy = std::clamp((y - crop.y() + .5f) * float(s) - .5f, 0.f, float(fill.h - 1));
        const int y0 = int(wy), y1 = std::min(y0 + 1, fill.h - 1);
        const float fy = wy - y0;
        for (int x = hole.left(); x <= hole.right(); ++x) {
            if (!area.contains(x, y))
                continue;
            const QRgb old = was[x - area.x()];
            const int a = qAlpha(old), h = qAlpha(line[x]);
            if (h >= a)
                continue;
            const float wx = std::clamp((x - crop.x() + .5f) * float(s) - .5f, 0.f, float(fill.w - 1));
            const int x0 = int(wx), x1 = std::min(x0 + 1, fill.w - 1);
            const float fx = wx - x0;
            int out[3];
            const int original[3] = {qRed(old), qGreen(old), qBlue(old)};
            for (int k = 0; k < 3; ++k) {
                const float top = fill.c[3 * (y0 * fill.w + x0) + k] * (1 - fx) + fill.c[3 * (y0 * fill.w + x1) + k] * fx;
                const float bottom = fill.c[3 * (y1 * fill.w + x0) + k] * (1 - fx) + fill.c[3 * (y1 * fill.w + x1) + k] * fx;
                const float filled = std::clamp((top * (1 - fy) + bottom * fy) * 255.f, 0.f, 255.f);
                out[k] = int(std::lround((h * original[k] + (a - h) * filled) / a));
            }
            line[x] = qRgba(out[0], out[1], out[2], a);
        }
    }
}

// Pieces cut from a layer, oldest first.
QList<int> piecesOf(const Scene &scene, const QString &name) {
    QList<int> pieces;
    for (int i = 0; i < scene.layers.size(); ++i)
        if (scene.layers.at(i).raw.value("cut").toObject().value("from").toString() == name)
            pieces.append(i);
    std::stable_sort(pieces.begin(), pieces.end(), [&](int a, int b) {
        return scene.layers.at(a).raw.value("cut").toObject().value("order").toInt() <
               scene.layers.at(b).raw.value("cut").toObject().value("order").toInt();
    });
    return pieces;
}

// Redraws every piece cut from a layer, and the layer's holes, from the image
// it had before any cuts ("uncut"). Each punched piece takes its pixels out
// of what the earlier ones left. An unfilled hole is worked out so that
// piece over hole is exactly the original: with a soft edge, the hole's
// alpha at a pixel is a(1 - m) / (1 - a·m) for image alpha a and mask m.
bool rebuildCuts(Scene &scene, AssetCache &assets, int index, QString *error, int depth = 0) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (depth > 16)
        return fail("Pieces are cut from each other too deeply.");
    Layer &source = scene.layers[index];
    const QString root = source.raw.value("uncut").toString(source.source);
    QImage remaining = assets.image(root).convertToFormat(QImage::Format_ARGB32);
    if (remaining.isNull())
        return fail(QString("Couldn't read %1.").arg(root));

    const QDir dir(assets.directory());
    dir.mkpath("assets");
    const QDir assetDir(dir.filePath("assets"));
    bool punched = false;

    for (const int i : piecesOf(scene, source.name)) {
        Layer &piece = scene.layers[i];
        QJsonObject cut = piece.raw.value("cut").toObject();
        const QPolygonF polygon = polygonFrom(cut.value("polygon"));
        const double feather = std::max(0.0, cut.value("feather").toDouble());
        const bool punch = cut.value("punch").toBool(true);
        const bool fill = punch && cut.value("fill").toBool(false);
        const int margin = int(std::ceil(feather));
        // A shape is an outline, or for shapes like depth layers that come in
        // many patches, a mask image in the source's pixels.
        const QString maskFile = cut.value("mask").toString();
        QImage stored;
        QPoint storedAt;
        QRect shape;
        if (!maskFile.isEmpty()) {
            stored = QImage(dir.filePath(maskFile)).convertToFormat(QImage::Format_Grayscale8);
            if (stored.isNull())
                return fail(QString("Can't read %1, the shape of %2.").arg(maskFile, piece.name));
            const QJsonArray at = cut.value("maskOrigin").toArray();
            storedAt = QPoint(at.at(0).toInt(), at.at(1).toInt());
            shape = QRect(storedAt, stored.size());
        } else if (polygon.size() >= 3) {
            shape = polygon.boundingRect().toAlignedRect();
        }
        const QRect area = shape.adjusted(-margin, -margin, margin, margin) & remaining.rect();
        if (area.isEmpty())
            return fail(QString("The shape of %1 doesn't cover any of %2.").arg(piece.name, source.name));

        QImage mask;
        if (!stored.isNull()) {
            mask = QImage(area.size(), QImage::Format_Alpha8);
            mask.fill(0);
            for (int y = 0; y < stored.height(); ++y) {
                const int ay = storedAt.y() + y - area.y();
                if (ay < 0 || ay >= area.height())
                    continue;
                const uchar *from = stored.constScanLine(y);
                uchar *to = mask.scanLine(ay);
                for (int x = 0; x < stored.width(); ++x) {
                    const int ax = storedAt.x() + x - area.x();
                    if (ax >= 0 && ax < area.width())
                        to[ax] = from[x];
                }
            }
            soften(mask, feather);
        } else {
            mask = maskFor(polygon, feather, area);
        }
        const QImage before = fill ? remaining.copy(area) : QImage();
        QImage image(area.size(), QImage::Format_ARGB32);
        bool opaque = false;
        for (int y = 0; y < area.height(); ++y) {
            QRgb *from = reinterpret_cast<QRgb *>(remaining.scanLine(area.y() + y)) + area.x();
            QRgb *to = reinterpret_cast<QRgb *>(image.scanLine(y));
            const uchar *coverage = mask.constScanLine(y);
            for (int x = 0; x < area.width(); ++x) {
                const QRgb pixel = from[x];
                const int a = qAlpha(pixel), m = coverage[x];
                const int pieceAlpha = (a * m + 127) / 255;
                to[x] = qRgba(qRed(pixel), qGreen(pixel), qBlue(pixel), pieceAlpha);
                opaque |= pieceAlpha > 0;
                if (punch && m > 0) {
                    // Unfilled, the hole is exact: piece over hole is the
                    // original even across a soft edge. Filled, it takes the
                    // whole edge too, so no fringe of the piece stays behind
                    // when it moves; at rest the edge then mixes in a little
                    // of the fill.
                    const double af = a / 255.0, mf = m / 255.0, rest = 1 - af * mf;
                    const double hole = fill ? af * (1 - mf) : (rest <= 1e-6 ? 0 : af * (1 - mf) / rest);
                    from[x] = qRgba(qRed(pixel), qGreen(pixel), qBlue(pixel), int(std::lround(hole * 255)));
                }
            }
        }
        if (!opaque)
            return fail(QString("The shape of %1 only covers transparent pixels.").arg(piece.name));
        if (fill)
            fillHole(remaining, before, area);

        // A reshaped piece keeps its pivot on the same spot of the picture,
        // so its position and every keyframe still mean what they did.
        const QJsonArray origin = cut.value("origin").toArray();
        const QSizeF oldSize = assets.size(piece.source);
        if (origin.size() == 2 && !oldSize.isEmpty()) {
            const QPointF pivot = QPointF(origin.at(0).toDouble(), origin.at(1).toDouble()) +
                                  QPointF(piece.anchor.x() * oldSize.width(), piece.anchor.y() * oldSize.height());
            piece.anchor = QPointF((pivot.x() - area.x()) / area.width(), (pivot.y() - area.y()) / area.height());
        } else {
            // New, so it sits where it was cut from, turning about its centre.
            piece.anchor = QPointF(.5, .5);
            piece.base["x"] = QRectF(area).center().x();
            piece.base["y"] = QRectF(area).center().y();
        }
        cut["origin"] = QJsonArray{area.x(), area.y()};
        piece.raw["cut"] = cut;

        const bool hasPieces = !piecesOf(scene, piece.name).isEmpty();
        QString file;
        if (!storeImage(image, hasPieces ? piece.raw.value("uncut").toString(piece.source) : piece.source, assets,
                        slug(piece.name), &file, error))
            return false;
        piece.source = file;
        if (hasPieces) {
            scene.layers[i].raw["uncut"] = file;
            if (!rebuildCuts(scene, assets, i, error, depth + 1))
                return false;
        }
        punched |= punch;
    }

    Layer &updated = scene.layers[index];
    if (punched) {
        QString file;
        if (!storeImage(remaining, updated.source != root ? updated.source : QString(), assets, slug(updated.name), &file,
                        error))
            return false;
        updated.raw["uncut"] = root;
        updated.source = file;
    } else {
        updated.source = root;
    }
    return true;
}

}

int cutOut(Scene &scene, AssetCache &assets, int index, const QPolygonF &polygon, double t,
           bool punch, QString *error, const QString &wantedName, double feather, bool fill) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return -1;
    };
    if (index < 0 || index >= scene.layers.size())
        return fail("Select the layer to cut a piece out of.");
    if (polygon.size() < 3)
        return fail("Click at least three points around the piece.");
    if (!wantedName.isEmpty() && scene.indexOf(wantedName) >= 0)
        return fail(QString("There's already a layer named \"%1\".").arg(wantedName));
    const Layer &source = scene.layers.at(index);
    if (assets.image(source.source).isNull())
        return fail(QString("Couldn't read %1.").arg(source.source));
    const QTransform transform = layerTransform(scene, assets, index, t);
    bool invertible = false;
    const QTransform inverse = transform.inverted(&invertible);
    if (!invertible)
        return fail("The layer is scaled to nothing at this point in time.");

    int order = 0;
    for (const int i : piecesOf(scene, source.name))
        order = std::max(order, scene.layers.at(i).raw.value("cut").toObject().value("order").toInt() + 1);

    // The piece lives in its source layer's pixel space and moves with it.
    Layer layer;
    layer.name = wantedName.isEmpty() ? scene.uniqueName(source.name + " piece") : wantedName;
    layer.parent = source.name;
    for (const QString &property : Scene::properties())
        layer.base.insert(property, Scene::defaultValue(property));
    layer.raw["cut"] = QJsonObject{{"from", source.name},
                                   {"polygon", polygonJson(inverse.map(polygon))},
                                   {"feather", std::round(feather / pixelScale(transform) * 100) / 100},
                                   {"punch", punch},
                                   {"fill", punch && fill},
                                   {"order", order}};

    Scene edited = scene;
    edited.layers.insert(index + 1, layer);
    if (!rebuildCuts(edited, assets, index, error))
        return -1;
    scene = edited;
    if (error)
        error->clear();
    return index + 1;
}

bool reshapeCut(Scene &scene, AssetCache &assets, int index, const QPolygonF &polygon, double t,
                double feather, QString *error, int fill) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (index < 0 || index >= scene.layers.size())
        return fail("Select the piece to reshape.");
    if (polygon.size() < 3)
        return fail("A shape needs at least three points.");
    QJsonObject cut = scene.layers.at(index).raw.value("cut").toObject();
    const int from = scene.indexOf(cut.value("from").toString());
    if (from < 0)
        return fail(QString("%1 was cut from \"%2\", which is gone, so it can't be reshaped.")
                        .arg(scene.layers.at(index).name, cut.value("from").toString()));
    const QTransform transform = layerTransform(scene, assets, from, t);
    bool invertible = false;
    const QTransform inverse = transform.inverted(&invertible);
    if (!invertible)
        return fail("The layer it was cut from is scaled to nothing at this point in time.");

    cut["polygon"] = polygonJson(inverse.map(polygon));
    cut["feather"] = std::round(feather / pixelScale(transform) * 100) / 100;
    if (fill >= 0)
        cut["fill"] = fill > 0 && cut.value("punch").toBool(true);
    Scene edited = scene;
    edited.layers[index].raw["cut"] = cut;
    if (!rebuildCuts(edited, assets, from, error))
        return false;
    scene = edited;
    return true;
}

bool isCut(const Scene &scene, int index) {
    return index >= 0 && index < scene.layers.size() &&
           scene.layers.at(index).raw.value("cut").toObject().contains("polygon");
}

QPolygonF cutOutline(const Scene &scene, AssetCache &assets, int index, double t, double *feather, bool *fill) {
    if (!isCut(scene, index))
        return {};
    const QJsonObject cut = scene.layers.at(index).raw.value("cut").toObject();
    const int from = scene.indexOf(cut.value("from").toString());
    if (from < 0)
        return {};
    const QTransform transform = layerTransform(scene, assets, from, t);
    if (feather)
        *feather = cut.value("feather").toDouble() * pixelScale(transform);
    if (fill)
        *fill = cut.value("fill").toBool(false);
    return transform.map(polygonFrom(cut.value("polygon")));
}

QPolygonF smartOutline(const Scene &scene, AssetCache &assets, int index, const QList<QPointF> &points, double t,
                       int tolerance, int *regions, int *used, QList<QImage> *levels) {
    if (regions)
        *regions = 0;
    if (index < 0 || index >= scene.layers.size())
        return {};
    const QString source = scene.layers.at(index).source;
    const QSizeF full = assets.size(source);
    if (full.isEmpty())
        return {};
    // Big images are searched at around 2000 pixels across: plenty for an
    // outline, and quick enough to follow a click.
    double factor = 1;
    const QImage image = assets.mip(source, std::min(1.0, 2048.0 / std::max(full.width(), full.height())), &factor);
    const QTransform toCanvas = layerTransform(scene, assets, index, t);
    bool invertible = false;
    const QTransform toImage = toCanvas.inverted(&invertible);
    if (!invertible)
        return {};

    QList<QImage> computed;
    QList<QImage> &searches = levels ? *levels : computed;
    if (searches.size() > points.size())
        searches.clear();
    for (int i = searches.size(); i < points.size(); ++i)
        searches.append(Select::joinLevels(image, (toImage.map(points.at(i)) * factor).toPoint()));

    QImage mask;
    for (const QImage &search : searches) {
        const int at = tolerance >= 0 ? tolerance : Select::autoTolerance(search);
        if (used)
            *used = at;
        const QImage area = Select::regionAt(search, at);
        if (Select::regionCount(area) == 0)
            continue;
        if (mask.isNull())
            mask = area;
        else
            Select::unite(mask, area);
    }
    if (mask.isNull())
        return {};
    // About a canvas pixel and a half more, for the blended edge.
    Select::grow(mask, std::max(1, int(std::lround(1.5 * factor * pixelScale(toImage)))));
    if (regions)
        *regions = Select::regionCount(mask);
    QPolygonF outline;
    for (const QPointF &point : Select::outline(mask, 1.0))
        outline.append(toCanvas.map(point / factor));
    return outline;
}

QPolygonF aiOutline(const Scene &scene, AssetCache &assets, int index, const QList<QPointF> &take,
                    const QList<QPointF> &leave, double t, int choice, int *choices, QString *error) {
    if (choices)
        *choices = 0;
    if (index < 0 || index >= scene.layers.size() || take.isEmpty())
        return {};
    const QString source = scene.layers.at(index).source;
    const QSizeF full = assets.size(source);
    if (full.isEmpty())
        return {};
    // The AI looks at a copy around 2000 pixels across; its masks come back
    // that size, which is detail enough for an outline.
    double factor = 1;
    const QImage image = assets.mip(source, std::min(1.0, 2048.0 / std::max(full.width(), full.height())), &factor);
    const QTransform toCanvas = layerTransform(scene, assets, index, t);
    bool invertible = false;
    const QTransform toImage = toCanvas.inverted(&invertible);
    if (!invertible)
        return {};
    QList<QPointF> points;
    QList<int> labels;
    for (const QPointF &point : take) {
        points.append(toImage.map(point) * factor);
        labels.append(1);
    }
    for (const QPointF &point : leave) {
        points.append(toImage.map(point) * factor);
        labels.append(0);
    }
    const QString key = QString("%1|%2|%3").arg(QDir(assets.directory()).filePath(source)).arg(
        QFileInfo(QDir(assets.directory()).filePath(source)).lastModified().toMSecsSinceEpoch()).arg(factor);
    const QList<QImage> masks = Ai::instance().segment(image, key, points, labels, error);
    if (masks.isEmpty())
        return {};
    if (choices)
        *choices = int(masks.size());
    QImage mask = masks.at(std::clamp(choice, 0, int(masks.size()) - 1));
    // The AI's edge sits a touch inside soft outlines; take the blend too.
    Select::grow(mask, std::max(1, int(std::lround(1.5 * factor * pixelScale(toImage)))));
    QPolygonF outline;
    for (const QPointF &point : Select::outline(mask, 1.0))
        outline.append(toCanvas.map(point / factor));
    return outline;
}

QImage depthOf(const Scene &scene, AssetCache &assets, int index, QString *error) {
    if (index < 0 || index >= scene.layers.size())
        return {};
    const QString source = scene.layers.at(index).source;
    const QSizeF full = assets.size(source);
    if (full.isEmpty()) {
        if (error)
            *error = QString("Couldn't read %1.").arg(source);
        return {};
    }
    // The model works at about 518 pixels; there's no need to hand it more
    // than a few times that.
    double factor = 1;
    const QImage image = assets.mip(source, std::min(1.0, 1600.0 / std::max(full.width(), full.height())), &factor);
    return Ai::instance().depth(image, error);
}

QList<int> splitByDepth(Scene &scene, AssetCache &assets, int index, const QImage &depthMap, int count, double feather,
                        QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return QList<int>();
    };
    if (index < 0 || index >= scene.layers.size())
        return fail("Select the layer to split.");
    if (count < 2 || count > 8)
        return fail("Split into between 2 and 8 layers.");
    const Layer &source = scene.layers.at(index);
    const QSize full = assets.size(source.source).toSize();
    if (full.isEmpty())
        return fail(QString("Couldn't read %1.").arg(source.source));
    if (!piecesOf(scene, source.name).isEmpty())
        return fail(QString("%1 already has pieces cut from it; split a layer that hasn't been cut yet.").arg(source.name));
    const QImage depth = depthMap.convertToFormat(QImage::Format_Grayscale16);
    const int w = depth.width(), h = depth.height();
    if (depth.isNull())
        return fail("There's no depth to split by.");

    // Where to split: k-means on the depth histogram, so the cuts fall in
    // the gaps between near, middle and far rather than at fixed steps.
    std::vector<double> histogram(1024, 0);
    for (int y = 0; y < h; ++y) {
        const quint16 *line = reinterpret_cast<const quint16 *>(depth.constScanLine(y));
        for (int x = 0; x < w; ++x)
            ++histogram[line[x] >> 6];
    }
    std::vector<double> centres(count);
    {
        double total = 0, seen = 0;
        for (double v : histogram)
            total += v;
        int next = 0;
        for (int bin = 0; bin < 1024 && next < count; ++bin) {
            seen += histogram[bin];
            while (next < count && seen >= total * (next + .5) / count)
                centres[next++] = bin;
        }
        for (int iteration = 0; iteration < 50; ++iteration) {
            std::vector<double> sum(count, 0), weight(count, 0);
            for (int bin = 0; bin < 1024; ++bin) {
                int nearest = 0;
                for (int c = 1; c < count; ++c)
                    if (std::abs(bin - centres[c]) < std::abs(bin - centres[nearest]))
                        nearest = c;
                sum[nearest] += histogram[bin] * bin;
                weight[nearest] += histogram[bin];
            }
            for (int c = 0; c < count; ++c)
                if (weight[c] > 0)
                    centres[c] = sum[c] / weight[c];
        }
        std::sort(centres.begin(), centres.end());
    }

    const QDir dir(assets.directory());
    dir.mkpath("assets");
    const QDir assetDir(dir.filePath("assets"));
    const double sourceScale = std::max(1e-6, pixelScale(layerTransform(scene, assets, index, 0)));
    Scene edited = scene;
    QList<int> created;
    // Nearest first: each nearer layer is lifted out and its place filled
    // before the next is cut, and each layer's shape takes in everything in
    // front of it too, so it carries a filled-in background behind the nearer
    // ones and nothing opens up when they move.
    for (int band = count - 1, order = 0; band >= 1; --band, ++order) {
        const double threshold = (centres[band - 1] + centres[band]) / 2 * 64;
        QImage mask(w, h, QImage::Format_Alpha8);
        for (int y = 0; y < h; ++y) {
            const quint16 *line = reinterpret_cast<const quint16 *>(depth.constScanLine(y));
            uchar *to = mask.scanLine(y);
            for (int x = 0; x < w; ++x)
                to[x] = line[x] >= threshold ? 255 : 0;
        }
        // Specks and pinholes make poor layers; drop the one, fill the other.
        const int speck = std::max(4, w * h / 1000);
        for (int pass = 0; pass < 2; ++pass) {
            if (pass == 1)
                mask.invertPixels();
            // Flood each area of the current colour; small ones flip.
            std::vector<uchar> seen(size_t(w) * h, 0);
            for (int start = 0; start < w * h; ++start) {
                if (seen[start] || !mask.constScanLine(start / w)[start % w])
                    continue;
                std::vector<int> area{start};
                seen[start] = 1;
                for (size_t k = 0; k < area.size(); ++k) {
                    const int x = area[k] % w, y = area[k] / w;
                    const int next[4][2] = {{x + 1, y}, {x - 1, y}, {x, y + 1}, {x, y - 1}};
                    for (const auto &n : next) {
                        if (n[0] < 0 || n[1] < 0 || n[0] >= w || n[1] >= h)
                            continue;
                        const int j = n[1] * w + n[0];
                        if (!seen[j] && mask.constScanLine(n[1])[n[0]]) {
                            seen[j] = 1;
                            area.push_back(j);
                        }
                    }
                }
                if (int(area.size()) < speck)
                    for (const int i : area)
                        mask.scanLine(i / w)[i % w] = 0;
            }
            if (pass == 1)
                mask.invertPixels();
        }

        // Up to the picture's size, smoothly, then crisp again; the soft edge
        // comes from the cut's feather.
        QImage grey(w, h, QImage::Format_Grayscale8);
        for (int y = 0; y < h; ++y)
            std::copy_n(mask.constScanLine(y), w, grey.scanLine(y));
        QImage big = grey.scaled(full, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        QRect bounds;
        for (int y = 0; y < big.height(); ++y) {
            uchar *line = big.scanLine(y);
            int first = -1, last = -1;
            for (int x = 0; x < big.width(); ++x) {
                line[x] = line[x] >= 128 ? 255 : 0;
                if (line[x]) {
                    if (first < 0)
                        first = x;
                    last = x;
                }
            }
            if (first >= 0)
                bounds |= QRect(first, y, last - first + 1, 1);
        }
        if (bounds.isEmpty())
            continue;
        static const QStringList names{"foreground", "middle ground", "background"};
        const QString name = edited.uniqueName(order < names.size() ? names.at(order) : QString("depth %1").arg(band));
        const QString maskFile = "assets/" + uniqueFile(assetDir, slug(name) + "-shape", "png");
        if (!writeImage(big.copy(bounds), dir.filePath(maskFile), error))
            return {};

        Layer layer;
        layer.name = name;
        layer.parent = source.name;
        for (const QString &property : Scene::properties())
            layer.base.insert(property, Scene::defaultValue(property));
        layer.raw["cut"] = QJsonObject{{"from", source.name},
                                       {"mask", maskFile},
                                       {"maskOrigin", QJsonArray{bounds.x(), bounds.y()}},
                                       {"feather", std::round(feather / sourceScale * 100) / 100},
                                       {"punch", true},
                                       {"fill", true},
                                       {"order", order},
                                       {"depth", std::round(centres[band] / 1023 * 1000) / 1000}};
        edited.layers.insert(index + 1, layer);
        // Each new layer goes in just above the source, under the nearer ones.
        created.append(index + 1 + int(created.size()));
    }
    if (created.isEmpty())
        return fail("The picture is all one depth; there's nothing to split.");
    edited.layers[index].raw["depth"] = std::round(centres[0] / 1023 * 1000) / 1000;
    if (!rebuildCuts(edited, assets, index, error))
        return {};
    scene = edited;
    if (error)
        error->clear();
    return created;
}

bool animateParallax(Scene &scene, AssetCache &assets, int index, double strength, QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (index < 0 || index >= scene.layers.size())
        return fail("Select the layer that was split by depth.");
    // Pieces carry their source's name, so a piece selected means its source.
    const QString from = scene.layers.at(index).raw.value("cut").toObject().value("from").toString();
    if (!scene.layers.at(index).raw.contains("depth") && scene.indexOf(from) >= 0)
        index = scene.indexOf(from);
    Layer &source = scene.layers[index];
    QList<int> pieces;
    for (const int i : piecesOf(scene, source.name))
        if (scene.layers.at(i).raw.value("cut").toObject().contains("depth"))
            pieces.append(i);
    if (pieces.isEmpty())
        return fail(QString("%1 hasn't been split by depth (D), so there's no depth to move by.").arg(source.name));

    const double far = source.raw.value("depth").toDouble(0);
    // A camera move this slow wants room; a short scene gets longer.
    scene.duration = std::max(scene.duration, 10.0);
    const double d = scene.duration;
    auto loop = [d](double a, double b) {
        return QList<Key>{Key{0, a, "inOut"}, Key{d / 2, b, "inOut"}, Key{d, a, "inOut"}};
    };
    // The camera drifts and pushes in a little; it's scaled up enough that the
    // nearest layer, which moves furthest, never shows an edge.
    const double base = scene.value(source, "scale", 0);
    const double x = scene.value(source, "x", 0);
    const double overscan = 1 + 2 * (strength * 4 / 3) / scene.width + .01;
    source.keys["scale"] = loop(base * overscan, base * overscan * 1.035);
    source.keys["x"] = loop(x, x - strength / 3);
    const double perPixel = 1 / std::max(1e-6, pixelScale(layerTransform(scene, assets, index, 0)) * overscan);
    for (const int i : pieces) {
        Layer &piece = scene.layers[i];
        const double depth = piece.raw.value("cut").toObject().value("depth").toDouble();
        const double shift = strength * std::clamp((depth - far) / std::max(1e-6, 1 - far), 0.0, 1.0);
        const double px = scene.value(piece, "x", 0);
        piece.keys["x"] = loop(px, px - shift * perPixel);
    }
    return true;
}

namespace {
// Every "assets/..." string anywhere in some JSON, so fields omotion doesn't
// know about keep their files too.
void collectAssets(const QJsonValue &value, QSet<QString> &into) {
    if (value.isString() && value.toString().startsWith("assets/"))
        into.insert(QDir::cleanPath(value.toString()));
    else if (value.isArray())
        for (const QJsonValue &item : value.toArray())
            collectAssets(item, into);
    else if (value.isObject())
        for (const QJsonValue &item : value.toObject())
            collectAssets(item, into);
}
}

QStringList unusedAssets(const QString &directory, const QList<QJsonObject> &scenes) {
    QSet<QString> used;
    for (const QJsonObject &scene : scenes)
        collectAssets(scene, used);
    QStringList unused;
    const QDir assets(QDir(directory).filePath("assets"));
    for (const QString &name : assets.entryList(QDir::Files, QDir::Name))
        if (isImage(name) && !used.contains("assets/" + name))
            unused.append("assets/" + name);
    return unused;
}

qint64 cleanAssets(const QString &directory, const QList<QJsonObject> &scenes, int *count, QString *error) {
    qint64 freed = 0;
    int moved = 0;
    for (const QString &file : unusedAssets(directory, scenes)) {
        const QString path = QDir(directory).filePath(file);
        const qint64 size = QFileInfo(path).size();
        if (!QFile::moveToTrash(path)) {
            if (error)
                *error = QString("Couldn't move %1 to the trash.").arg(file);
            if (count)
                *count = moved;
            return moved ? freed : -1;
        }
        freed += size;
        ++moved;
    }
    if (count)
        *count = moved;
    return freed;
}

void renameCutSource(Scene &scene, const QString &from, const QString &to) {
    for (Layer &layer : scene.layers) {
        QJsonObject cut = layer.raw.value("cut").toObject();
        if (cut.value("from").toString() == from) {
            cut["from"] = to;
            layer.raw["cut"] = cut;
        }
    }
}

void setPivot(Scene &scene, AssetCache &assets, int index, const QPointF &point, double t) {
    if (index < 0 || index >= scene.layers.size())
        return;
    const Layer &layer = scene.layers.at(index);
    const QSizeF size = assets.size(layer.source);
    bool invertible = false;
    const QTransform inverse = layerTransform(scene, assets, index, t).inverted(&invertible);
    if (size.isEmpty() || !invertible)
        return;

    // Keeping the image still means moving the position by however far the
    // old pivot was from the new one, turned and scaled as the layer is.
    const QPointF pixel = inverse.map(point);
    const QPointF oldAnchor(layer.anchor.x() * size.width(), layer.anchor.y() * size.height());
    const double scale = scene.value(layer, "scale", t);
    QTransform rs;
    rs.rotate(scene.value(layer, "rotation", t));
    rs.scale(scale * scene.value(layer, "scaleX", t), scale * scene.value(layer, "scaleY", t));
    const QPointF delta = rs.map(pixel - oldAnchor);

    Layer &edited = scene.layers[index];
    edited.anchor = QPointF(pixel.x() / size.width(), pixel.y() / size.height());
    edited.base["x"] += delta.x();
    edited.base["y"] += delta.y();
    for (const QString &property : {QString("x"), QString("y")}) {
        auto it = edited.keys.find(property);
        if (it == edited.keys.end())
            continue;
        for (Key &key : *it)
            key.v += property == "x" ? delta.x() : delta.y();
    }
}

int importImage(Scene &scene, const QString &directory, const QString &path, QString *error) {
    const QSize size = QImageReader(path).size();
    if (!size.isValid()) {
        if (error)
            *error = QString("Couldn't read %1 as an image.").arg(path);
        return -1;
    }
    const QDir dir(directory);
    dir.mkpath("assets");
    const QFileInfo info(path);
    const QString source = "assets/" + uniqueFile(QDir(dir.filePath("assets")), info.completeBaseName(),
                                                  info.suffix());
    if (!QFile::copy(path, dir.filePath(source))) {
        if (error)
            *error = QString("Couldn't copy %1 into the project.").arg(path);
        return -1;
    }
    const double fit = std::min(1.0, std::min(double(scene.width) / size.width(),
                                              double(scene.height) / size.height()));
    scene.layers.append(centeredLayer(scene, info.completeBaseName(), source, fit));
    return scene.layers.size() - 1;
}

}
