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
#include <QProcess>
#include <QStandardPaths>
#include <QSaveFile>
#include <QSet>
#include <QUrl>
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
// Lays fill colours into the hole: each hole pixel gets back the alpha it
// had, its colour mixing what's left of the original with the fill.
template <typename FillAt>
void layFill(QImage &image, const QImage &before, const QRect &area, const QRect &hole, FillAt fillAt) {
    for (int y = hole.top(); y <= hole.bottom(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        const QRgb *was = reinterpret_cast<const QRgb *>(before.constScanLine(y - area.y()));
        for (int x = hole.left(); x <= hole.right(); ++x) {
            const QRgb old = was[x - area.x()];
            const int a = qAlpha(old), h = qAlpha(line[x]);
            if (h >= a)
                continue;
            const QColor filled = fillAt(x, y);
            const int fill[3] = {filled.red(), filled.green(), filled.blue()};
            const int original[3] = {qRed(old), qGreen(old), qBlue(old)};
            int out[3];
            for (int k = 0; k < 3; ++k)
                out[k] = int(std::lround((h * original[k] + (a - h) * fill[k]) / double(a)));
            line[x] = qRgba(out[0], out[1], out[2], a);
        }
    }
}

// The AI's fill: the area around the hole, about three times its size, is
// handed to LaMa with the hole (grown a little, so no edge of what was cut
// out is left to copy) blanked. False when the AI can't, to fall back.
bool fillHoleWithAi(QImage &image, const QImage &before, const QRect &area, const QRect &hole) {
    if (!Ai::instance().canInpaint())
        return false;
    const int side = std::min({std::max({hole.width(), hole.height()}) * 3, image.width(), image.height()});
    QRect crop(0, 0, std::max(side, std::min({64, image.width(), image.height()})),
               std::max(side, std::min({64, image.width(), image.height()})));
    crop.moveCenter(hole.center());
    crop.moveLeft(std::clamp(crop.left(), 0, image.width() - crop.width()));
    crop.moveTop(std::clamp(crop.top(), 0, image.height() - crop.height()));
    QImage mask(crop.size(), QImage::Format_Alpha8);
    mask.fill(0);
    for (int y = hole.top(); y <= hole.bottom(); ++y) {
        const QRgb *now = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        const QRgb *was = reinterpret_cast<const QRgb *>(before.constScanLine(y - area.y()));
        for (int x = hole.left(); x <= hole.right(); ++x)
            if (crop.contains(x, y) && qAlpha(now[x]) < qAlpha(was[x - area.x()]))
                mask.scanLine(y - crop.y())[x - crop.x()] = 255;
    }
    Select::grow(mask, std::max(2, crop.width() / 170));
    QImage filled = Ai::instance().inpaint(image.copy(crop), mask, nullptr);
    if (filled.isNull())
        return false;
    filled = filled.convertToFormat(QImage::Format_RGB32);
    layFill(image, before, area, hole & crop, [&](int x, int y) { return filled.pixelColor(x - crop.x(), y - crop.y()); });
    // Any of the hole outside the crop (a very long hole) gets the smooth fill.
    return (hole & crop) == hole;
}

void fillHole(QImage &image, const QImage &before, const QRect &area, bool ai) {
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
    if (ai && fillHoleWithAi(image, before, area, hole))
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

    // Lay the fill into the hole, read smoothly from the finest level.
    const Level &fill = levels[0];
    layFill(image, before, area, hole, [&](int x, int y) {
        const float wy = std::clamp((y - crop.y() + .5f) * float(s) - .5f, 0.f, float(fill.h - 1));
        const float wx = std::clamp((x - crop.x() + .5f) * float(s) - .5f, 0.f, float(fill.w - 1));
        const int y0 = int(wy), y1 = std::min(y0 + 1, fill.h - 1), x0 = int(wx), x1 = std::min(x0 + 1, fill.w - 1);
        const float fy = wy - y0, fx = wx - x0;
        int rgb[3];
        for (int k = 0; k < 3; ++k) {
            const float top = fill.c[3 * (y0 * fill.w + x0) + k] * (1 - fx) + fill.c[3 * (y0 * fill.w + x1) + k] * fx;
            const float bottom = fill.c[3 * (y1 * fill.w + x0) + k] * (1 - fx) + fill.c[3 * (y1 * fill.w + x1) + k] * fx;
            rgb[k] = int(std::clamp((top * (1 - fy) + bottom * fy) * 255.f, 0.f, 255.f) + .5f);
        }
        return QColor(rgb[0], rgb[1], rgb[2]);
    });
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
            fillHole(remaining, before, area, cut.value("fillWith").toString() == "ai");

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
           bool punch, QString *error, const QString &wantedName, double feather, bool fill, bool aiFill) {
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
                                   {"fillWith", punch && fill && aiFill ? "ai" : "smooth"},
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
    if (fill >= 0) {
        cut["fill"] = fill > 0 && cut.value("punch").toBool(true);
        cut["fillWith"] = fill == 2 ? "ai" : "smooth";
    }
    Scene edited = scene;
    edited.layers[index].raw["cut"] = cut;
    if (!rebuildCuts(edited, assets, from, error))
        return false;
    scene = edited;
    return true;
}

QImage cutMask(const Scene &scene, AssetCache &assets, int index, double t, QTransform *toCanvas) {
    if (index < 0 || index >= scene.layers.size())
        return {};
    const QJsonObject cut = scene.layers.at(index).raw.value("cut").toObject();
    const int from = scene.indexOf(cut.value("from").toString());
    if (from < 0)
        return {};
    const QSize size = assets.size(scene.layers.at(from).source).toSize();
    if (size.isEmpty())
        return {};
    if (toCanvas)
        *toCanvas = layerTransform(scene, assets, from, t);
    QImage mask(size, QImage::Format_Alpha8);
    mask.fill(0);
    const QString file = cut.value("mask").toString();
    if (!file.isEmpty()) {
        const QImage stored = QImage(QDir(assets.directory()).filePath(file)).convertToFormat(QImage::Format_Grayscale8);
        const QJsonArray at = cut.value("maskOrigin").toArray();
        const QPoint origin(at.at(0).toInt(), at.at(1).toInt());
        for (int y = 0; y < stored.height(); ++y) {
            const int my = origin.y() + y;
            if (my < 0 || my >= size.height())
                continue;
            const uchar *line = stored.constScanLine(y);
            uchar *to = mask.scanLine(my);
            for (int x = 0; x < stored.width(); ++x) {
                const int mx = origin.x() + x;
                if (mx >= 0 && mx < size.width())
                    to[mx] = line[x];
            }
        }
    } else {
        QPainter painter(&mask);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::black);
        painter.drawPolygon(polygonFrom(cut.value("polygon")));
    }
    return mask;
}

bool reshapeCutMask(Scene &scene, AssetCache &assets, int index, const QImage &shape, QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (index < 0 || index >= scene.layers.size())
        return fail("Select the piece to reshape.");
    QJsonObject cut = scene.layers.at(index).raw.value("cut").toObject();
    const int from = scene.indexOf(cut.value("from").toString());
    if (from < 0)
        return fail(QString("%1 was cut from \"%2\", which is gone, so it can't be reshaped.")
                        .arg(scene.layers.at(index).name, cut.value("from").toString()));
    const QImage mask = shape.convertToFormat(QImage::Format_Alpha8);
    QRect bounds;
    for (int y = 0; y < mask.height(); ++y) {
        const uchar *line = mask.constScanLine(y);
        int first = -1, last = -1;
        for (int x = 0; x < mask.width(); ++x)
            if (line[x] > 0) {
                if (first < 0)
                    first = x;
                last = x;
            }
        if (first >= 0)
            bounds |= QRect(first, y, last - first + 1, 1);
    }
    if (bounds.isEmpty())
        return fail("The shape is empty; paint some of it back, or delete the piece.");
    QImage stored(bounds.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < bounds.height(); ++y)
        std::copy_n(mask.constScanLine(bounds.y() + y) + bounds.x(), bounds.width(), stored.scanLine(y));
    const QDir dir(assets.directory());
    dir.mkpath("assets");
    const QString file = "assets/" + uniqueFile(QDir(dir.filePath("assets")), slug(scene.layers.at(index).name) + "-shape", "png");
    if (!writeImage(stored, dir.filePath(file), error))
        return false;
    cut.remove("polygon");
    cut["mask"] = file;
    cut["maskOrigin"] = QJsonArray{bounds.x(), bounds.y()};
    Scene edited = scene;
    edited.layers[index].raw["cut"] = cut;
    if (!rebuildCuts(edited, assets, from, error))
        return false;
    scene = edited;
    return true;
}

bool isAudio(const QString &path) {
    static const QStringList suffixes{"mp3", "wav", "ogg", "opus", "flac", "m4a", "aac"};
    return suffixes.contains(QFileInfo(path).suffix().toLower());
}

bool setAudio(Scene &scene, const QString &directory, const QString &path, QString *error) {
    if (!isAudio(path) || !QFileInfo::exists(path)) {
        if (error)
            *error = QString("%1 isn't a sound file omotion knows (mp3, wav, ogg, opus, flac, m4a).").arg(path);
        return false;
    }
    const QDir dir(directory);
    dir.mkpath("assets");
    const QFileInfo info(path);
    const QString source = "assets/" + uniqueFile(QDir(dir.filePath("assets")), info.completeBaseName(), info.suffix());
    if (!QFile::copy(path, dir.filePath(source))) {
        if (error)
            *error = QString("Couldn't copy %1 into the project.").arg(path);
        return false;
    }
    scene.raw["audio"] = QJsonObject{{"source", source}, {"offset", 0}, {"volume", 1}};
    return true;
}

QList<double> audioPeaks(const QString &file, double offset, double duration, int count) {
    QList<double> peaks;
    const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
    if (ffmpeg.isEmpty() || count <= 0 || duration <= 0)
        return peaks;
    // Mono, a few thousand samples a second: plenty to see the beat.
    constexpr int Rate = 4000;
    QProcess process;
    process.start(ffmpeg, {"-hide_banner", "-loglevel", "error", "-ss", QString::number(std::max(0.0, offset)), "-t",
                           QString::number(duration), "-i", file, "-ac", "1", "-ar", QString::number(Rate), "-f", "s16le", "-"});
    if (!process.waitForFinished(60000))
        return peaks;
    const QByteArray data = process.readAllStandardOutput();
    const qint16 *samples = reinterpret_cast<const qint16 *>(data.constData());
    const qint64 total = data.size() / 2;
    const double perPeak = duration * Rate / count;
    for (int i = 0; i < count; ++i) {
        const qint64 from = qint64(i * perPeak), to = std::min(total, qint64((i + 1) * perPeak));
        int most = 0;
        for (qint64 k = from; k < to; ++k)
            most = std::max(most, std::abs(int(samples[k])));
        peaks.append(most / 32768.0);
    }
    return peaks;
}

bool isCut(const Scene &scene, int index) {
    return index >= 0 && index < scene.layers.size() &&
           scene.layers.at(index).raw.value("cut").toObject().contains("polygon");
}

QPolygonF cutOutline(const Scene &scene, AssetCache &assets, int index, double t, double *feather, int *fill) {
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
        *fill = !cut.value("fill").toBool(false) ? 0 : cut.value("fillWith").toString() == "ai" ? 2 : 1;
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
    // Pieces already cut from it stay pieces, drawn out before the depth
    // layers are, and each goes with the depth layer it stands in.
    QStringList existing;
    int firstOrder = 0;
    for (const int i : piecesOf(scene, source.name)) {
        const QJsonObject cut = scene.layers.at(i).raw.value("cut").toObject();
        if (cut.contains("depth"))
            return fail(QString("%1 is split by depth already.").arg(source.name));
        existing.append(scene.layers.at(i).name);
        firstOrder = std::max(firstOrder, cut.value("order").toInt() + 1);
    }
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
    QStringList bandNames;  // The new layers, nearest first, with the depth each starts at.
    QList<double> thresholds;
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
                                       {"order", firstOrder + order},
                                       {"depth", std::round(centres[band] / 1023 * 1000) / 1000}};
        edited.layers.insert(index + 1, layer);
        // Each new layer goes in just above the source, under the nearer ones.
        created.append(index + 1 + int(created.size()));
        bandNames.append(name);
        thresholds.append(threshold);
    }
    if (created.isEmpty())
        return fail("The picture is all one depth; there's nothing to split.");
    edited.layers[index].raw["depth"] = std::round(centres[0] / 1023 * 1000) / 1000;
    if (!rebuildCuts(edited, assets, index, error))
        return {};

    // Each piece cut before goes with the depth layer it stands in: the depth
    // just under its foot, where it meets the ground (the depth map was made
    // with the piece already lifted out, so there it sees what's behind).
    for (const QString &name : existing) {
        const int i = edited.indexOf(name);
        if (i < 0 || edited.layers.at(i).parent != source.name)
            continue;
        const QJsonArray origin = edited.layers.at(i).raw.value("cut").toObject().value("origin").toArray();
        const QSizeF size = assets.size(edited.layers.at(i).source);
        const QPointF foot(origin.at(0).toDouble() + size.width() / 2, origin.at(1).toDouble() + size.height() + 2);
        const int dx = std::clamp(int(foot.x() * w / full.width()), 0, w - 1);
        const int dy = std::clamp(int(foot.y() * h / full.height()), 0, h - 1);
        const double at = reinterpret_cast<const quint16 *>(depth.constScanLine(dy))[dx];
        int band = -1;
        for (int k = 0; k < thresholds.size() && band < 0; ++k)
            if (at >= thresholds.at(k))
                band = k;
        if (band < 0)
            continue;  // As far as the background: it stays with the source.
        const QString holderName = bandNames.at(band);
        QPointF shift;
        {
            // A depth layer sits on its source with no turn or scale, so going
            // from one's pixels to the other's is a shift.
            const Layer &holder = edited.layers.at(edited.indexOf(holderName));
            const QSizeF bandSize = assets.size(holder.source);
            shift = QPointF(holder.base.value("x") - holder.anchor.x() * bandSize.width(),
                            holder.base.value("y") - holder.anchor.y() * bandSize.height());
        }
        // The piece and everything that hangs off it move together, in order.
        QSet<QString> family{name};
        for (bool grew = true; grew;) {
            grew = false;
            for (const Layer &layer : edited.layers)
                if (!family.contains(layer.name) && family.contains(layer.parent)) {
                    family.insert(layer.name);
                    grew = true;
                }
        }
        QList<Layer> moving;
        for (int k = 0; k < edited.layers.size();)
            if (family.contains(edited.layers.at(k).name))
                moving.append(edited.layers.takeAt(k));
            else
                ++k;
        for (Layer &piece : moving) {
            if (piece.name != name)
                continue;
            piece.parent = holderName;
            piece.base["x"] -= shift.x();
            piece.base["y"] -= shift.y();
            for (const QString &property : {QString("x"), QString("y")})
                for (Key &key : piece.keys[property])
                    key.v -= property == "x" ? shift.x() : shift.y();
            if (piece.keys.value("x").isEmpty())
                piece.keys.remove("x");
            if (piece.keys.value("y").isEmpty())
                piece.keys.remove("y");
        }
        int place = edited.indexOf(holderName) + 1;
        for (const Layer &layer : moving)
            edited.layers.insert(place++, layer);
    }
    // The layers moved about; say where the new ones ended up.
    created.clear();
    for (const QString &name : bandNames)
        created.prepend(edited.indexOf(name));
    scene = edited;
    if (error)
        error->clear();
    return created;
}

bool animateParallax(Scene &scene, AssetCache &assets, int index, double strength, QString *error) {
    Q_UNUSED(assets);
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
    // A camera move this slow wants room; a short scene gets longer, and
    // what's animated already stretches with it, so its loops still loop.
    if (scene.duration < 10) {
        const double stretch = 10 / scene.duration;
        for (Layer &layer : scene.layers)
            for (auto &keys : layer.keys)
                for (Key &key : keys)
                    key.t *= stretch;
        for (auto &keys : scene.camera.keys)
            for (Key &key : keys)
                key.t *= stretch;
        scene.duration = 10;
    }
    const double d = scene.duration;
    auto loop = [d](double a, double b) {
        return QList<Key>{Key{0, a, "inOut"}, Key{d / 2, b, "inOut"}, Key{d, a, "inOut"}};
    };

    // Depth becomes how much each layer follows the camera: the background
    // a little, the nearest fully, so one camera move makes the parallax.
    constexpr double FarFollow = .3;
    scene.layers[index].follow = FarFollow;
    for (const int i : pieces) {
        const double depth = scene.layers.at(i).raw.value("cut").toObject().value("depth").toDouble();
        scene.layers[i].follow = FarFollow + (1 - FarFollow) * std::clamp((depth - far) / std::max(1e-6, 1 - far), 0.0, 1.0);
    }

    // The camera drifts sideways and pushes in a little, zoomed in enough
    // from the start that no layer's edge comes into view.
    const double centre = scene.width / 2.0;
    const double reach = std::min(std::abs(strength), centre * .5);
    const double zoom = 1 / (1 - reach / centre) + .01;
    Layer &camera = scene.camera;
    const double x = scene.value(camera, "x", 0);
    camera.keys["x"] = loop(x, x + reach);
    camera.keys["zoom"] = loop(zoom, zoom * 1.035);
    return true;
}

bool particleSetting(const QString &name, const QVariant &value, QJsonValue *json, QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    static const QStringList numbers{"count", "size", "speed", "direction", "spread", "sway", "twinkle", "seed",
                                     "rate", "life", "inherit", "gravity", "wind", "drag"};
    // An emitter: the layer they come from, and the point on it (its pixels).
    if (name == "from") {
        *json = value.toString();
        return true;
    }
    if (name == "at") {
        const QStringList xy = value.toString().split(',');
        if (value.typeId() == QMetaType::QVariantList && value.toList().size() == 2) {
            *json = QJsonArray{value.toList().at(0).toDouble(), value.toList().at(1).toDouble()};
            return true;
        }
        if (xy.size() != 2)
            return fail("at is a point on the layer they come from: x,y in its pixels.");
        *json = QJsonArray{xy.at(0).toDouble(), xy.at(1).toDouble()};
        return true;
    }
    if (name == "color") {
        if (!QColor(value.toString()).isValid() || value.typeId() != QMetaType::QString)
            return fail(QString("\"%1\" isn't a colour, like #ffffff.").arg(value.toString()));
        *json = value.toString();
        return true;
    }
    if (!numbers.contains(name))
        return fail(QString("Particles have no setting \"%1\"; they have %2, color, from, at.").arg(name, numbers.join(", ")));
    bool ok = false;
    const double number = value.toDouble(&ok);
    if (!ok || !std::isfinite(number))
        return fail(QString("%1 needs a number, not \"%2\".").arg(name, value.toString()));
    if (name == "count" || name == "seed")
        *json = std::clamp(int(std::lround(number)), 0, name == "count" ? 5000 : 1 << 30);
    else if (name == "twinkle")
        *json = std::clamp(number, 0.0, 1.0);
    else if (name == "rate")
        *json = std::clamp(number, 0.0, 2000.0);
    else if (name == "life")
        *json = std::clamp(number, .05, 30.0);
    else
        *json = number;
    return true;
}

int addParticles(Scene &scene, const QString &kind, const QJsonObject &settings, QString *error) {
    if (!Scene::particleKinds().contains(kind)) {
        if (error)
            *error = QString("There's no particle kind \"%1\"; there's %2.").arg(kind, Scene::particleKinds().join(", "));
        return -1;
    }
    Layer layer;
    layer.name = scene.uniqueName(kind);
    for (const QString &property : Scene::properties())
        layer.base.insert(property, Scene::defaultValue(property));
    layer.particles = Scene::particleDefaults(kind);
    for (auto it = settings.begin(); it != settings.end(); ++it) {
        QJsonValue value;
        if (!particleSetting(it.key(), it.value().toVariant(), &value, error))
            return -1;
        layer.particles.insert(it.key(), value);
    }
    // Light things glow on what's under them.
    if (kind == "fireflies" || kind == "embers")
        layer.blend = "add";
    scene.layers.append(layer);
    return int(scene.layers.size()) - 1;
}

namespace {
QString motionDirectoryOverride;
}

void setMotionDirectory(const QString &directory) { motionDirectoryOverride = directory; }

QString motionDirectory() {
    return motionDirectoryOverride.isEmpty()
               ? QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/omotion/motions"
               : motionDirectoryOverride;
}

QStringList savedMotions() {
    QStringList names;
    for (const QString &file : QDir(motionDirectory()).entryList({"*.json"}, QDir::Files, QDir::Name))
        names.append(QFileInfo(file).completeBaseName());
    return names;
}

bool saveMotion(const QString &wanted, const QList<KeyClip> &keys, QString *error) {
    const QString name = wanted.trimmed();
    if (name.isEmpty() || name.contains('/') || name.startsWith('.')) {
        if (error)
            *error = "Give the motion a name (no slashes).";
        return false;
    }
    if (keys.isEmpty()) {
        if (error)
            *error = "There are no keyframes to save.";
        return false;
    }
    QJsonObject properties;
    for (const KeyClip &item : keys) {
        // Pins belong to one layer's picture; a motion is for any layer.
        if (!Scene::properties().contains(item.property) && !Scene::cameraProperties().contains(item.property))
            continue;
        QJsonArray list = properties.value(item.property).toArray();
        list.append(QJsonObject{{"t", item.key.t}, {"v", item.key.v}, {"ease", item.key.ease}});
        properties.insert(item.property, list);
    }
    if (properties.isEmpty()) {
        if (error)
            *error = "Only pin keyframes there, and pins belong to one layer; there's nothing to save as a motion.";
        return false;
    }
    QDir().mkpath(motionDirectory());
    QSaveFile file(QDir(motionDirectory()).filePath(name + ".json"));
    const QByteArray data = QJsonDocument(QJsonObject{{"name", name}, {"keys", properties}}).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        if (error)
            *error = QString("Couldn't save the motion: %1").arg(file.errorString());
        return false;
    }
    return true;
}

QList<KeyClip> loadMotion(const QString &name, QString *error) {
    QFile file(QDir(motionDirectory()).filePath(name + ".json"));
    QList<KeyClip> keys;
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QString("There's no saved motion \"%1\".").arg(name);
        return keys;
    }
    const QJsonObject properties = QJsonDocument::fromJson(file.readAll()).object().value("keys").toObject();
    for (auto it = properties.begin(); it != properties.end(); ++it) {
        if (!Scene::properties().contains(it.key()) && !Scene::cameraProperties().contains(it.key()))
            continue;
        for (const QJsonValue &value : it.value().toArray()) {
            const QJsonObject k = value.toObject();
            const QString ease = k.value("ease").toString("linear");
            keys.append({it.key(), Key{k.value("t").toDouble(), k.value("v").toDouble(), Scene::validEase(ease) ? ease : "linear"}});
        }
    }
    if (keys.isEmpty() && error)
        *error = QString("The motion \"%1\" has no keyframes omotion can use.").arg(name);
    return keys;
}

const QStringList &presets() {
    static const QStringList list{"sway", "float", "pulse", "bounce", "spin", "fade"};
    return list;
}

bool applyPreset(Scene &scene, AssetCache &assets, int index, const QString &preset, double t, double amount,
                 QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (index < 0 || index >= scene.layers.size())
        return fail("Select the layer to move.");
    if (!presets().contains(preset))
        return fail(QString("There's no preset \"%1\"; there's %2.").arg(preset, presets().join(", ")));
    Layer &layer = scene.layers[index];
    const double d = scene.duration;
    // Canvas pixels in the layer's parent's pixels, for moves that should
    // look the same size whatever the parent's scale.
    const double perPixel = 1 / std::max(1e-6, pixelScale(parentTransform(scene, assets, index, t)));

    // A whole number of cycles in the scene, near the period wanted.
    auto cycles = [d](double period) { return std::max(1, int(std::lround(d / period))); };
    // Swinging either side of a value, smoothly, starting and ending on it.
    auto swing = [&](double base, double reach, double period) {
        const int n = cycles(period);
        const double T = d / n;
        QList<Key> keys{Key{0, base, "out"}};
        for (int i = 0; i < 2 * n; ++i)
            keys.append(Key{T / 4 + i * T / 2, base + (i % 2 ? -reach : reach), "inOut"});
        keys.last().ease = "in";
        keys.append(Key{d, base, "linear"});
        return keys;
    };

    // Keys hold a property's own value: a link adds to them, so reading the
    // linked total here would count the link twice.
    if (preset == "sway") {
        layer.keys["rotation"] = swing(scene.ownValue(layer, "rotation", t), 4 * amount, 3);
    } else if (preset == "float") {
        layer.keys["y"] = swing(scene.ownValue(layer, "y", t), 10 * amount * perPixel, 4);
    } else if (preset == "pulse") {
        const double scale = scene.ownValue(layer, "scale", t);
        layer.keys["scale"] = swing(scale, scale * .04 * amount, 2);
    } else if (preset == "bounce") {
        // Up quickly and slowing, down slowly and speeding up, like a ball.
        const double y = scene.ownValue(layer, "y", t), height = 30 * amount * perPixel;
        const int n = cycles(1.2);
        const double T = d / n;
        QList<Key> keys;
        for (int i = 0; i < n; ++i) {
            keys.append(Key{i * T, y, "out"});
            keys.append(Key{i * T + T / 2, y - height, "in"});
        }
        keys.append(Key{d, y, "linear"});
        layer.keys["y"] = keys;
    } else if (preset == "spin") {
        const double rotation = scene.ownValue(layer, "rotation", t);
        // Whole turns, so the end looks like the start and it loops.
        const int turns = std::max(1, int(std::lround(std::abs(amount))));
        layer.keys["rotation"] = {Key{0, rotation, "linear"}, Key{d, rotation + 360.0 * turns * (amount < 0 ? -1 : 1), "linear"}};
    } else {
        // Up to however solid it is now, over a second from t.
        const double start = std::min(t, std::max(0.0, d - 1));
        const double opacity = scene.ownValue(layer, "opacity", t) > 0 ? scene.ownValue(layer, "opacity", t) : 1;
        layer.keys["opacity"] = {Key{start, 0, "out"}, Key{std::min(d, start + 1), opacity, "linear"}};
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

QStringList describeChanges(const Scene &before, const Scene &after) {
    QStringList lines;
    const QJsonObject a = before.toJson(), b = after.toJson();
    auto byName = [](const QJsonObject &scene) {
        QMap<QString, QJsonObject> layers;
        for (const QJsonValue &value : scene.value("layers").toArray())
            layers.insert(value.toObject().value("name").toString(), value.toObject());
        return layers;
    };
    // Which fields of an object differ: animated properties as "keys on x",
    // the rest by name.
    auto fields = [](const QJsonObject &from, const QJsonObject &to) {
        QStringList changed, keyed;
        QSet<QString> names;
        for (const QString &key : from.keys())
            names.insert(key);
        for (const QString &key : to.keys())
            names.insert(key);
        QStringList sorted(names.begin(), names.end());
        std::sort(sorted.begin(), sorted.end());
        for (const QString &name : sorted) {
            if (name == "name" || from.value(name) == to.value(name))
                continue;
            if (name == "keys") {
                const QJsonObject k1 = from.value(name).toObject(), k2 = to.value(name).toObject();
                QSet<QString> props;
                for (const QString &key : k1.keys())
                    props.insert(key);
                for (const QString &key : k2.keys())
                    props.insert(key);
                QStringList list(props.begin(), props.end());
                std::sort(list.begin(), list.end());
                for (const QString &prop : list)
                    if (k1.value(prop) != k2.value(prop))
                        keyed << prop;
            } else {
                changed << name;
            }
        }
        QStringList words;
        if (!changed.isEmpty())
            words << changed.join(", ");
        if (!keyed.isEmpty())
            words << "keys on " + keyed.join(", ");
        return words.join("; ");
    };
    const auto was = byName(a), now = byName(b);
    for (auto it = now.begin(); it != now.end(); ++it)
        if (!was.contains(it.key()))
            lines << "added " + it.key();
    for (auto it = was.begin(); it != was.end(); ++it)
        if (!now.contains(it.key()))
            lines << "took away " + it.key();
    for (auto it = now.begin(); it != now.end(); ++it)
        if (was.contains(it.key())) {
            const QString what = fields(was.value(it.key()), it.value());
            if (!what.isEmpty())
                lines << it.key() + ": " + what;
        }
    // The camera is left out of the file while it's untouched, so it's
    // compared property by property.
    QStringList cameraChanged, cameraKeyed;
    for (const QString &property : Scene::cameraProperties()) {
        const double was = before.camera.base.value(property, Scene::defaultValue(property));
        const double is = after.camera.base.value(property, Scene::defaultValue(property));
        // x and y default to the canvas's middle.
        const bool middle = property == "x" || property == "y";
        if (!middle && was != is)
            cameraChanged << property;
        if (middle && before.camera.base.value(property, property == "x" ? before.width / 2.0 : before.height / 2.0) !=
                          after.camera.base.value(property, property == "x" ? after.width / 2.0 : after.height / 2.0))
            cameraChanged << property;
        const auto keysOf = [&](const Scene &scene) {
            QList<QPair<double, double>> list;
            for (const Key &key : scene.camera.keys.value(property))
                list << qMakePair(key.t, key.v);
            return list;
        };
        if (keysOf(before) != keysOf(after))
            cameraKeyed << property;
    }
    QStringList camera;
    if (!cameraChanged.isEmpty())
        camera << cameraChanged.join(", ");
    if (!cameraKeyed.isEmpty())
        camera << "keys on " + cameraKeyed.join(", ");
    if (!camera.isEmpty())
        lines << "camera: " + camera.join("; ");
    const QString world = fields(a.value("world").toObject(), b.value("world").toObject());
    if (!world.isEmpty())
        lines << "world: " + world;
    QJsonObject sceneA = a, sceneB = b;
    for (const QString &key : {"layers", "camera", "world"}) {
        sceneA.remove(key);
        sceneB.remove(key);
    }
    const QString rest = fields(sceneA, sceneB);
    if (!rest.isEmpty())
        lines << "scene: " + rest;
    // The order of layers, when that's all that changed among them.
    QStringList orderA, orderB;
    for (const QJsonValue &value : a.value("layers").toArray())
        orderA << value.toObject().value("name").toString();
    for (const QJsonValue &value : b.value("layers").toArray())
        orderB << value.toObject().value("name").toString();
    QStringList keptA = orderA, keptB = orderB;
    keptA.removeIf([&](const QString &name) { return !orderB.contains(name); });
    keptB.removeIf([&](const QString &name) { return !orderA.contains(name); });
    if (keptA != keptB)
        lines << "the layers' order";
    return lines;
}

QList<int> rankMatches(const QString &query, const QStringList &names) {
    const QStringList words = query.toLower().split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    QList<QPair<int, int>> scored;
    for (int i = 0; i < names.size(); ++i) {
        const QString name = names.at(i).toLower();
        const QStringList parts = name.split(QRegularExpression("[^\\w]+"), Qt::SkipEmptyParts);
        int score = 0;
        bool all = true;
        for (const QString &word : words) {
            int best = 0;
            for (int p = 0; p < parts.size(); ++p)
                if (parts.at(p).startsWith(word))
                    best = std::max(best, p == 0 ? 30 : 20);
            if (best == 0 && name.contains(word))
                best = 5;
            if (best == 0) {
                all = false;
                break;
            }
            score += best;
        }
        if (all)
            scored << qMakePair(-score, i);  // Highest first; ties keep the list's order.
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto &x, const auto &y) { return x.first < y.first; });
    QList<int> order;
    for (const auto &pair : scored)
        order << pair.second;
    return order;
}

int addEmpty(Scene &scene, const QString &name, const QString &parent, const QPointF &at) {
    Layer empty;
    empty.name = scene.uniqueName(name);
    empty.parent = parent;
    empty.base["x"] = at.x();
    empty.base["y"] = at.y();
    empty.raw["empty"] = true;
    scene.layers.append(empty);
    return int(scene.layers.size()) - 1;
}

namespace {
// The middle of what's solid in a layer's image, and how far its solid part
// reaches from a point; in its pixels.
QRectF solidBox(const QImage &image) {
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

// The end of a bone: of what's solid, the point farthest along a way from
// a point (and of those, the nearest the line), in its pixels.
QPointF farthestSolid(const QImage &image, const QPointF &from, QPointF way) {
    const QImage argb = image.convertToFormat(QImage::Format_ARGB32);
    const double length = std::hypot(way.x(), way.y());
    way = length > 0 ? way / length : QPointF(1, 0);
    // Big pictures are looked at a pixel in so many.
    const int step = std::max(1, int(std::sqrt(double(argb.width()) * argb.height() / 250000)));
    QPointF best = from;
    double score = -1e30;
    for (int y = 0; y < argb.height(); y += step) {
        const QRgb *line = reinterpret_cast<const QRgb *>(argb.constScanLine(y));
        for (int x = 0; x < argb.width(); x += step)
            if (qAlpha(line[x]) > 128) {
                const QPointF d = QPointF(x + .5, y + .5) - from;
                const double along = QPointF::dotProduct(d, way), across = std::abs(d.x() * way.y() - d.y() * way.x());
                const double s = std::round(along) - across * 1e-3;
                if (s > score)
                    score = s, best = from + d;
            }
    }
    return best;
}

QPointF pointOf(const QJsonValue &value) { return QPointF(value.toArray().at(0).toDouble(), value.toArray().at(1).toDouble()); }

QJsonArray pointJson(const QPointF &p) { return QJsonArray{std::round(p.x() * 100) / 100, std::round(p.y() * 100) / 100}; }

void addConstraint(Scene &scene, int layer, const QJsonObject &constraint) {
    QJsonArray list;
    // One of a kind a layer: a new one takes the old one's place.
    for (const QJsonValue &c : scene.layers.at(layer).raw.value("constraints").toArray())
        if (c.toObject().value("type") != constraint.value("type"))
            list.append(c);
    list.append(constraint);
    scene.layers[layer].raw["constraints"] = list;
}

// From the canvas into a layer's parent's pixels (the world, for none).
QPointF intoParent(const Scene &scene, AssetCache &assets, int layer, const QPointF &canvas, double t) {
    const int parent = scene.indexOf(scene.layers.at(layer).parent);
    const QTransform toCanvas = (parent >= 0 ? chainTransform(scene, assets, parent, t) : QTransform()) *
                                cameraTransform(scene, followOf(scene, layer), t);
    return toCanvas.inverted().map(canvas);
}
}

bool rig(Scene &scene, AssetCache &assets, int index, const QString &type, const QString &otherName, QString *error,
         const QJsonObject &options) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (index < 0 || index >= scene.layers.size())
        return fail("There's no such layer.");
    const Layer &layer = scene.layers.at(index);
    const int other = scene.indexOf(otherName);
    const double t = 0;
    if (type == "path") {
        const QJsonArray given = options.value("points").toArray();
        if (given.size() < 2)
            return fail("A path needs two points or more.");
        QJsonArray points;
        for (const QJsonValue &p : given)
            points.append(pointJson(intoParent(scene, assets, index, QPointF(p.toArray().at(0).toDouble(), p.toArray().at(1).toDouble()), t)));
        QJsonObject c{{"type", "path"}, {"points", points}, {"start", options.value("start").toDouble(0)},
                      {"end", options.value("end").toDouble(scene.duration)}, {"turn", options.value("turn").toBool()}};
        if (options.contains("ease"))
            c["ease"] = options.value("ease");
        // Turning with the way keeps the way it faces now, at the start.
        if (c["turn"].toBool()) {
            const QPointF a = pointOf(points.at(0)), b = pointOf(points.at(1));
            c["axis"] = std::round((std::atan2(b.y() - a.y(), b.x() - a.x()) * 180 / M_PI - scene.ownValue(layer, "rotation", t)) * 100) / 100;
        }
        addConstraint(scene, index, c);
        return true;
    }
    if (other < 0 || other == index)
        return fail(QString("Rig %1 to another layer: there's none called \"%2\".").arg(layer.name, otherName));
    const Layer &with = scene.layers.at(other);
    if (type == "lookAt") {
        if (with.parent != layer.parent)
            return fail(QString("%1 and %2 should move with the same layer to look at each other.").arg(layer.name, with.name));
        const QPointF aim = QPointF(scene.value(with, "x", t), scene.value(with, "y", t)) -
                            QPointF(scene.value(layer, "x", t), scene.value(layer, "y", t));
        const double axis = std::atan2(aim.y(), aim.x()) * 180 / M_PI - scene.ownValue(layer, "rotation", t);
        addConstraint(scene, index, {{"type", "lookAt"}, {"target", with.name}, {"axis", std::round(axis * 100) / 100}});
        return true;
    }
    if (type == "ik") {
        if (with.parent != layer.name)
            return fail(QString("%1 should move with %2 (be its child) to be the lower bone.").arg(with.name, layer.name));
        const QSizeF upperSize = assets.size(layer.source), lowerSize = assets.size(with.source);
        const QImage lowerImage = assets.image(with.source);
        if (upperSize.isEmpty() || lowerImage.isNull())
            return fail("Bones need pictures.");
        const QPointF upperPivot(layer.anchor.x() * upperSize.width(), layer.anchor.y() * upperSize.height());
        const QPointF lowerPivot(with.anchor.x() * lowerSize.width(), with.anchor.y() * lowerSize.height());
        // Along the bone: from the upper's pivot through the lower's, as the
        // lower's own pixels see it.
        const QTransform lowerToCanvas = layerTransform(scene, assets, other, t);
        const QPointF upperOnCanvas = layerTransform(scene, assets, index, t).map(upperPivot);
        const QPointF lowerOnCanvas = lowerToCanvas.map(lowerPivot);
        const QTransform back = lowerToCanvas.inverted();
        QPointF way = back.map(lowerOnCanvas + (lowerOnCanvas - upperOnCanvas)) - lowerPivot;
        if (std::hypot(way.x(), way.y()) < 1e-6)
            way = QPointF(1, 0);
        const QPointF tip = farthestSolid(lowerImage, lowerPivot, way);
        // The target starts where the tip is, so nothing moves.
        const QPointF tipCanvas = lowerToCanvas.map(tip);
        const QPointF at = intoParent(scene, assets, index, tipCanvas, t);
        const int target = addEmpty(scene, with.name + " target", scene.layers.at(index).parent, at);
        addConstraint(scene, index, {{"type", "ik"}, {"lower", scene.layers.at(other).name}, {"target", scene.layers.at(target).name},
                                     {"pivot", pointJson(upperPivot)}, {"tip", pointJson(tip - lowerPivot)},
                                     {"bend", options.value("bend").toInt(1)}});
        return true;
    }
    if (type == "wheel") {
        if (layer.parent != with.name && layer.parent != with.parent)
            return fail(QString("%1 should move with %2, or with the same layer, to roll as it goes.").arg(layer.name, with.name));
        const QImage image = assets.image(layer.source);
        const QRectF box = solidBox(image);
        if (box.isEmpty())
            return fail(QString("%1 has nothing solid to roll.").arg(layer.name));
        // It turns about its middle.
        setPivot(scene, assets, index, layerTransform(scene, assets, index, t).map(box.center()), t);
        const Layer &wheel = scene.layers.at(index);
        double radius = std::max(box.width(), box.height()) / 2 * scene.value(wheel, "scale", t) * scene.value(wheel, "scaleX", t);
        if (wheel.parent == with.name)
            radius *= scene.value(with, "scale", t) * scene.value(with, "scaleX", t);
        if (radius <= 0)
            return fail("The wheel has no size.");
        // A turn for every circumference it goes, the way it rolls.
        Link link;
        link.from = with.name + ".x";
        link.times = std::round(180 / (M_PI * radius) * 1e5) / 1e5;
        QString why;
        if (!scene.setLink(index, "rotation", link, &why))
            return fail(why);
        scene.layers[index].raw["wheel"] = with.name;
        return true;
    }
    return fail(QString("There's no rig called %1: ik, lookAt, wheel or path.").arg(type));
}

bool unrig(Scene &scene, int index) {
    if (index < 0 || index >= scene.layers.size())
        return false;
    Layer &layer = scene.layers[index];
    const bool had = layer.raw.contains("constraints") || layer.raw.contains("wheel");
    layer.raw.remove("constraints");
    if (layer.raw.contains("wheel")) {
        layer.raw.remove("wheel");
        layer.links.remove("rotation");
    }
    return had;
}

bool setModifier(Scene &scene, int index, const QString &property, const QString &type, double amount, double speed,
                 QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    Layer *layer = scene.at(index);
    if (!layer)
        return fail("There's no such layer.");
    const QStringList known = index == Scene::CameraIndex ? Scene::cameraProperties() : Scene::propertiesOf(*layer);
    if (!known.contains(property))
        return fail(QString("There's no property called %1.").arg(property));
    if (type != "cycle" && type != "noise" && type != "none")
        return fail("A modifier is cycle, noise or none.");
    QJsonObject modifiers = layer->raw.value("modifiers").toObject();
    QJsonArray list;
    for (const QJsonValue &m : modifiers.value(property).toArray())
        if (type != "none" && m.toObject().value("type").toString() != type)
            list.append(m);
    if (type == "cycle")
        list.append(QJsonObject{{"type", "cycle"}});
    else if (type == "noise")
        list.append(QJsonObject{{"type", "noise"}, {"amount", amount}, {"speed", speed > 0 ? speed : 2}});
    if (list.isEmpty())
        modifiers.remove(property);
    else
        modifiers[property] = list;
    if (modifiers.isEmpty())
        layer->raw.remove("modifiers");
    else
        layer->raw["modifiers"] = modifiers;
    return true;
}

bool addClip(Scene &scene, int index, const QString &motion, double start, int repeat, double speed, double amount,
             QString *error) {
    Layer *layer = scene.at(index);
    if (!layer) {
        if (error)
            *error = "There's no such layer.";
        return false;
    }
    const QList<KeyClip> keys = loadMotion(motion, error);
    if (keys.isEmpty())
        return false;
    QJsonObject properties;
    for (const KeyClip &item : keys) {
        QJsonArray list = properties.value(item.property).toArray();
        list.append(QJsonObject{{"t", item.key.t}, {"v", item.key.v}, {"ease", item.key.ease}});
        properties[item.property] = list;
    }
    QJsonObject motions = scene.raw.value("motions").toObject();
    motions[motion] = QJsonObject{{"keys", properties}};
    scene.raw["motions"] = motions;
    QJsonArray clips = layer->raw.value("clips").toArray();
    QJsonObject clip{{"motion", motion}, {"start", std::round(start * 1000) / 1000}};
    if (repeat > 1)
        clip["repeat"] = repeat;
    if (speed != 1)
        clip["speed"] = speed;
    if (amount != 1)
        clip["amount"] = amount;
    clips.append(clip);
    layer->raw["clips"] = clips;
    return true;
}

bool removeClip(Scene &scene, int index, int clip) {
    Layer *layer = scene.at(index);
    if (!layer)
        return false;
    QJsonArray clips = layer->raw.value("clips").toArray();
    if (clip < 0 || clip >= clips.size())
        return false;
    clips.removeAt(clip);
    if (clips.isEmpty())
        layer->raw.remove("clips");
    else
        layer->raw["clips"] = clips;
    return true;
}

const QStringList &lookSettings() {
    static const QStringList list{"exposure", "contrast", "saturation", "temperature", "tint", "vignette", "bloom",
                                  "bloomThreshold", "focus", "aperture"};
    return list;
}

bool setLook(Scene &scene, const QString &name, double value, QString *error) {
    if (!lookSettings().contains(name) || !std::isfinite(value)) {
        if (error)
            *error = QString("The look has %1.").arg(lookSettings().join(", "));
        return false;
    }
    static const QHash<QString, QPair<double, double>> ranges{
        {"exposure", {-5, 5}}, {"contrast", {0, 3}}, {"saturation", {0, 3}}, {"temperature", {-1, 1}}, {"tint", {-1, 1}},
        {"vignette", {0, 1}},  {"bloom", {0, 1}},    {"bloomThreshold", {0, .99}}, {"focus", {-1e5, 1e5}}, {"aperture", {0, 100}}};
    const auto [low, high] = ranges.value(name);
    QJsonObject look = scene.raw.value("look").toObject();
    const double neutral = name == "contrast" || name == "saturation" ? 1 : name == "bloomThreshold" ? .75 : 0;
    const double v = std::clamp(value, low, high);
    if (v == neutral && name != "focus")
        look.remove(name);
    else
        look[name] = v;
    // Focus alone does nothing: it goes with an aperture.
    if (look.size() == 1 && look.contains("focus"))
        look.remove("focus");
    if (look.isEmpty())
        scene.raw.remove("look");
    else
        scene.raw["look"] = look;
    return true;
}

int addDrawing(Scene &scene, const QString &name) {
    Layer layer;
    layer.name = scene.uniqueName(name.isEmpty() ? QString("drawing") : name);
    layer.anchor = QPointF(0, 0);
    // A pixel of the drawing is a pixel of the world it's drawn in.
    layer.base["x"] = 0;
    layer.base["y"] = 0;
    layer.raw["drawing"] = QJsonObject{{"frames", QJsonArray{}}};
    scene.layers.append(layer);
    return int(scene.layers.size()) - 1;
}

bool addStroke(Scene &scene, AssetCache &assets, int index, double t, const QList<QPointF> &canvas, const QColor &colour,
               double width, QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (index < 0 || index >= scene.layers.size() || !scene.layers.at(index).raw.contains("drawing"))
        return fail("Draw on a drawing layer.");
    if (canvas.isEmpty())
        return fail("There's no stroke to add.");
    bool invertible = false;
    const QTransform back = layerTransform(scene, assets, index, t).inverted(&invertible);
    if (!invertible)
        return fail("The drawing layer is squashed flat; it can't be drawn on.");
    QJsonArray points;
    for (const QPointF &p : canvas) {
        const QPointF local = back.map(p);
        points.append(QJsonArray{std::round(local.x() * 10) / 10, std::round(local.y() * 10) / 10});
    }
    // Widths are on the canvas; the layer's own scale is taken out.
    const double scale = std::sqrt(std::abs(back.determinant()));
    const QJsonObject stroke{{"color", colour.name(QColor::HexArgb)}, {"width", std::round(width * scale * 100) / 100}, {"points", points}};
    QJsonObject drawing = scene.layers.at(index).raw.value("drawing").toObject();
    QJsonArray frames = drawing.value("frames").toArray();
    const double half = .5 / std::max(1.0, scene.fps);
    int frame = -1;
    for (int i = 0; i < frames.size(); ++i)
        if (std::abs(frames.at(i).toObject().value("t").toDouble() - t) < half)
            frame = i;
    if (frame < 0) {
        frames.append(QJsonObject{{"t", std::round(t * 1000) / 1000}, {"strokes", QJsonArray{}}});
        frame = int(frames.size()) - 1;
    }
    QJsonObject f = frames.at(frame).toObject();
    QJsonArray strokes = f.value("strokes").toArray();
    strokes.append(stroke);
    f["strokes"] = strokes;
    frames[frame] = f;
    drawing["frames"] = frames;
    scene.layers[index].raw["drawing"] = drawing;
    return true;
}

bool removeStroke(Scene &scene, int index, double t) {
    if (index < 0 || index >= scene.layers.size())
        return false;
    const int frame = drawingFrameAt(scene.layers.at(index), t);
    if (frame < 0)
        return false;
    QJsonObject drawing = scene.layers.at(index).raw.value("drawing").toObject();
    QJsonArray frames = drawing.value("frames").toArray();
    QJsonObject f = frames.at(frame).toObject();
    QJsonArray strokes = f.value("strokes").toArray();
    if (strokes.isEmpty())
        return false;
    strokes.removeLast();
    f["strokes"] = strokes;
    frames[frame] = f;
    drawing["frames"] = frames;
    scene.layers[index].raw["drawing"] = drawing;
    return true;
}

int addLight(Scene &scene, const QString &name, const QVector3D &at, const QJsonObject &settings) {
    Layer light;
    light.name = scene.uniqueName(name.isEmpty() ? QString("light") : name);
    light.base["worldX"] = at.x();
    light.base["worldY"] = at.y();
    light.base["worldZ"] = at.z();
    QJsonObject values{{"color", "#fff2dd"}, {"power", 1}, {"reach", 4}};
    for (auto it = settings.begin(); it != settings.end(); ++it)
        values[it.key()] = it.value();
    light.raw["light"] = values;
    scene.layers.append(light);
    return int(scene.layers.size()) - 1;
}

bool setLight(Scene &scene, int index, const QString &name, const QVariant &value, QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (index < 0 || index >= scene.layers.size() || !scene.layers.at(index).raw.contains("light"))
        return fail("Select a light.");
    QJsonObject light = scene.layers.at(index).raw.value("light").toObject();
    if (name == "color") {
        const QColor colour(value.toString().trimmed());
        if (!colour.isValid())
            return fail(QString("%1 isn't a colour; try #ffcc88.").arg(value.toString()));
        light["color"] = colour.name();
    } else if (name == "power" || name == "reach") {
        bool ok = false;
        const double number = value.toDouble(&ok);
        if (!ok || !std::isfinite(number) || number < 0)
            return fail(QString("A light's %1 is a number of 0 or more.").arg(name));
        light[name] = name == "reach" ? std::max(.1, number) : std::min(number, 100.0);
    } else {
        return fail("A light has a color, a power and a reach.");
    }
    scene.layers[index].raw["light"] = light;
    return true;
}

bool setMaterial(Scene &scene, int index, const QString &name, double value, QString *error) {
    if (index < 0 || index >= scene.layers.size() || (name != "shine" && name != "roughness") || !std::isfinite(value)) {
        if (error)
            *error = "A material has a shine and a roughness, from 0 to 1.";
        return false;
    }
    QJsonObject material = scene.layers.at(index).raw.value("material").toObject();
    material[name] = std::clamp(value, 0.0, 1.0);
    scene.layers[index].raw["material"] = material;
    return true;
}

bool isVideo(const QString &path) {
    static const QStringList kinds{"mp4", "webm", "mov", "mkv", "m4v", "avi"};
    return kinds.contains(QFileInfo(path).suffix().toLower());
}

int importVideo(Scene &scene, const QString &directory, const QString &path, QString *error, int max) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return -1;
    };
    const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
    if (ffmpeg.isEmpty())
        return fail("Video needs ffmpeg to read it. Install it with: omarchy-pkg-add ffmpeg");
    if (!QFileInfo::exists(path))
        return fail(QString("There's no %1.").arg(path));
    const QDir dir(directory);
    const QFileInfo info(path);
    // A folder of its own for the frames, never one already there.
    QString frames = "assets/video-" + info.completeBaseName();
    for (int n = 2; dir.exists(frames); ++n)
        frames = QString("assets/video-%1-%2").arg(info.completeBaseName()).arg(n);
    if (!dir.mkpath(frames))
        return fail(QString("Couldn't make %1.").arg(frames));
    QProcess process;
    process.start(ffmpeg, {"-hide_banner", "-loglevel", "error", "-i", path, "-vf", QString("fps=%1").arg(scene.fps),
                           "-frames:v", QString::number(max), "-q:v", "3", dir.filePath(frames + "/%05d.jpg")});
    if (!process.waitForFinished(10 * 60 * 1000) || process.exitCode() != 0)
        return fail("ffmpeg couldn't read the video: " + QString::fromUtf8(process.readAllStandardError()).trimmed().left(300));
    const int count = int(QDir(dir.filePath(frames)).entryList({"*.jpg"}, QDir::Files).size());
    if (count == 0)
        return fail("The video has no frames ffmpeg could read.");
    const QString first = frames + "/00001.jpg";
    const QSize size = QImageReader(dir.filePath(first)).size();
    const double fit = std::min(1.0, std::min(double(scene.width) / size.width(), double(scene.height) / size.height()));
    Layer layer = centeredLayer(scene, info.completeBaseName(), first, fit);
    layer.raw["video"] = QJsonObject{{"frames", frames}, {"count", count}, {"fps", scene.fps}, {"start", 0}, {"loop", true}};
    scene.layers.append(layer);
    return int(scene.layers.size()) - 1;
}

bool setVideo(Scene &scene, int index, const QString &name, const QVariant &value, QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (index < 0 || index >= scene.layers.size() || !scene.layers.at(index).raw.contains("video"))
        return fail("Select a video layer.");
    QJsonObject video = scene.layers.at(index).raw.value("video").toObject();
    if (name == "loop") {
        video["loop"] = value.toBool();
    } else if (name == "start" || name == "speed") {
        bool ok = false;
        const double number = value.toDouble(&ok);
        if (!ok || !std::isfinite(number))
            return fail(QString("A video's %1 is a number.").arg(name));
        video[name] = name == "speed" ? std::clamp(number, -16.0, 16.0) : number;
    } else {
        return fail("A video has a start, a speed and loop.");
    }
    scene.layers[index].raw["video"] = video;
    return true;
}

namespace {
// A frame as grey values, for matching patches.
std::vector<float> greyOf(const QImage &image) {
    const QImage grey = image.convertToFormat(QImage::Format_Grayscale8);
    std::vector<float> values(size_t(grey.width()) * grey.height());
    for (int y = 0; y < grey.height(); ++y) {
        const uchar *line = grey.constScanLine(y);
        for (int x = 0; x < grey.width(); ++x)
            values[size_t(y) * grey.width() + x] = line[x];
    }
    return values;
}
}

int track(Scene &scene, AssetCache &assets, int video, const QPointF &point, double from, double to, QString *error,
          const QString &onto, QList<QPointF> *path) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return -1;
    };
    if (video < 0 || video >= scene.layers.size() || !scene.layers.at(video).raw.contains("video"))
        return fail("Track a point on a video layer.");
    const Layer &clip = scene.layers.at(video);
    const double step = 1 / std::max(1.0, scene.fps);
    from = std::clamp(from, 0.0, scene.duration);
    to = std::clamp(to, from, scene.duration);
    const int frames = int(std::lround((to - from) / step)) + 1;
    bool invertible = false;
    QTransform back = layerTransform(scene, assets, video, from).inverted(&invertible);
    if (!invertible)
        return fail("The video layer is squashed flat.");
    // In the clip's own pixels: a patch about the point, looked for nearby
    // in each next frame, learning a little of how it looks as it goes.
    QPointF at = back.map(point);
    const QImage first = assets.image(sourceAt(clip, from));
    if (first.isNull())
        return fail("Couldn't read the video's frame.");
    const int w = first.width(), h = first.height();
    const int half = std::clamp(std::min(w, h) / 40, 6, 16), search = half * 2;
    if (at.x() < half || at.y() < half || at.x() >= w - half || at.y() >= h - half)
        return fail("Pick a point inside the video, away from its edges.");
    std::vector<float> grey = greyOf(first);
    const int side = 2 * half + 1;
    std::vector<float> pattern(size_t(side) * side);
    auto cut = [&](const std::vector<float> &from, int cx, int cy, std::vector<float> &into) {
        for (int y = -half; y <= half; ++y)
            for (int x = -half; x <= half; ++x)
                into[size_t(y + half) * side + x + half] = from[size_t(std::clamp(cy + y, 0, h - 1)) * w + std::clamp(cx + x, 0, w - 1)];
    };
    cut(grey, int(std::lround(at.x())), int(std::lround(at.y())), pattern);
    QList<QPointF> canvas;
    canvas << point;
    for (int f = 1; f < frames; ++f) {
        const double t = from + f * step;
        const QImage frame = assets.image(sourceAt(clip, t));
        if (frame.size() != first.size())
            break;
        grey = greyOf(frame);
        // Each offset's mismatch, with the means taken out, so a change of
        // light doesn't throw it.
        double best = 1e300;
        int bx = 0, by = 0;
        std::vector<double> scores(size_t(2 * search + 1) * (2 * search + 1), 1e300);
        double patternMean = 0;
        for (const float v : pattern)
            patternMean += v;
        patternMean /= pattern.size();
        const int cx = int(std::lround(at.x())), cy = int(std::lround(at.y()));
        for (int dy = -search; dy <= search; ++dy)
            for (int dx = -search; dx <= search; ++dx) {
                const int x0 = cx + dx, y0 = cy + dy;
                if (x0 - half < 0 || y0 - half < 0 || x0 + half >= w || y0 + half >= h)
                    continue;
                double mean = 0;
                for (int y = -half; y <= half; ++y)
                    for (int x = -half; x <= half; ++x)
                        mean += grey[size_t(y0 + y) * w + x0 + x];
                mean /= pattern.size();
                double sum = 0;
                for (int y = -half; y <= half; ++y)
                    for (int x = -half; x <= half; ++x) {
                        const double d = (grey[size_t(y0 + y) * w + x0 + x] - mean) - (pattern[size_t(y + half) * side + x + half] - patternMean);
                        sum += d * d;
                    }
                scores[size_t(dy + search) * (2 * search + 1) + dx + search] = sum;
                if (sum < best)
                    best = sum, bx = dx, by = dy;
            }
        if (best >= 1e300)
            break;
        // A little finer than a pixel, from the dip's shape either way.
        auto refine = [&](double minus, double centre, double plus) {
            const double curve = minus - 2 * centre + plus;
            return curve > 1e-9 && minus < 1e299 && plus < 1e299 ? std::clamp(.5 * (minus - plus) / curve, -.5, .5) : 0.0;
        };
        auto score = [&](int dx, int dy) {
            if (std::abs(dx) > search || std::abs(dy) > search)
                return 1e300;
            return scores[size_t(dy + search) * (2 * search + 1) + dx + search];
        };
        at = QPointF(cx + bx + refine(score(bx - 1, by), best, score(bx + 1, by)), cy + by + refine(score(bx, by - 1), best, score(bx, by + 1)));
        std::vector<float> now(pattern.size());
        cut(grey, int(std::lround(at.x())), int(std::lround(at.y())), now);
        for (size_t i = 0; i < pattern.size(); ++i)
            pattern[i] = pattern[i] * .8f + now[i] * .2f;
        canvas << layerTransform(scene, assets, video, t).map(at);
    }
    if (path)
        *path = canvas;
    // What follows it: the named layer, or a new empty.
    int target = onto.isEmpty() ? -1 : scene.indexOf(onto);
    if (!onto.isEmpty() && target < 0)
        return fail(QString("There's no layer named \"%1\".").arg(onto));
    if (target < 0)
        target = addEmpty(scene, clip.name + " track", {}, QPointF());
    const int parent = scene.indexOf(scene.layers.at(target).parent);
    QList<Key> xs, ys;
    for (int f = 0; f < canvas.size(); ++f) {
        const double t = from + f * step;
        const QTransform toCanvas = (parent >= 0 ? chainTransform(scene, assets, parent, t) : QTransform()) *
                                    cameraTransform(scene, followOf(scene, target), t);
        const QPointF p = toCanvas.inverted().map(canvas.at(f));
        xs << Key{t, std::round(p.x() * 100) / 100, "linear"};
        ys << Key{t, std::round(p.y() * 100) / 100, "linear"};
    }
    Layer &follower = scene.layers[target];
    // Its own keys outside the tracked stretch stay.
    for (const QString &property : {QString("x"), QString("y")}) {
        QList<Key> keys;
        for (const Key &key : follower.keys.value(property))
            if (key.t < from - 1e-9 || key.t > from + (canvas.size() - 1) * step + 1e-9)
                keys << key;
        keys << (property == "x" ? xs : ys);
        std::sort(keys.begin(), keys.end(), [](const Key &a, const Key &b) { return a.t < b.t; });
        follower.keys[property] = keys;
    }
    return target;
}

bool isModel(const QString &path) {
    static const QStringList kinds{"glb", "gltf", "obj"};
    return kinds.contains(QFileInfo(path).suffix().toLower());
}

int importModel(Scene &scene, const QString &directory, const QString &path, const QVector3D &at, double size, QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return -1;
    };
    if (!isModel(path) || !QFileInfo::exists(path))
        return fail(QString("%1 isn't a model omotion reads (glTF, GLB or OBJ).").arg(path));
    const QDir dir(directory), from = QFileInfo(path).absoluteDir();
    const QString stem = QFileInfo(path).completeBaseName();
    QString folder = "assets/model-" + stem;
    for (int n = 2; dir.exists(folder); ++n)
        folder = QString("assets/model-%1-%2").arg(stem).arg(n);
    dir.mkpath(folder);
    // The file, and what it names beside it: a glTF's buffers and images,
    // an OBJ's materials and their pictures.
    QStringList files{QFileInfo(path).fileName()};
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        const QByteArray bytes = file.readAll();
        const QString suffix = QFileInfo(path).suffix().toLower();
        if (suffix == "gltf") {
            const QJsonObject gltf = QJsonDocument::fromJson(bytes).object();
            for (const QString &list : {"buffers", "images"})
                for (const QJsonValue &item : gltf.value(list).toArray()) {
                    const QString uri = item.toObject().value("uri").toString();
                    if (!uri.isEmpty() && !uri.startsWith("data:"))
                        files << QUrl::fromPercentEncoding(uri.toUtf8());
                }
        } else if (suffix == "obj") {
            for (const QByteArray &line : bytes.split('\n'))
                if (line.trimmed().startsWith("mtllib ")) {
                    const QString library = QString::fromUtf8(line.trimmed().mid(7)).trimmed();
                    files << library;
                    QFile mtl(from.filePath(library));
                    if (mtl.open(QIODevice::ReadOnly))
                        for (const QByteArray &m : mtl.readAll().split('\n'))
                            if (m.trimmed().startsWith("map_"))
                                files << QString::fromUtf8(m.trimmed().split(' ').last());
                }
        }
    }
    for (const QString &name : files) {
        dir.mkpath(QFileInfo(dir.filePath(folder + "/" + name)).absolutePath());
        if (!QFile::copy(from.filePath(name), dir.filePath(folder + "/" + name)) && name == files.first())
            return fail(QString("Couldn't copy %1 into the project.").arg(path));
    }
    Layer layer;
    layer.name = scene.uniqueName(stem);
    layer.base["worldX"] = at.x();
    layer.base["worldY"] = at.y();
    layer.base["worldZ"] = at.z();
    layer.raw["model"] = QJsonObject{{"file", folder + "/" + files.first()}, {"size", size > 0 ? size : 1}};
    scene.layers.append(layer);
    return int(scene.layers.size()) - 1;
}

bool setWarp(Scene &scene, int index, const QString &name, double value, QString *error) {
    if (index < 0 || index >= scene.layers.size() || !QStringList{"amount", "scale", "speed", "seed"}.contains(name) || !std::isfinite(value)) {
        if (error)
            *error = "A warp has an amount, a scale, a speed and a seed.";
        return false;
    }
    QJsonObject warp = scene.layers.at(index).raw.value("warp").toObject();
    warp[name] = name == "seed" ? double(std::lround(value)) : std::max(0.0, value);
    if (name == "amount" && value <= 0)
        scene.layers[index].raw.remove("warp");
    else
        scene.layers[index].raw["warp"] = warp;
    return true;
}

}
