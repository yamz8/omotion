#include "scene.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QPainter>
#include <QSet>
#include <QVarLengthArray>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }
}

const QStringList &Scene::properties() {
    static const QStringList list{"x", "y", "scale", "scaleX", "scaleY", "rotation", "opacity",
                                  "brightness", "contrast", "saturation", "blur", "glow"};
    return list;
}

const QStringList &Scene::lookProperties() {
    static const QStringList list{"brightness", "contrast", "saturation", "blur", "glow"};
    return list;
}

const QStringList &Scene::blends() {
    static const QStringList list{"normal", "add", "screen", "multiply", "overlay", "lighten", "darken"};
    return list;
}

const QStringList &Scene::eases() {
    static const QStringList list{"linear", "in", "out", "inOut", "outBack", "hold"};
    return list;
}

double Scene::defaultValue(const QString &property) {
    if (property == "scale" || property == "scaleX" || property == "scaleY" || property == "opacity" ||
        property == "contrast" || property == "saturation")
        return 1;
    return 0;
}

double Scene::ease(const QString &name, double p) {
    p = clamp01(p);
    if (name == "in")
        return p * p * p;
    if (name == "out")
        return 1 - std::pow(1 - p, 3);
    if (name == "inOut")
        return p < .5 ? 4 * p * p * p : 1 - std::pow(-2 * p + 2, 3) / 2;
    if (name == "outBack") {
        const double c1 = 1.70158, c3 = c1 + 1;
        return 1 + c3 * std::pow(p - 1, 3) + c1 * std::pow(p - 1, 2);
    }
    if (name == "hold")
        return p < 1 ? 0 : 1;
    return p;
}

Scene Scene::fromJson(const QJsonObject &json, QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return Scene();
    };

    Scene scene;
    scene.raw = json;
    scene.width = json.value("width").toInt(scene.width);
    scene.height = json.value("height").toInt(scene.height);
    scene.fps = json.value("fps").toDouble(scene.fps);
    scene.duration = json.value("duration").toDouble(scene.duration);
    scene.background = QColor(json.value("background").toString("#000000"));
    if (scene.width < 2 || scene.height < 2 || scene.width > 8192 || scene.height > 8192)
        return fail("The canvas must be between 2 and 8192 pixels on each side.");
    if (scene.fps <= 0 || scene.fps > 240)
        return fail("fps must be between 0 and 240.");
    if (scene.duration <= 0)
        return fail("duration must be more than 0 seconds.");
    if (!scene.background.isValid())
        return fail("background must be a color like \"#000000\".");

    QSet<QString> names;
    const QJsonArray layers = json.value("layers").toArray();
    for (int i = 0; i < layers.size(); ++i) {
        const QJsonObject object = layers.at(i).toObject();
        Layer layer;
        layer.raw = object;
        layer.name = object.value("name").toString(QString("layer %1").arg(i + 1));
        layer.source = object.value("source").toString();
        layer.parent = object.value("parent").toString();
        layer.visible = object.value("visible").toBool(true);
        layer.blend = object.value("blend").toString("normal");
        if (!blends().contains(layer.blend))
            return fail(QString("Layer \"%1\" blends as \"%2\"; use one of %3.")
                            .arg(object.value("name").toString(), layer.blend, blends().join(", ")));
        if (layer.source.isEmpty())
            return fail(QString("Layer \"%1\" has no source image.").arg(layer.name));
        if (names.contains(layer.name))
            return fail(QString("Two layers are named \"%1\"; names must be unique.").arg(layer.name));
        names.insert(layer.name);

        const QJsonArray anchor = object.value("anchor").toArray();
        if (anchor.size() == 2)
            layer.anchor = QPointF(anchor.at(0).toDouble(.5), anchor.at(1).toDouble(.5));

        for (const QString &property : properties())
            layer.base.insert(property, object.value(property).toDouble(defaultValue(property)));

        const QJsonObject keys = object.value("keys").toObject();
        for (auto it = keys.begin(); it != keys.end(); ++it) {
            if (!properties().contains(it.key()))
                return fail(QString("Layer \"%1\" animates \"%2\", which isn't a property (%3).")
                                .arg(layer.name, it.key(), properties().join(", ")));
            QList<Key> list;
            for (const auto &entry : it.value().toArray()) {
                const QJsonObject k = entry.toObject();
                Key key{k.value("t").toDouble(), k.value("v").toDouble(), k.value("ease").toString("linear")};
                if (!eases().contains(key.ease))
                    return fail(QString("Layer \"%1\" uses easing \"%2\"; use one of %3.")
                                    .arg(layer.name, key.ease, eases().join(", ")));
                list.append(key);
            }
            std::stable_sort(list.begin(), list.end(), [](const Key &a, const Key &b) { return a.t < b.t; });
            if (!list.isEmpty())
                layer.keys.insert(it.key(), list);
        }
        scene.layers.append(layer);
    }

    for (const Layer &layer : scene.layers) {
        // Walk up the parents to catch unknown names and loops.
        QSet<QString> seen{layer.name};
        QString parent = layer.parent;
        while (!parent.isEmpty()) {
            const int index = scene.indexOf(parent);
            if (index < 0)
                return fail(QString("Layer \"%1\" has parent \"%2\", which doesn't exist.").arg(layer.name, parent));
            if (seen.contains(parent))
                return fail(QString("Layer \"%1\" is its own ancestor.").arg(layer.name));
            seen.insert(parent);
            parent = scene.layers.at(index).parent;
        }
    }
    if (error)
        error->clear();
    return scene;
}

QJsonObject Scene::toJson() const {
    QJsonObject json = raw;
    json.insert("version", 1);
    json.insert("width", width);
    json.insert("height", height);
    json.insert("fps", fps);
    json.insert("duration", duration);
    // Keep the alpha when the background is see-through, for WebM and GIF.
    json.insert("background", background.name(background.alpha() < 255 ? QColor::HexArgb : QColor::HexRgb));

    QJsonArray array;
    for (const Layer &layer : layers) {
        QJsonObject object = layer.raw;
        object.insert("name", layer.name);
        object.insert("source", layer.source);
        if (layer.parent.isEmpty())
            object.remove("parent");
        else
            object.insert("parent", layer.parent);
        object.insert("visible", layer.visible);
        object.insert("anchor", QJsonArray{layer.anchor.x(), layer.anchor.y()});
        if (layer.blend == "normal")
            object.remove("blend");
        else
            object.insert("blend", layer.blend);
        // Looks only appear once they're used, so plain layers stay short.
        for (const QString &property : properties()) {
            const double v = layer.base.value(property, defaultValue(property));
            if (lookProperties().contains(property) && v == defaultValue(property))
                object.remove(property);
            else
                object.insert(property, v);
        }

        QJsonObject keys;
        for (auto it = layer.keys.begin(); it != layer.keys.end(); ++it) {
            QJsonArray list;
            for (const Key &key : it.value())
                list.append(QJsonObject{{"t", key.t}, {"v", key.v}, {"ease", key.ease}});
            keys.insert(it.key(), list);
        }
        if (keys.isEmpty())
            object.remove("keys");
        else
            object.insert("keys", keys);
        array.append(object);
    }
    json.insert("layers", array);
    return json;
}

int Scene::indexOf(const QString &name) const {
    for (int i = 0; i < layers.size(); ++i)
        if (layers.at(i).name == name)
            return i;
    return -1;
}

QString Scene::uniqueName(const QString &wanted) const {
    const QString stem = wanted.isEmpty() ? QString("layer") : wanted;
    QString name = stem;
    for (int n = 2; indexOf(name) >= 0; ++n)
        name = QString("%1 %2").arg(stem).arg(n);
    return name;
}

int Scene::frameCount() const { return std::max(1, int(std::lround(duration * fps))); }

double Scene::frameTime(int frame) const { return frame / fps; }

double Scene::value(const Layer &layer, const QString &property, double t) const {
    const auto it = layer.keys.constFind(property);
    if (it == layer.keys.constEnd() || it->isEmpty())
        return layer.base.value(property, defaultValue(property));

    const QList<Key> &keys = *it;
    if (t <= keys.first().t)
        return keys.first().v;
    if (t >= keys.last().t)
        return keys.last().v;
    for (int i = 0; i + 1 < keys.size(); ++i) {
        const Key &a = keys.at(i), &b = keys.at(i + 1);
        if (t < b.t) {
            const double span = b.t - a.t;
            const double p = span <= 0 ? 1 : (t - a.t) / span;
            return a.v + (b.v - a.v) * ease(a.ease, p);
        }
    }
    return keys.last().v;
}

void Scene::setValue(int index, const QString &property, double t, double v) {
    if (index < 0 || index >= layers.size())
        return;
    Layer &layer = layers[index];
    auto it = layer.keys.find(property);
    if (it == layer.keys.end() || it->isEmpty()) {
        layer.base.insert(property, v);
        return;
    }
    const double tolerance = .5 / fps;
    for (Key &key : *it) {
        if (std::abs(key.t - t) < tolerance) {
            key.v = v;
            return;
        }
    }
    QList<Key> &keys = *it;
    // A new key between two others takes the easing of the one before it,
    // so the segment it splits keeps its feel.
    QString ease = "inOut";
    int insertAt = 0;
    while (insertAt < keys.size() && keys.at(insertAt).t < t)
        ++insertAt;
    if (insertAt > 0)
        ease = keys.at(insertAt - 1).ease;
    keys.insert(insertAt, Key{t, v, ease});
}

void Scene::keyProperty(int index, const QString &property, double t) {
    if (index < 0 || index >= layers.size() || !properties().contains(property))
        return;
    const double v = value(layers.at(index), property, t);
    Layer &layer = layers[index];
    if (layer.keys.value(property).isEmpty())
        layer.keys.insert(property, {Key{t, v, "inOut"}});
    else
        setValue(index, property, t, v);
}

void Scene::keyAll(int index, double t) {
    if (index < 0 || index >= layers.size())
        return;
    for (const QString &property : properties()) {
        const Layer &layer = layers.at(index);
        const bool used = layer.keys.contains(property) ||
                          layer.base.value(property, defaultValue(property)) != defaultValue(property);
        if (!lookProperties().contains(property) || used)
            keyProperty(index, property, t);
    }
}

bool Scene::removeKeysAt(int index, double t) {
    if (index < 0 || index >= layers.size())
        return false;
    Layer &layer = layers[index];
    const double tolerance = .5 / fps;
    bool removed = false;
    for (auto it = layer.keys.begin(); it != layer.keys.end();) {
        // The last key going away leaves the property constant at that value.
        QList<Key> &keys = *it;
        for (int i = keys.size() - 1; i >= 0; --i) {
            if (std::abs(keys.at(i).t - t) < tolerance) {
                if (keys.size() == 1)
                    layer.base.insert(it.key(), keys.at(i).v);
                keys.removeAt(i);
                removed = true;
            }
        }
        it = keys.isEmpty() ? layer.keys.erase(it) : std::next(it);
    }
    return removed;
}

QString Scene::cycleEaseAt(int index, double t) {
    if (index < 0 || index >= layers.size())
        return {};
    const double tolerance = .5 / fps;
    QString next;
    for (auto &keys : layers[index].keys) {
        for (Key &key : keys) {
            if (std::abs(key.t - t) >= tolerance)
                continue;
            if (next.isEmpty())
                next = eases().at((eases().indexOf(key.ease) + 1) % eases().size());
            key.ease = next;
        }
    }
    return next;
}

bool KeyRef::operator==(const KeyRef &other) const {
    return layer == other.layer && std::abs(t - other.t) < 1e-6;
}

double Scene::snap(double t) const { return std::clamp(std::round(t * fps) / fps, 0.0, duration); }

bool Scene::setEaseAt(int index, double t, const QString &ease) {
    if (index < 0 || index >= layers.size() || !eases().contains(ease))
        return false;
    const double tolerance = .5 / fps;
    bool found = false;
    for (auto &keys : layers[index].keys)
        for (Key &key : keys)
            if (std::abs(key.t - t) < tolerance) {
                key.ease = ease;
                found = true;
            }
    return found;
}

namespace {
// Puts a key into a property's list in time order, replacing one already
// within half a frame of it.
void placeKey(QList<Key> &keys, const Key &key, double tolerance) {
    keys.erase(std::remove_if(keys.begin(), keys.end(),
                              [&](const Key &k) { return std::abs(k.t - key.t) < tolerance; }),
               keys.end());
    auto at = std::lower_bound(keys.begin(), keys.end(), key.t, [](const Key &k, double t) { return k.t < t; });
    keys.insert(at, key);
}
}

QList<KeyRef> Scene::shiftKeys(const QList<KeyRef> &refs, double delta) {
    const double tolerance = .5 / fps;
    struct Lifted {
        int layer;
        QString property;
        Key key;
    };
    // Lift every selected key off first, so columns moving past each other
    // don't trample one another, then set them all down at their new times.
    QList<Lifted> lifted;
    for (const KeyRef &ref : refs) {
        if (ref.layer < 0 || ref.layer >= layers.size())
            continue;
        for (auto it = layers[ref.layer].keys.begin(); it != layers[ref.layer].keys.end(); ++it) {
            QList<Key> &keys = *it;
            for (int i = keys.size() - 1; i >= 0; --i)
                if (std::abs(keys.at(i).t - ref.t) < tolerance)
                    lifted.append({ref.layer, it.key(), keys.takeAt(i)});
        }
    }
    QList<KeyRef> moved;
    for (Lifted &item : lifted) {
        item.key.t = snap(item.key.t + delta);
        placeKey(layers[item.layer].keys[item.property], item.key, tolerance);
        const KeyRef ref{item.layer, item.key.t};
        if (!moved.contains(ref))
            moved.append(ref);
    }
    return moved;
}

QList<KeyClip> Scene::copyKeys(const QList<KeyRef> &refs) const {
    const double tolerance = .5 / fps;
    QList<KeyClip> clip;
    double first = std::numeric_limits<double>::max();
    for (const KeyRef &ref : refs)
        first = std::min(first, ref.t);
    for (const KeyRef &ref : refs) {
        if (ref.layer < 0 || ref.layer >= layers.size())
            continue;
        const Layer &layer = layers.at(ref.layer);
        for (auto it = layer.keys.begin(); it != layer.keys.end(); ++it)
            for (const Key &key : *it)
                if (std::abs(key.t - ref.t) < tolerance)
                    clip.append({it.key(), Key{key.t - first, key.v, key.ease}});
    }
    return clip;
}

QList<KeyRef> Scene::pasteKeys(int index, double t, const QList<KeyClip> &clip) {
    QList<KeyRef> pasted;
    if (index < 0 || index >= layers.size())
        return pasted;
    for (const KeyClip &item : clip) {
        Key key = item.key;
        key.t = snap(t + key.t);
        placeKey(layers[index].keys[item.property], key, .5 / fps);
        const KeyRef ref{index, key.t};
        if (!pasted.contains(ref))
            pasted.append(ref);
    }
    return pasted;
}

QList<double> Scene::keyTimes(int index) const {
    QList<double> times;
    if (index < 0 || index >= layers.size())
        return times;
    for (const auto &keys : layers.at(index).keys)
        for (const Key &key : keys)
            times.append(key.t);
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end(),
                            [this](double a, double b) { return std::abs(a - b) < .5 / fps; }),
                times.end());
    return times;
}

void AssetCache::setDirectory(const QString &directory) {
    if (directory == m_directory)
        return;
    m_directory = directory;
    m_entries.clear();
    m_looks.clear();
    m_lookOrder.clear();
    m_missing.clear();
}

QImage AssetCache::image(const QString &source) {
    const QString path = QDir(m_directory).absoluteFilePath(source);
    const QFileInfo info(path);
    if (!info.exists()) {
        if (!m_missing.contains(source))
            m_missing.append(source);
        m_entries.remove(source);
        return {};
    }
    auto it = m_entries.find(source);
    if (it != m_entries.end() && it->modified == info.lastModified())
        return it->image;

    QImage image(path);
    if (image.isNull()) {
        if (!m_missing.contains(source))
            m_missing.append(source);
        return {};
    }
    image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    m_entries.insert(source, Entry{image, info.lastModified()});
    m_missing.removeAll(source);
    return image;
}

QSizeF AssetCache::size(const QString &source) { return image(source).size(); }

QImage AssetCache::mip(const QString &source, double scale, double *factor) {
    QImage full = image(source);
    *factor = 1;
    if (full.isNull())
        return full;
    Entry &entry = m_entries[source];
    QImage current = full;
    for (int level = 0; *factor / 2 >= scale && current.width() > 16 && current.height() > 16; ++level) {
        if (level >= entry.mips.size())
            entry.mips.append(current.scaled(current.size() / 2, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
        current = entry.mips.at(level);
        *factor /= 2;
    }
    // The halves are rounded down, so the real factor is the size ratio.
    *factor = double(current.width()) / full.width();
    return current;
}

namespace {

// Brightness, contrast and saturation, worked on premultiplied pixels, so
// "towards white" means towards the pixel's own alpha.
void tone(QImage &image, double brightness, double contrast, double saturation) {
    for (int y = 0; y < image.height(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const int a = qAlpha(line[x]);
            if (a == 0)
                continue;
            double c[3] = {double(qRed(line[x])), double(qGreen(line[x])), double(qBlue(line[x]))};
            const double grey = .299 * c[0] + .587 * c[1] + .114 * c[2];
            for (double &v : c) {
                v = grey + (v - grey) * saturation;
                v = (v - a / 2.0) * contrast + a / 2.0;
                v = brightness >= 0 ? v + (a - v) * brightness : v * (1 + brightness);
                v = std::clamp(v, 0.0, double(a));
            }
            line[x] = qRgba(int(c[0] + .5), int(c[1] + .5), int(c[2] + .5), a);
        }
    }
}

void blurLine(uchar *line, int count, int stride, int radius, QVarLengthArray<uchar> &scratch) {
    scratch.resize(count);
    for (int i = 0; i < count; ++i)
        scratch[i] = line[i * stride];
    const int window = 2 * radius + 1;
    int sum = 0;
    for (int i = -radius; i <= radius; ++i)
        sum += scratch[std::clamp(i, 0, count - 1)];
    for (int i = 0; i < count; ++i) {
        line[i * stride] = uchar(sum / window);
        sum += scratch[std::min(i + radius + 1, count - 1)] - scratch[std::max(i - radius, 0)];
    }
}

int blurRadius(double width) { return int(std::lround(width / 2.56)); }

}

void soften(QImage &image, double width) {
    const int radius = blurRadius(width);
    if (radius < 1 || image.isNull())
        return;
    const int channels = image.depth() == 8 ? 1 : 4;
    QVarLengthArray<uchar> scratch;
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < image.height(); ++y)
            for (int c = 0; c < channels; ++c)
                blurLine(image.scanLine(y) + c, image.width(), channels, radius, scratch);
        for (int x = 0; x < image.width(); ++x)
            for (int c = 0; c < channels; ++c)
                blurLine(image.bits() + x * channels + c, image.height(), int(image.bytesPerLine()), radius, scratch);
    }
}

const AssetCache::Look &AssetCache::look(const QString &source, double factor, double brightness, double contrast,
                                         double saturation, double blur, double glow, double canvasPerPixel) {
    // Pixel sizes are bucketed, so a slow zoom doesn't redo the work every frame.
    const double bucket = std::exp(std::round(std::log(std::max(canvasPerPixel, 1e-6)) * 25) / 25);
    const QString key = QString("%1|%2|%3|%4|%5|%6|%7|%8|%9")
                            .arg(source)
                            .arg(m_entries.value(source).modified.toMSecsSinceEpoch())
                            .arg(factor)
                            .arg(brightness)
                            .arg(contrast)
                            .arg(saturation)
                            .arg(blur)
                            .arg(glow)
                            .arg(bucket);
    auto found = m_looks.find(key);
    if (found != m_looks.end())
        return *found;

    double f = 0;
    const QImage base = mip(source, factor, &f);
    // Blur and glow are in canvas pixels; here they're in this copy's pixels.
    const double blurPixels = blur / bucket, glowPixels = glow / bucket;
    Look look;
    look.margin = blurPixels > 0 || glowPixels > 0 ? 3 * (blurRadius(blurPixels) + blurRadius(glowPixels)) + 2 : 0;
    look.image = QImage(base.width() + 2 * look.margin, base.height() + 2 * look.margin,
                        QImage::Format_ARGB32_Premultiplied);
    look.image.fill(Qt::transparent);
    {
        QPainter painter(&look.image);
        painter.drawImage(look.margin, look.margin, base);
    }
    if (brightness != 0 || contrast != 1 || saturation != 1)
        tone(look.image, std::clamp(brightness, -1.0, 1.0), std::max(0.0, contrast), std::max(0.0, saturation));
    soften(look.image, blurPixels);
    if (glowPixels > 0) {
        look.glow = look.image.copy();
        soften(look.glow, glowPixels);
    }

    // A small cache: animated looks change every frame anyway.
    m_lookOrder.append(key);
    if (m_lookOrder.size() > 24)
        m_looks.remove(m_lookOrder.takeFirst());
    return *m_looks.insert(key, look);
}

QTransform layerTransform(const Scene &scene, AssetCache &assets, int index, double t) {
    QTransform transform;
    QSet<int> seen;
    // Built from the layer up through its parents: each one maps its own
    // pixels into its parent's pixels, and the top one into the canvas.
    while (index >= 0 && index < scene.layers.size() && !seen.contains(index)) {
        seen.insert(index);
        const Layer &layer = scene.layers.at(index);
        const QSizeF size = assets.size(layer.source);
        const double scale = scene.value(layer, "scale", t);
        QTransform local;
        local.translate(scene.value(layer, "x", t), scene.value(layer, "y", t));
        local.rotate(scene.value(layer, "rotation", t));
        local.scale(scale * scene.value(layer, "scaleX", t), scale * scene.value(layer, "scaleY", t));
        local.translate(-layer.anchor.x() * size.width(), -layer.anchor.y() * size.height());
        transform = transform * local;
        index = layer.parent.isEmpty() ? -1 : scene.indexOf(layer.parent);
    }
    return transform;
}

namespace {
QPainter::CompositionMode compositionFor(const QString &blend) {
    if (blend == "add")
        return QPainter::CompositionMode_Plus;
    if (blend == "screen")
        return QPainter::CompositionMode_Screen;
    if (blend == "multiply")
        return QPainter::CompositionMode_Multiply;
    if (blend == "overlay")
        return QPainter::CompositionMode_Overlay;
    if (blend == "lighten")
        return QPainter::CompositionMode_Lighten;
    if (blend == "darken")
        return QPainter::CompositionMode_Darken;
    return QPainter::CompositionMode_SourceOver;
}
}

void renderScene(QPainter &painter, const Scene &scene, AssetCache &assets, double t) {
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setRenderHint(QPainter::Antialiasing);
    const QTransform base = painter.transform();
    // Output pixels per canvas pixel: the view's zoom, or the export's size.
    const double outputScale = std::sqrt(std::abs(base.determinant()));
    painter.fillRect(QRectF(0, 0, scene.width, scene.height), scene.background);
    painter.setClipRect(QRectF(0, 0, scene.width, scene.height));
    for (int i = 0; i < scene.layers.size(); ++i) {
        const Layer &layer = scene.layers.at(i);
        const double opacity = clamp01(scene.value(layer, "opacity", t));
        if (!layer.visible || opacity <= 0)
            continue;
        const QTransform toOutput = layerTransform(scene, assets, i, t) * base;
        // How many output pixels one image pixel covers, along its longer axis.
        const double scale = std::max(std::hypot(toOutput.m11(), toOutput.m12()),
                                      std::hypot(toOutput.m21(), toOutput.m22()));
        double factor = 1;
        QImage image = assets.mip(layer.source, scale, &factor);
        if (image.isNull())
            continue;

        QImage glow;
        int margin = 0;
        const double brightness = scene.value(layer, "brightness", t), contrast = scene.value(layer, "contrast", t),
                     saturation = scene.value(layer, "saturation", t), blur = std::max(0.0, scene.value(layer, "blur", t)),
                     glowSize = std::max(0.0, scene.value(layer, "glow", t));
        if (brightness != 0 || contrast != 1 || saturation != 1 || blur > 0 || glowSize > 0) {
            const double canvasPerPixel = scale / factor / std::max(outputScale, 1e-6);
            const AssetCache::Look &look =
                assets.look(layer.source, factor, brightness, contrast, saturation, blur, glowSize, canvasPerPixel);
            image = look.image;
            glow = look.glow;
            margin = look.margin;
        }

        // The copy is drawn back up to the image's full size, less its padding.
        const QTransform fromCopy(1 / factor, 0, 0, 1 / factor, -margin / factor, -margin / factor);
        painter.setTransform(fromCopy * toOutput);
        painter.setOpacity(opacity);
        painter.setCompositionMode(compositionFor(layer.blend));
        painter.drawImage(QPointF(0, 0), image);
        if (!glow.isNull()) {
            painter.setCompositionMode(QPainter::CompositionMode_Screen);
            painter.drawImage(QPointF(0, 0), glow);
        }
    }
    painter.restore();
}

QImage renderFrame(const Scene &scene, AssetCache &assets, double t, const QSize &size) {
    const QSize target = size.isValid() ? size : QSize(scene.width, scene.height);
    QImage frame(target, QImage::Format_ARGB32_Premultiplied);
    frame.fill(Qt::transparent);
    QPainter painter(&frame);
    painter.scale(double(target.width()) / scene.width, double(target.height()) / scene.height);
    renderScene(painter, scene, assets, t);
    return frame;
}

int layerAt(const Scene &scene, AssetCache &assets, double t, const QPointF &point) {
    for (int i = scene.layers.size() - 1; i >= 0; --i) {
        const Layer &layer = scene.layers.at(i);
        if (!layer.visible || scene.value(layer, "opacity", t) <= 0)
            continue;
        const QImage image = assets.image(layer.source);
        bool invertible = false;
        const QTransform inverse = layerTransform(scene, assets, i, t).inverted(&invertible);
        if (image.isNull() || !invertible)
            continue;
        const QPoint local = inverse.map(point).toPoint();
        if (image.rect().contains(local) && qAlpha(image.pixel(local)) > 16)
            return i;
    }
    return -1;
}
