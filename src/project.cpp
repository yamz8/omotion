#include "project.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QPainter>
#include <QPainterPath>
#include <QSaveFile>
#include <algorithm>
#include <cmath>

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
    if (file.open(QIODevice::WriteOnly) && image.save(&file, "PNG") && file.commit())
        return true;
    if (error)
        *error = QString("Couldn't write %1: %2").arg(path, file.errorString());
    return false;
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

int cutOut(Scene &scene, AssetCache &assets, int index, const QPolygonF &polygon, double t,
           bool punch, QString *error, const QString &wantedName) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return -1;
    };
    if (index < 0 || index >= scene.layers.size())
        return fail("Select the layer to cut a piece out of.");
    if (polygon.size() < 3)
        return fail("Click at least three points around the piece.");

    const Layer source = scene.layers.at(index);
    const QImage image = assets.image(source.source);
    if (image.isNull())
        return fail(QString("Couldn't read %1.").arg(source.source));
    bool invertible = false;
    const QTransform inverse = layerTransform(scene, assets, index, t).inverted(&invertible);
    if (!invertible)
        return fail("The layer is scaled to nothing at this point in time.");

    QPainterPath path;
    path.addPolygon(inverse.map(polygon));
    path.closeSubpath();
    const QRect bounds = path.boundingRect().toAlignedRect() & image.rect();
    if (bounds.isEmpty())
        return fail("The shape doesn't cover any of the layer.");

    QImage piece(bounds.size(), QImage::Format_ARGB32_Premultiplied);
    piece.fill(Qt::transparent);
    {
        QPainter painter(&piece);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.translate(-bounds.topLeft());
        painter.setClipPath(path);
        painter.drawImage(0, 0, image);
    }
    bool opaque = false;
    for (int y = 0; y < piece.height() && !opaque; ++y)
        for (int x = 0; x < piece.width() && !opaque; ++x)
            opaque = qAlpha(piece.pixel(x, y)) > 0;
    if (!opaque)
        return fail("The shape only covers transparent pixels.");

    const QDir dir(assets.directory());
    dir.mkpath("assets");
    const QDir assetDir(dir.filePath("assets"));
    if (!wantedName.isEmpty() && scene.indexOf(wantedName) >= 0)
        return fail(QString("There's already a layer named \"%1\".").arg(wantedName));
    const QString name = wantedName.isEmpty() ? scene.uniqueName(source.name + " piece") : wantedName;
    const QString pieceSource = "assets/" + uniqueFile(assetDir, slug(name), "png");
    if (!writeImage(piece, dir.filePath(pieceSource), error))
        return -1;

    if (punch) {
        QImage holed = image.copy();
        {
            QPainter painter(&holed);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setCompositionMode(QPainter::CompositionMode_Clear);
            painter.fillPath(path, Qt::black);
        }
        const QString holedSource = "assets/" + uniqueFile(assetDir, slug(source.name), "png");
        if (!writeImage(holed, dir.filePath(holedSource), error))
            return -1;
        scene.layers[index].source = holedSource;
    }

    // The piece lives in its source layer's pixel space, so x and y are where
    // its centre sat in the source image, and it moves with the source.
    Layer layer;
    layer.name = name;
    layer.source = pieceSource;
    layer.parent = source.name;
    for (const QString &property : Scene::properties())
        layer.base.insert(property, Scene::defaultValue(property));
    const QPointF centre = QRectF(bounds).center();
    layer.base["x"] = centre.x();
    layer.base["y"] = centre.y();
    scene.layers.insert(index + 1, layer);
    if (error)
        error->clear();
    return index + 1;
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
