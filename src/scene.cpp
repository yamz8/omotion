#include "scene.h"
#include "world.h"

#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QLineF>
#include <QPainter>
#include <QPainterPath>
#include <QRandomGenerator>
#include <QRadialGradient>
#include <QtMath>
#include <QRegularExpression>
#include <QSet>
#include <QVarLengthArray>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>

namespace {
double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }
}

const QStringList &Scene::properties() {
    static const QStringList list{"x", "y", "z", "scale", "scaleX", "scaleY", "rotation", "opacity",
                                  "brightness", "contrast", "saturation", "blur", "glow",
                                  "worldX", "worldY", "worldZ", "yaw", "pitch", "roll"};
    return list;
}

const QStringList &Scene::optionalProperties() {
    static const QStringList list = QStringList(lookProperties()) << "z" << worldProperties();
    return list;
}

QPointF Scene::vanishPoint() const {
    return std::isnan(vanish.x()) ? QPointF(width / 2.0, height / 2.0) : vanish;
}

double Scene::focalDistance() const { return std::isnan(distance) ? double(width) : distance; }

const QStringList &Scene::lookProperties() {
    static const QStringList list{"brightness", "contrast", "saturation", "blur", "glow"};
    return list;
}

const QStringList &Scene::blends() {
    static const QStringList list{"normal", "add", "screen", "multiply", "overlay", "lighten", "darken"};
    return list;
}

const QStringList &Scene::eases() {
    static const QStringList list{"linear", "in", "out", "inOut", "outBack", "spring", "bounce", "hold"};
    return list;
}

const QStringList &Scene::particleKinds() {
    static const QStringList list{"snow", "rain", "dust", "fireflies", "leaves", "embers"};
    return list;
}

QJsonObject Scene::particleDefaults(const QString &kind) {
    // Speeds in canvas pixels a second, directions in degrees (90 is down),
    // sizes in canvas pixels; twinkle is how much they flicker, 0 to 1.
    if (kind == "rain")
        return {{"kind", kind}, {"count", 320}, {"size", 2}, {"speed", 900}, {"direction", 100}, {"spread", 4},
                {"sway", 0}, {"twinkle", 0}, {"color", "#c8d8ff"}, {"seed", 1}};
    if (kind == "dust")
        return {{"kind", kind}, {"count", 140}, {"size", 2.5}, {"speed", 12}, {"direction", 20}, {"spread", 180},
                {"sway", 12}, {"twinkle", .5}, {"color", "#fff4dc"}, {"seed", 1}};
    if (kind == "fireflies")
        return {{"kind", kind}, {"count", 40}, {"size", 3}, {"speed", 10}, {"direction", 0}, {"spread", 180},
                {"sway", 40}, {"twinkle", .9}, {"color", "#ffe27a"}, {"seed", 1}};
    if (kind == "leaves")
        return {{"kind", kind}, {"count", 28}, {"size", 9}, {"speed", 70}, {"direction", 80}, {"spread", 20},
                {"sway", 45}, {"twinkle", 0}, {"color", "#e07a2f"}, {"seed", 1}};
    if (kind == "embers")
        return {{"kind", kind}, {"count", 90}, {"size", 2.5}, {"speed", 60}, {"direction", -90}, {"spread", 25},
                {"sway", 20}, {"twinkle", .6}, {"color", "#ff9a3c"}, {"seed", 1}};
    return {{"kind", "snow"}, {"count", 260}, {"size", 3}, {"speed", 55}, {"direction", 90}, {"spread", 15},
            {"sway", 18}, {"twinkle", 0}, {"color", "#ffffff"}, {"seed", 1}};
}

const QStringList &Scene::cameraProperties() {
    static const QStringList list{"x", "y", "zoom", "rotation", "worldX", "worldY", "worldZ", "yaw", "pitch"};
    return list;
}

const QStringList &Scene::worldProperties() {
    static const QStringList list{"worldX", "worldY", "worldZ", "yaw", "pitch", "roll"};
    return list;
}

bool Scene::isPinProperty(const Layer &layer, const QString &property) {
    static const QRegularExpression pin("^pin(\\d+)[xy]$");
    const auto match = pin.match(property);
    return match.hasMatch() && match.captured(1).toInt() < layer.pins.size();
}

QStringList Scene::propertiesOf(const Layer &layer) {
    QStringList list = properties();
    for (int i = 0; i < layer.pins.size(); ++i)
        list << QString("pin%1x").arg(i) << QString("pin%1y").arg(i);
    return list;
}

Layer *Scene::at(int index) {
    if (index == CameraIndex)
        return &camera;
    return index >= 0 && index < layers.size() ? &layers[index] : nullptr;
}

const Layer *Scene::at(int index) const {
    if (index == CameraIndex)
        return &camera;
    return index >= 0 && index < layers.size() ? &layers.at(index) : nullptr;
}

double Scene::defaultValue(const QString &property) {
    if (property == "scale" || property == "scaleX" || property == "scaleY" || property == "opacity" ||
        property == "contrast" || property == "saturation" || property == "zoom")
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
    QString kind;
    double amount = 0;
    if (physical(name, &kind, &amount)) {
        if (kind == "spring") {
            // Rings down to a thousandth by the next key; the last term takes
            // that thousandth back, so it arrives exactly.
            const double n = std::clamp(amount, .5, 20.0), k = std::log(1000.0);
            return 1 - std::exp(-k * p) * std::cos(2 * M_PI * n * p) + p * std::exp(-k) * std::cos(2 * M_PI * n);
        }
        if (kind == "bounce") {
            // A dropped ball: it falls under gravity, then each bounce keeps
            // half its speed, so it rises a quarter as high and lasts half as
            // long. The fall and the bounces share the time to the next key.
            const int n = std::clamp(int(std::lround(amount)), 0, 10);
            const double r = .5;
            double total = 1;
            for (int i = 1; i <= n; ++i)
                total += 2 * std::pow(r, i);
            const double fall = 1 / total;
            if (p < fall)
                return (p / fall) * (p / fall);
            double start = fall;
            for (int i = 1; i <= n; ++i) {
                const double length = 2 * fall * std::pow(r, i);
                if (p < start + length) {
                    const double u = (p - start) / length;
                    return 1 - std::pow(r, 2 * i) * 4 * u * (1 - u);
                }
                start += length;
            }
            return 1;
        }
        return p;  // An arc goes straight; its lift is added on y.
    }
    double x1, y1, x2, y2;
    if (cubic(name, &x1, &y1, &x2, &y2)) {
        // A curve from (0,0) to (1,1) with handles at (x1,y1) and (x2,y2): find
        // where along it time p is reached, then how far the value has got.
        auto at = [](double a, double b, double u) {
            return 3 * a * u * (1 - u) * (1 - u) + 3 * b * u * u * (1 - u) + u * u * u;
        };
        auto slope = [](double a, double b, double u) {
            return 3 * a * (1 - u) * (1 - u) + 6 * (b - a) * u * (1 - u) + 3 * (1 - b) * u * u;
        };
        double u = p;
        for (int i = 0; i < 8; ++i) {
            const double d = slope(x1, x2, u);
            if (std::abs(d) < 1e-6)
                break;
            u = std::clamp(u - (at(x1, x2, u) - p) / d, 0.0, 1.0);
        }
        // Where Newton's method stalls, halving the interval always gets there.
        if (std::abs(at(x1, x2, u) - p) > 1e-5) {
            double lo = 0, hi = 1;
            for (int i = 0; i < 40; ++i) {
                u = (lo + hi) / 2;
                (at(x1, x2, u) < p ? lo : hi) = u;
            }
        }
        return at(y1, y2, u);
    }
    return p;
}

bool Scene::cubic(const QString &ease, double *x1, double *y1, double *x2, double *y2) {
    static const QRegularExpression pattern(
        R"(^cubic\(\s*(-?[\d.]+)\s*,\s*(-?[\d.]+)\s*,\s*(-?[\d.]+)\s*,\s*(-?[\d.]+)\s*\)$)");
    const auto match = pattern.match(ease);
    if (!match.hasMatch())
        return false;
    bool ok[4];
    const double v[4] = {match.captured(1).toDouble(&ok[0]), match.captured(2).toDouble(&ok[1]),
                         match.captured(3).toDouble(&ok[2]), match.captured(4).toDouble(&ok[3])};
    // Time can't run backwards: the handles' x stay within 0..1.
    if (!ok[0] || !ok[1] || !ok[2] || !ok[3] || v[0] < 0 || v[0] > 1 || v[2] < 0 || v[2] > 1)
        return false;
    *x1 = v[0];
    *y1 = v[1];
    *x2 = v[2];
    *y2 = v[3];
    return true;
}

bool Scene::validEase(const QString &ease) {
    double a, b, c, d;
    QString kind;
    return eases().contains(ease) || cubic(ease, &a, &b, &c, &d) || physical(ease, &kind, &a);
}

bool Scene::physical(const QString &ease, QString *kind, double *amount) {
    if (ease == "spring" || ease == "bounce") {
        *kind = ease;
        *amount = ease == "spring" ? 2 : 3;
        return true;
    }
    static const QRegularExpression pattern(R"(^(spring|bounce|arc)\(\s*([-+]?[\d.]+(?:[eE][-+]?\d+)?)\s*\)$)");
    const auto match = pattern.match(ease);
    bool ok = false;
    const double v = match.hasMatch() ? match.captured(2).toDouble(&ok) : 0;
    if (!ok || !std::isfinite(v))
        return false;
    *kind = match.captured(1);
    *amount = v;
    return true;
}

double Scene::arcLift(const QString &ease, double p) {
    QString kind;
    double height = 0;
    if (!ease.startsWith("arc") || !physical(ease, &kind, &height))
        return 0;
    // Straight across plus a parabola in time: what gravity does to a thing
    // thrown from one key to land on the next.
    p = clamp01(p);
    return height * 4 * p * (1 - p);
}

QString Scene::cubicEase(double x1, double y1, double x2, double y2) {
    auto number = [](double v) { return QString::number(std::round(v * 1000) / 1000); };
    return QString("cubic(%1,%2,%3,%4)").arg(number(std::clamp(x1, 0.0, 1.0)), number(y1),
                                              number(std::clamp(x2, 0.0, 1.0)), number(y2));
}

QList<double> Scene::curveOf(const QString &ease) {
    double x1, y1, x2, y2;
    if (cubic(ease, &x1, &y1, &x2, &y2))
        return {x1, y1, x2, y2};
    // The named ones, as the curves closest to them, for showing and editing.
    if (ease == "in")
        return {.32, 0, .67, 0};
    if (ease == "out")
        return {.33, 1, .68, 1};
    if (ease == "inOut")
        return {.65, 0, .35, 1};
    if (ease == "outBack")
        return {.34, 1.56, .64, 1};
    // Physical easings and hold aren't curves the handles could shape.
    QString kind;
    if (ease == "hold" || physical(ease, &kind, &x1))
        return {};
    return {0, 0, 1, 1};
}

namespace {
// A layer's (or the camera's) "links", checked for what can be told without
// the rest of the scene; where they point is checked once it's all read.
bool readLinks(const QJsonObject &object, Layer &layer, const QStringList &properties, const QString &who,
               QString *error) {
    const QJsonObject links = object.value("links").toObject();
    for (auto it = links.begin(); it != links.end(); ++it) {
        if (!properties.contains(it.key())) {
            *error = QString("%1 links \"%2\", which isn't one of its properties (%3).")
                         .arg(who, it.key(), properties.join(", "));
            return false;
        }
        if (!it.value().isObject() || it.value().toObject().value("from").toString().isEmpty()) {
            *error = QString("%1 links \"%2\" without saying where from, as in {\"from\": \"car.x\"}.")
                         .arg(who, it.key());
            return false;
        }
        const Link link = Scene::linkFromJson(it.value().toObject());
        if (!Scene::linkMeasures().contains(link.of)) {
            *error = QString("%1 links \"%2\" by \"%3\"; \"of\" is one of %4.")
                         .arg(who, it.key(), link.of, Scene::linkMeasures().join(", "));
            return false;
        }
        if (!std::isfinite(link.times) || !std::isfinite(link.lag) || !std::isfinite(link.smooth) ||
            link.smooth < 0 || !std::isfinite(link.spring) || link.spring < 0 || !std::isfinite(link.damping) ||
            link.damping <= 0 || std::isnan(link.min) || std::isnan(link.max) || link.min > link.max) {
            *error = QString("%1 links \"%2\" with numbers that don't work: times and lag are any number, "
                             "smooth and spring from 0, damping over 0, and min no more than max.")
                         .arg(who, it.key());
            return false;
        }
        layer.links.insert(it.key(), link);
    }
    return true;
}

QString formatNumber(double v) { return QString::number(v, 'g', 6); }
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
    const QJsonObject perspective = json.value("perspective").toObject();
    const QJsonArray vanish = perspective.value("vanish").toArray();
    if (vanish.size() == 2)
        scene.vanish = QPointF(vanish.at(0).toDouble(), vanish.at(1).toDouble());
    if (perspective.contains("distance"))
        scene.distance = perspective.value("distance").toDouble(-1);
    if (perspective.contains("distance") && !(scene.distance > 0))
        return fail("perspective's distance must be more than 0 pixels.");

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
        if (object.contains("follow"))
            layer.follow = object.value("follow").toDouble(1);
        for (const QJsonValue &pin : object.value("pins").toArray()) {
            const QJsonArray xy = pin.toArray();
            if (xy.size() == 2)
                layer.pins.append(QPointF(xy.at(0).toDouble(), xy.at(1).toDouble()));
        }
        layer.blend = object.value("blend").toString("normal");
        if (!blends().contains(layer.blend))
            return fail(QString("Layer \"%1\" blends as \"%2\"; use one of %3.")
                            .arg(object.value("name").toString(), layer.blend, blends().join(", ")));
        layer.particles = object.value("particles").toObject();
        if (!layer.particles.isEmpty() && !particleKinds().contains(layer.particles.value("kind").toString()))
            return fail(QString("Layer \"%1\" has particles of kind \"%2\"; use one of %3.")
                            .arg(layer.name, layer.particles.value("kind").toString(), particleKinds().join(", ")));
        // An empty is a point to aim at or move by, with nothing to draw.
        if (layer.source.isEmpty() && layer.particles.isEmpty() && !object.value("empty").toBool() && !object.contains("drawing"))
            return fail(QString("Layer \"%1\" has no source image.").arg(layer.name));
        if (names.contains(layer.name))
            return fail(QString("Two layers are named \"%1\"; names must be unique.").arg(layer.name));
        names.insert(layer.name);

        const QJsonArray anchor = object.value("anchor").toArray();
        if (anchor.size() == 2)
            layer.anchor = QPointF(anchor.at(0).toDouble(.5), anchor.at(1).toDouble(.5));

        for (const QString &property : properties())
            layer.base.insert(property, object.value(property).toDouble(defaultValue(property)));
        for (auto it = object.begin(); it != object.end(); ++it)
            if (isPinProperty(layer, it.key()))
                layer.base.insert(it.key(), it.value().toDouble());

        const QJsonObject keys = object.value("keys").toObject();
        for (auto it = keys.begin(); it != keys.end(); ++it) {
            if (!properties().contains(it.key()) && !isPinProperty(layer, it.key()))
                return fail(QString("Layer \"%1\" animates \"%2\", which isn't a property (%3, or pin<n>x/y for its pins).")
                                .arg(layer.name, it.key(), properties().join(", ")));
            QList<Key> list;
            for (const auto &entry : it.value().toArray()) {
                const QJsonObject k = entry.toObject();
                Key key{k.value("t").toDouble(), k.value("v").toDouble(), k.value("ease").toString("linear")};
                if (!validEase(key.ease))
                    return fail(QString("Layer \"%1\" uses easing \"%2\"; use one of %3.")
                                    .arg(layer.name, key.ease, eases().join(", ") + ", spring(n), bounce(n), arc(h), or cubic(x1,y1,x2,y2)"));
                list.append(key);
            }
            std::stable_sort(list.begin(), list.end(), [](const Key &a, const Key &b) { return a.t < b.t; });
            if (!list.isEmpty())
                layer.keys.insert(it.key(), list);
        }
        QString linkError;
        if (!readLinks(object, layer, propertiesOf(layer), QString("Layer \"%1\"").arg(layer.name), &linkError))
            return fail(linkError);
        scene.layers.append(layer);
    }

    // The camera: looking at the canvas centre, unzoomed, unless it says.
    {
        const QJsonObject camera = json.value("camera").toObject();
        scene.camera.name = "camera";
        scene.camera.raw = camera;
        scene.camera.base["x"] = camera.value("x").toDouble(scene.width / 2.0);
        scene.camera.base["y"] = camera.value("y").toDouble(scene.height / 2.0);
        scene.camera.base["zoom"] = camera.value("zoom").toDouble(1);
        scene.camera.base["rotation"] = camera.value("rotation").toDouble(0);
        for (const QString &property : worldProperties())
            if (camera.contains(property))
                scene.camera.base[property] = camera.value(property).toDouble(0);
        if (scene.camera.base["zoom"] <= 0)
            return fail("The camera's zoom must be more than 0.");
        const QJsonObject keys = camera.value("keys").toObject();
        for (auto it = keys.begin(); it != keys.end(); ++it) {
            if (!cameraProperties().contains(it.key()))
                return fail(QString("The camera animates \"%1\", which isn't one of its properties (%2).")
                                .arg(it.key(), cameraProperties().join(", ")));
            QList<Key> list;
            for (const auto &entry : it.value().toArray()) {
                const QJsonObject k = entry.toObject();
                Key key{k.value("t").toDouble(), k.value("v").toDouble(), k.value("ease").toString("linear")};
                if (!validEase(key.ease))
                    return fail(QString("The camera uses easing \"%1\"; use one of %2.").arg(key.ease, eases().join(", ") + ", spring(n), bounce(n), arc(h), or cubic(x1,y1,x2,y2)"));
                list.append(key);
            }
            std::stable_sort(list.begin(), list.end(), [](const Key &a, const Key &b) { return a.t < b.t; });
            if (!list.isEmpty())
                scene.camera.keys.insert(it.key(), list);
        }
        QString linkError;
        if (!readLinks(camera, scene.camera, cameraProperties(), "The camera", &linkError))
            return fail(linkError);
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
    const QString linkProblem = scene.checkLinks();
    if (!linkProblem.isEmpty())
        return fail(linkProblem);
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
    QJsonObject perspective = raw.value("perspective").toObject();
    if (std::isnan(vanish.x()))
        perspective.remove("vanish");
    else
        perspective.insert("vanish", QJsonArray{vanish.x(), vanish.y()});
    if (std::isnan(distance))
        perspective.remove("distance");
    else
        perspective.insert("distance", distance);
    if (perspective.isEmpty())
        json.remove("perspective");
    else
        json.insert("perspective", perspective);

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
        if (layer.pins.isEmpty()) {
            object.remove("pins");
        } else {
            QJsonArray pins;
            for (const QPointF &pin : layer.pins)
                pins.append(QJsonArray{std::round(pin.x() * 100) / 100, std::round(pin.y() * 100) / 100});
            object.insert("pins", pins);
        }
        // Pin offsets: written for pins it has, dropped for ones it hasn't.
        for (const QString &key : object.keys())
            if (key.startsWith("pin") && key != "pins" && !isPinProperty(layer, key))
                object.remove(key);
        for (int i = 0; i < layer.pins.size(); ++i)
            for (const QString &axis : {QString("x"), QString("y")}) {
                const QString property = QString("pin%1%2").arg(i).arg(axis);
                const double v = layer.base.value(property, 0);
                if (v == 0)
                    object.remove(property);
                else
                    object.insert(property, v);
            }
        if (layer.particles.isEmpty())
            object.remove("particles");
        else
            object.insert("particles", layer.particles);
        if (layer.source.isEmpty())
            object.remove("source");
        if (std::isnan(layer.follow))
            object.remove("follow");
        else
            object.insert("follow", layer.follow);
        // Looks only appear once they're used, so plain layers stay short.
        for (const QString &property : properties()) {
            const double v = layer.base.value(property, defaultValue(property));
            if (optionalProperties().contains(property) && v == defaultValue(property))
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
        QJsonObject links;
        for (auto it = layer.links.begin(); it != layer.links.end(); ++it)
            links.insert(it.key(), linkToJson(it.value()));
        if (links.isEmpty())
            object.remove("links");
        else
            object.insert("links", links);
        array.append(object);
    }
    json.insert("layers", array);

    // The camera appears once it's moved or animated.
    QJsonObject cam = camera.raw;
    const double cx = camera.base.value("x", width / 2.0), cy = camera.base.value("y", height / 2.0);
    const double zoom = camera.base.value("zoom", 1), turn = camera.base.value("rotation", 0);
    QJsonObject cameraKeys;
    for (auto it = camera.keys.begin(); it != camera.keys.end(); ++it) {
        QJsonArray list;
        for (const Key &key : it.value())
            list.append(QJsonObject{{"t", key.t}, {"v", key.v}, {"ease", key.ease}});
        cameraKeys.insert(it.key(), list);
    }
    QJsonObject cameraLinks;
    for (auto it = camera.links.begin(); it != camera.links.end(); ++it)
        cameraLinks.insert(it.key(), linkToJson(it.value()));
    bool moved = false;
    for (const QString &property : worldProperties())
        moved |= camera.base.value(property, 0) != 0;
    if (cameraKeys.isEmpty() && cameraLinks.isEmpty() && cx == width / 2.0 && cy == height / 2.0 && zoom == 1 &&
        turn == 0 && !moved && cam.isEmpty()) {
        json.remove("camera");
    } else {
        cam.insert("x", cx);
        cam.insert("y", cy);
        cam.insert("zoom", zoom);
        cam.insert("rotation", turn);
        for (const QString &property : worldProperties()) {
            const double v = camera.base.value(property, 0);
            if (v == 0)
                cam.remove(property);
            else
                cam.insert(property, v);
        }
        if (cameraKeys.isEmpty())
            cam.remove("keys");
        else
            cam.insert("keys", cameraKeys);
        if (cameraLinks.isEmpty())
            cam.remove("links");
        else
            cam.insert("links", cameraLinks);
        json.insert("camera", cam);
    }
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
    double own = 0;
    if (!constrained(layer, property, t, &own))
        own = ownValue(layer, property, t);
    return layer.links.isEmpty() ? own : own + linkValue(layer, property, t);
}

namespace {
QPointF pointOf(const QJsonValue &value) {
    const QJsonArray a = value.toArray();
    return QPointF(a.at(0).toDouble(), a.at(1).toDouble());
}
}

bool Scene::constrained(const Layer &layer, const QString &property, double t, double *v) const {
    if (property != "rotation" && property != "x" && property != "y")
        return false;
    // Constraints can lean on each other; a loop of them stops somewhere.
    thread_local int depth = 0;
    if (depth > 12)
        return false;
    struct Guard {
        Guard() { ++depth; }
        ~Guard() { --depth; }
    } guard;
    const double degrees = 180 / M_PI;
    auto position = [&](const Layer &of) { return QPointF(value(of, "x", t), value(of, "y", t)); };
    auto spread = [&](const Layer &of) { return value(of, "scale", t) * value(of, "scaleX", t); };
    // Two bones: the upper's constraint turns both.
    auto solveIk = [&](const Layer &upper, const QJsonObject &c, double *upperTurn, double *lowerTurn) {
        const int lowerIndex = indexOf(c.value("lower").toString()), targetIndex = indexOf(c.value("target").toString());
        if (lowerIndex < 0 || targetIndex < 0)
            return false;
        const Layer &lower = layers.at(lowerIndex);
        const QPointF pivot = pointOf(c.value("pivot")), tip = pointOf(c.value("tip"));
        const QPointF joint = position(lower) - pivot;  // The lower's pivot, from the upper's, in its pixels.
        const double first = std::hypot(joint.x(), joint.y()) * spread(upper);
        const double second = std::hypot(tip.x(), tip.y()) * spread(upper) * spread(lower);
        if (first <= 0 || second <= 0)
            return false;
        const QPointF base = position(upper), aim = position(layers.at(targetIndex)) - base;
        const double reach = std::clamp(std::hypot(aim.x(), aim.y()), std::abs(first - second) + 1e-6, first + second - 1e-6);
        const double toward = std::atan2(aim.y(), aim.x());
        const double bend = std::acos(std::clamp((first * first + reach * reach - second * second) / (2 * first * reach), -1.0, 1.0));
        const double upperAngle = toward + (c.value("bend").toInt(1) >= 0 ? -bend : bend);
        const QPointF elbow = base + QPointF(std::cos(upperAngle), std::sin(upperAngle)) * first;
        const QPointF target = base + QPointF(std::cos(toward), std::sin(toward)) * reach;
        const double lowerAngle = std::atan2(target.y() - elbow.y(), target.x() - elbow.x());
        const double restUpper = std::atan2(joint.y(), joint.x()), restLower = std::atan2(tip.y(), tip.x());
        *upperTurn = (upperAngle - restUpper) * degrees;
        *lowerTurn = (lowerAngle - upperAngle - restLower + restUpper) * degrees;
        return true;
    };
    for (const QJsonValue &entry : layer.raw.value("constraints").toArray()) {
        const QJsonObject c = entry.toObject();
        const QString type = c.value("type").toString();
        if (type == "ik" && property == "rotation") {
            double upper = 0, lower = 0;
            if (solveIk(layer, c, &upper, &lower)) {
                *v = upper;
                return true;
            }
        } else if (type == "lookAt" && property == "rotation") {
            const int target = indexOf(c.value("target").toString());
            if (target < 0)
                continue;
            const QPointF aim = position(layers.at(target)) - position(layer);
            *v = std::atan2(aim.y(), aim.x()) * degrees - c.value("axis").toDouble();
            return true;
        } else if (type == "path") {
            const QJsonArray points = c.value("points").toArray();
            if (points.size() < 2)
                continue;
            const double start = c.value("start").toDouble(), end = c.value("end").toDouble(duration);
            const double p = ease(c.value("ease").toString("inOut"), std::clamp(end > start ? (t - start) / (end - start) : 1, 0.0, 1.0));
            double total = 0;
            for (int i = 1; i < points.size(); ++i)
                total += QLineF(pointOf(points.at(i - 1)), pointOf(points.at(i))).length();
            double left = p * total;
            QPointF at = pointOf(points.last()), way = pointOf(points.last()) - pointOf(points.at(points.size() - 2));
            for (int i = 1; i < points.size(); ++i) {
                const QLineF piece(pointOf(points.at(i - 1)), pointOf(points.at(i)));
                if (left <= piece.length() || i == points.size() - 1) {
                    at = piece.pointAt(piece.length() > 0 ? std::min(1.0, left / piece.length()) : 0);
                    way = piece.p2() - piece.p1();
                    break;
                }
                left -= piece.length();
            }
            if (property == "x" || property == "y") {
                *v = property == "x" ? at.x() : at.y();
                return true;
            }
            if (c.value("turn").toBool()) {
                *v = std::atan2(way.y(), way.x()) * degrees - c.value("axis").toDouble();
                return true;
            }
        }
    }
    // A lower bone is turned by its upper's constraint.
    if (property == "rotation" && !layer.parent.isEmpty()) {
        const int parent = indexOf(layer.parent);
        if (parent >= 0)
            for (const QJsonValue &entry : layers.at(parent).raw.value("constraints").toArray()) {
                const QJsonObject c = entry.toObject();
                double upper = 0, lower = 0;
                if (c.value("type").toString() == "ik" && c.value("lower").toString() == layer.name &&
                    solveIk(layers.at(parent), c, &upper, &lower)) {
                    *v = lower;
                    return true;
                }
            }
    }
    return false;
}

namespace {
// Between keyframes, by each one's easing.
double interpolate(const QList<Key> &keys, double t, const QString &property) {
    if (t <= keys.first().t)
        return keys.first().v;
    if (t >= keys.last().t)
        return keys.last().v;
    for (int i = 0; i + 1 < keys.size(); ++i) {
        const Key &a = keys.at(i), &b = keys.at(i + 1);
        if (t < b.t) {
            const double span = b.t - a.t;
            const double p = span <= 0 ? 1 : (t - a.t) / span;
            // Up is less y on the canvas, more worldY in a world.
            const double lift = property == "y" ? -Scene::arcLift(a.ease, p) : property == "worldY" ? Scene::arcLift(a.ease, p) : 0;
            return a.v + (b.v - a.v) * Scene::ease(a.ease, p) + lift;
        }
    }
    return keys.last().v;
}

// Smooth noise from -1 to 1: two octaves of values at whole steps, eased
// between, the same for the same seed every time.
double smoothNoise(double x, quint32 seed) {
    auto at = [seed](qint64 i) {
        quint32 h = quint32(i) * 374761393u + seed * 668265263u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return ((h ^ (h >> 16)) & 0xffff) / 32767.5 - 1;
    };
    auto octave = [&](double y) {
        const double whole = std::floor(y), f = y - whole;
        const double s = f * f * (3 - 2 * f);
        return at(qint64(whole)) * (1 - s) + at(qint64(whole) + 1) * s;
    };
    return (octave(x) * 2 + octave(x * 2.3 + 17)) / 3;
}
}

double Scene::ownValue(const Layer &layer, const QString &property, double t) const {
    // Curve modifiers, as Blender's: cycles repeat the keys, noise wobbles.
    const QJsonArray modifiers = layer.raw.isEmpty() || !layer.raw.contains("modifiers")
                                     ? QJsonArray()
                                     : layer.raw.value("modifiers").toObject().value(property).toArray();
    const auto it = layer.keys.constFind(property);
    double v;
    if (it == layer.keys.constEnd() || it->isEmpty()) {
        // The camera looks at the canvas centre until it's told otherwise.
        if (&layer == &camera && (property == "x" || property == "y"))
            v = layer.base.value(property, property == "x" ? width / 2.0 : height / 2.0);
        else
            v = layer.base.value(property, defaultValue(property));
    } else {
        double at = t;
        for (const QJsonValue &m : modifiers) {
            const double first = it->first().t, span = it->last().t - first;
            if (m.toObject().value("type").toString() == "cycle" && span > 0 && (at > it->last().t || at < first))
                at = first + std::fmod(std::fmod(at - first, span) + span, span);
        }
        v = interpolate(*it, at, property);
    }
    for (const QJsonValue &m : modifiers) {
        const QJsonObject modifier = m.toObject();
        if (modifier.value("type").toString() == "noise")
            v += modifier.value("amount").toDouble(1) *
                 smoothNoise(t * modifier.value("speed").toDouble(2), quint32(qHash(property) + modifier.value("seed").toInt(1) * 7919u));
    }
    // Clips: saved motions laid on the layer, each adding how far its own
    // keys have moved from where they start, from its start, repeated.
    if (!layer.raw.isEmpty() && layer.raw.contains("clips")) {
        const QJsonObject motions = raw.value("motions").toObject();
        for (const QJsonValue &c : layer.raw.value("clips").toArray()) {
            const QJsonObject clip = c.toObject();
            const QJsonArray list = motions.value(clip.value("motion").toString()).toObject().value("keys").toObject().value(property).toArray();
            if (list.isEmpty())
                continue;
            QList<Key> keys;
            for (const QJsonValue &k : list)
                keys << Key{k.toObject().value("t").toDouble(), k.toObject().value("v").toDouble(), k.toObject().value("ease").toString("linear")};
            const double length = keys.last().t - keys.first().t;
            const int repeat = std::max(1, clip.value("repeat").toInt(1));
            double local = (t - clip.value("start").toDouble()) * clip.value("speed").toDouble(1);
            if (local < 0)
                continue;
            if (length > 0 && local < length * repeat)
                local = std::fmod(local, length);
            else
                local = length;
            v += clip.value("amount").toDouble(1) * (interpolate(keys, keys.first().t + local, property) - keys.first().v);
        }
    }
    return v;
}

double Scene::linkValue(const Layer &layer, const QString &property, double t) const {
    const auto it = layer.links.constFind(property);
    if (it == layer.links.constEnd())
        return 0;
    const Link &link = *it;
    int index = -1;
    QString source;
    if (!linkSource(link.from, &index, &source))
        return 0;
    // Loops are refused when links are made; this only keeps one that got
    // through some other way from recursing until the stack runs out.
    thread_local int depth = 0;
    if (depth > 32)
        return 0;
    ++depth;
    auto read = [&](double when) { return index == -1 ? when : value(*at(index), source, when); };
    const double h = link.smooth > 0 ? link.smooth : 1 / fps;
    // What the link gives at a moment, before any spring or limits.
    auto drive = [&](double moment) {
        const double when = moment - link.lag;
        double measure = 0;
        if (link.of != "value") {
            // Measured inside the scene, so a car already moving at the first
            // frame isn't taken to be half as fast there, or to have jolted.
            double lo = when - h, hi = when + h;
            if (lo < 0)
                hi -= lo, lo = 0;
            if (hi > duration)
                lo -= hi - duration, hi = duration;
            lo = std::max(0.0, lo);
            const double mid = (lo + hi) / 2, half = (hi - lo) / 2;
            if (half <= 0)
                measure = 0;
            else if (link.of == "speed")
                measure = (read(hi) - read(lo)) / (hi - lo);
            else
                measure = (read(hi) - 2 * read(mid) + read(lo)) / (half * half);
        } else {
            measure = read(when);
        }
        return (link.abs ? std::abs(measure) : measure) * link.times;
    };
    double v = 0;
    if (link.spring > 0) {
        // A weight on a spring chasing what the link gives. It starts at rest
        // where it's pulled, a little before t: long enough before that how
        // it started has rung out, so frames agree without simulating the
        // whole scene each time.
        const double w = 2 * M_PI * link.spring, zeta = std::clamp(link.damping, .05, 4.0);
        const double window = std::min({t, std::log(1000.0) / (zeta * w), 20.0});
        const int steps = std::max(1, int(std::ceil(window / std::min(.25 / fps, .1 / w))));
        const double dt = window / steps;
        double moment = t - window, x = drive(moment), speed = 0;
        for (int i = 0; i < steps; ++i) {
            moment += dt;
            speed += (w * w * (drive(moment) - x) - 2 * zeta * w * speed) * dt;
            x += speed * dt;
        }
        v = x;
    } else {
        v = drive(t);
    }
    --depth;
    v = std::clamp(v, link.min, link.max);
    return std::isfinite(v) ? v : 0;
}

const QStringList &Scene::linkMeasures() {
    static const QStringList list{"value", "speed", "acceleration"};
    return list;
}

bool Scene::linkSource(const QString &from, int *index, QString *property) const {
    if (from == "time") {
        *index = -1;
        property->clear();
        return true;
    }
    // Split at the last dot: layer names may have dots, properties don't.
    const int dot = from.lastIndexOf('.');
    if (dot <= 0)
        return false;
    const QString name = from.left(dot), wanted = from.mid(dot + 1);
    const int i = name == "camera" ? CameraIndex : indexOf(name);
    if (!at(i) || !(i == CameraIndex ? cameraProperties() : propertiesOf(*at(i))).contains(wanted))
        return false;
    *index = i;
    *property = wanted;
    return true;
}

QString Scene::checkLinks() const {
    auto who = [this](int index, const QString &property) {
        return index == CameraIndex ? QString("The camera's %1").arg(property)
                                    : QString("Layer \"%1\"'s %2").arg(layers.at(index).name, property);
    };
    QList<int> all{CameraIndex};
    for (int i = 0; i < layers.size(); ++i)
        all.append(i);
    for (int index : all)
        for (auto it = at(index)->links.begin(); it != at(index)->links.end(); ++it) {
            int source = -1;
            QString property;
            if (!linkSource(it->from, &source, &property))
                return QString("%1 is linked to \"%2\", which isn't there; link to layer.property, "
                               "camera.property or time.")
                    .arg(who(index, it.key()), it->from);
        }
    // Follow each link to its source, and the source's link to its own, and
    // so on: arriving back where it started is a loop with no answer.
    for (int start : all)
        for (auto it = at(start)->links.begin(); it != at(start)->links.end(); ++it) {
            int index = start;
            QString property = it.key();
            for (int steps = 0; steps <= 64; ++steps) {
                const auto link = at(index)->links.constFind(property);
                if (link == at(index)->links.constEnd())
                    break;
                int next = -1;
                QString nextProperty;
                linkSource(link->from, &next, &nextProperty);
                if (next == -1)
                    break;
                if (next == start && nextProperty == it.key())
                    return QString("%1 is linked in a loop that comes back to itself.").arg(who(start, it.key()));
                index = next;
                property = nextProperty;
            }
        }
    return {};
}

bool Scene::setLink(int index, const QString &property, const Link &link, QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    Layer *layer = at(index);
    if (!layer)
        return fail("There's no such layer.");
    if (!(index == CameraIndex ? cameraProperties() : propertiesOf(*layer)).contains(property))
        return fail(QString("\"%1\" isn't a property that can be linked.").arg(property));
    if (link.from.isEmpty()) {
        layer->links.remove(property);
        return true;
    }
    int source = -1;
    QString sourceProperty;
    if (!linkSource(link.from, &source, &sourceProperty))
        return fail(QString("There's nothing called \"%1\" to link to; use layer.property, camera.property or time.")
                        .arg(link.from));
    const bool had = layer->links.contains(property);
    const Link old = layer->links.value(property);
    layer->links.insert(property, link);
    const QString problem = checkLinks();
    if (problem.isEmpty())
        return true;
    if (had)
        layer->links.insert(property, old);
    else
        layer->links.remove(property);
    return fail(problem);
}

void Scene::renameLinkSource(const QString &from, const QString &to) {
    auto rename = [&](Layer &layer) {
        for (Link &link : layer.links)
            if (link.from.startsWith(from + ".") && link.from.lastIndexOf('.') == from.size())
                link.from = to + link.from.mid(from.size());
    };
    rename(camera);
    for (Layer &layer : layers)
        rename(layer);
}

int Scene::dropLinksTo(const QString &name) {
    int dropped = 0;
    auto drop = [&](Layer &layer) {
        for (auto it = layer.links.begin(); it != layer.links.end();) {
            const bool gone = it->from.left(it->from.lastIndexOf('.')) == name;
            dropped += gone;
            it = gone ? layer.links.erase(it) : std::next(it);
        }
    };
    drop(camera);
    for (Layer &layer : layers)
        drop(layer);
    return dropped;
}

QString Scene::linkText(const Link &link) {
    QString text = (link.of == "value" ? QString() : link.of + " of ") + link.from;
    if (link.times != 1)
        text += " × " + formatNumber(link.times);
    if (link.lag != 0)
        text += ", lag " + formatNumber(link.lag);
    if (link.smooth != 0)
        text += ", smooth " + formatNumber(link.smooth);
    if (link.spring != 0)
        text += ", spring " + formatNumber(link.spring);
    if (link.spring != 0 && link.damping != .5)
        text += ", damping " + formatNumber(link.damping);
    if (link.abs)
        text += ", abs";
    if (std::isfinite(link.min))
        text += ", min " + formatNumber(link.min);
    if (std::isfinite(link.max))
        text += ", max " + formatNumber(link.max);
    return text;
}

bool Scene::parseLink(const QString &text, Link *link, QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    static const QRegularExpression measured("^(value|speed|acceleration)\\s+of\\s+(.+)$");
    static const QRegularExpression scaled("^(.+?)\\s*[*×]\\s*([-+]?[\\d.]+(?:[eE][-+]?\\d+)?)$");
    Link parsed;
    const QStringList parts = text.split(',');
    QString main = parts.first().trimmed();
    if (const auto match = measured.match(main); match.hasMatch()) {
        parsed.of = match.captured(1);
        main = match.captured(2).trimmed();
    }
    if (const auto match = scaled.match(main); match.hasMatch()) {
        bool ok = false;
        parsed.times = match.captured(2).toDouble(&ok);
        if (!ok)
            return fail(QString("\"%1\" isn't a number to multiply by.").arg(match.captured(2)));
        main = match.captured(1).trimmed();
    }
    parsed.from = main;
    if (parsed.from.isEmpty())
        return fail("Say what to link to, like car.x × 0.8, speed of car.x, or time × 90.");
    for (int i = 1; i < parts.size(); ++i) {
        const QStringList words = parts.at(i).trimmed().split(' ', Qt::SkipEmptyParts);
        if (words.size() == 1 && words.first() == "abs") {
            parsed.abs = true;
            continue;
        }
        bool ok = false;
        const double v = words.size() == 2 ? words.at(1).toDouble(&ok) : 0;
        const QString name = words.value(0);
        if (!ok || !QStringList{"lag", "smooth", "spring", "damping", "min", "max"}.contains(name))
            return fail(QString("\"%1\" isn't something a link takes: lag, smooth, spring, damping, min or max "
                                "with a number, or abs.")
                            .arg(parts.at(i).trimmed()));
        if (name == "lag")
            parsed.lag = v;
        else if (name == "smooth")
            parsed.smooth = v;
        else if (name == "spring")
            parsed.spring = v;
        else if (name == "damping")
            parsed.damping = v;
        else if (name == "min")
            parsed.min = v;
        else
            parsed.max = v;
    }
    if (!std::isfinite(parsed.times) || !std::isfinite(parsed.lag) || parsed.smooth < 0 || parsed.spring < 0 ||
        parsed.damping <= 0 || parsed.min > parsed.max)
        return fail("Smooth and spring can't be below 0, damping must be over 0, and min can't be over max.");
    *link = parsed;
    return true;
}

Link Scene::linkFromJson(const QJsonObject &json) {
    Link link;
    link.raw = json;
    link.from = json.value("from").toString();
    link.of = json.value("of").toString("value");
    link.times = json.value("times").toDouble(1);
    link.lag = json.value("lag").toDouble(0);
    link.smooth = json.value("smooth").toDouble(0);
    link.abs = json.value("abs").toBool(false);
    link.spring = json.value("spring").toDouble(0);
    link.damping = json.value("damping").toDouble(.5);
    if (json.contains("min"))
        link.min = json.value("min").toDouble(std::numeric_limits<double>::quiet_NaN());
    if (json.contains("max"))
        link.max = json.value("max").toDouble(std::numeric_limits<double>::quiet_NaN());
    return link;
}

QJsonObject Scene::linkToJson(const Link &link) {
    // Only what differs from the defaults, so a link stays one short line.
    QJsonObject json = link.raw;
    json.insert("from", link.from);
    auto put = [&json](const char *name, double v, bool shown) {
        if (shown)
            json.insert(name, v);
        else
            json.remove(name);
    };
    if (link.of == "value")
        json.remove("of");
    else
        json.insert("of", link.of);
    put("times", link.times, link.times != 1);
    put("lag", link.lag, link.lag != 0);
    put("smooth", link.smooth, link.smooth != 0);
    put("spring", link.spring, link.spring != 0);
    put("damping", link.damping, link.spring != 0 && link.damping != .5);
    if (link.abs)
        json.insert("abs", true);
    else
        json.remove("abs");
    put("min", link.min, std::isfinite(link.min));
    put("max", link.max, std::isfinite(link.max));
    return json;
}

void Scene::setValue(int index, const QString &property, double t, double v) {
    if (!at(index))
        return;
    Layer &layer = (*at(index));
    // What's set is what's seen: the link's share comes off before it's stored.
    v -= linkValue(layer, property, t);
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
    if (!at(index) || !(index == CameraIndex ? cameraProperties() : propertiesOf(*at(index))).contains(property))
        return;
    const double v = ownValue((*at(index)), property, t);
    Layer &layer = (*at(index));
    if (layer.keys.value(property).isEmpty())
        layer.keys.insert(property, {Key{t, v, "inOut"}});
    else
        setValue(index, property, t, v);
}

void Scene::keyAll(int index, double t) {
    if (!at(index))
        return;
    if (index == CameraIndex) {
        for (const QString &property : cameraProperties())
            keyProperty(index, property, t);
        return;
    }
    for (const QString &property : propertiesOf(*at(index))) {
        const Layer &layer = (*at(index));
        const bool used = layer.keys.contains(property) ||
                          layer.base.value(property, defaultValue(property)) != defaultValue(property);
        if (!optionalProperties().contains(property) || used)
            keyProperty(index, property, t);
    }
}

bool Scene::removeKeysAt(int index, double t) {
    if (!at(index))
        return false;
    Layer &layer = (*at(index));
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
    if (!at(index))
        return {};
    const double tolerance = .5 / fps;
    QString next;
    for (auto &keys : (*at(index)).keys) {
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
    if (!at(index) || !validEase(ease))
        return false;
    const double tolerance = .5 / fps;
    bool found = false;
    for (auto &keys : (*at(index)).keys)
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
        if (!at(ref.layer))
            continue;
        for (auto it = (*at(ref.layer)).keys.begin(); it != (*at(ref.layer)).keys.end(); ++it) {
            QList<Key> &keys = *it;
            for (int i = keys.size() - 1; i >= 0; --i)
                if (std::abs(keys.at(i).t - ref.t) < tolerance)
                    lifted.append({ref.layer, it.key(), keys.takeAt(i)});
        }
    }
    QList<KeyRef> moved;
    for (Lifted &item : lifted) {
        item.key.t = snap(item.key.t + delta);
        placeKey((*at(item.layer)).keys[item.property], item.key, tolerance);
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
        if (!at(ref.layer))
            continue;
        const Layer &layer = (*at(ref.layer));
        for (auto it = layer.keys.begin(); it != layer.keys.end(); ++it)
            for (const Key &key : *it)
                if (std::abs(key.t - ref.t) < tolerance)
                    clip.append({it.key(), Key{key.t - first, key.v, key.ease}});
    }
    return clip;
}

QList<KeyRef> Scene::pasteKeys(int index, double t, const QList<KeyClip> &clip) {
    QList<KeyRef> pasted;
    if (!at(index))
        return pasted;
    // Only what the target can animate: a camera's zoom is a layer's scale
    // and back, and the rest that doesn't fit (another layer's pins, say) is
    // left out rather than written where it would break the scene.
    const QStringList valid = index == CameraIndex ? cameraProperties() : propertiesOf(*at(index));
    for (KeyClip item : clip) {
        if (index == CameraIndex && item.property == "scale")
            item.property = "zoom";
        else if (index != CameraIndex && item.property == "zoom")
            item.property = "scale";
        if (!valid.contains(item.property))
            continue;
        Key key = item.key;
        key.t = snap(t + key.t);
        placeKey((*at(index)).keys[item.property], key, .5 / fps);
        const KeyRef ref{index, key.t};
        if (!pasted.contains(ref))
            pasted.append(ref);
    }
    return pasted;
}

QList<double> Scene::keyTimes(int index) const {
    QList<double> times;
    if (!at(index))
        return times;
    for (const auto &keys : (*at(index)).keys)
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

AssetCache::Entry *AssetCache::entry(const QString &source) {
    if (source.isEmpty())
        return nullptr;  // A layer without a picture, like particles.
    const QString path = QDir(m_directory).absoluteFilePath(source);
    const QFileInfo info(path);
    auto missing = [&]() -> Entry * {
        if (!m_missing.contains(source))
            m_missing.append(source);
        m_entries.remove(source);
        return nullptr;
    };
    if (!info.exists())
        return missing();
    auto it = m_entries.find(source);
    if (it != m_entries.end() && it->modified == info.lastModified())
        return &*it;
    // Only the header to begin with: the size is enough to place a layer,
    // and the pixels may only ever be wanted smaller.
    const QSize size = QImageReader(path).size();
    if (!size.isValid())
        return missing();
    m_missing.removeAll(source);
    return &*m_entries.insert(source, Entry{QImage(), size, info.lastModified(), {}});
}

QImage AssetCache::image(const QString &source) {
    Entry *e = entry(source);
    if (!e)
        return {};
    if (e->image.isNull()) {
        QImage image(QDir(m_directory).absoluteFilePath(source));
        if (image.isNull()) {
            if (!m_missing.contains(source))
                m_missing.append(source);
            m_entries.remove(source);
            return {};
        }
        e->image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        e->size = e->image.size();
    }
    return e->image;
}

QSizeF AssetCache::size(const QString &source) {
    const Entry *e = entry(source);
    return e ? QSizeF(e->size) : QSizeF();
}

QImage AssetCache::mip(const QString &source, double scale, double *factor) {
    *factor = 1;
    Entry *e = entry(source);
    if (!e)
        return {};
    // How many halvings fit: down while half is still at least scale. Halved
    // as the copies are made (QSize rounds), so a request for a copy's own
    // factor, as look() makes, finds that copy and not the one above it.
    int levels = 0;
    for (QSize next = e->size; double((next / 2).width()) / e->size.width() >= scale && next.width() > 16 &&
                               next.height() > 16;
         next /= 2)
        ++levels;
    if (levels == 0)
        return image(source);
    if (e->mips.size() < levels) {
        QImage current = e->mips.isEmpty() ? image(source) : e->mips.last();
        e = entry(source);  // image() may have reloaded it.
        if (!e || current.isNull())
            return {};
        while (e->mips.size() < levels) {
            current = current.scaled(current.size() / 2, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            e->mips.append(current);
        }
        // Drawn small, the full picture isn't needed; it's read again if
        // anyone zooms in that far.
        if (!m_keepFull)
            e->image = QImage();
    }
    const QImage &chosen = e->mips.at(levels - 1);
    *factor = double(chosen.width()) / e->size.width();
    return chosen;
}

void AssetCache::clear() {
    m_entries.clear();
    m_looks.clear();
    m_lookOrder.clear();
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

int drawingFrameAt(const Layer &layer, double t) {
    const QJsonArray frames = layer.raw.value("drawing").toObject().value("frames").toArray();
    int shown = -1;
    double latest = -1e300;
    for (int i = 0; i < frames.size(); ++i) {
        const double at = frames.at(i).toObject().value("t").toDouble();
        if (at <= t + 1e-6 && at > latest)
            latest = at, shown = i;
    }
    return shown;
}

void drawStrokes(QPainter &painter, const Layer &layer, double t) {
    const int shown = drawingFrameAt(layer, t);
    if (shown < 0)
        return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(Qt::NoBrush);
    const QJsonArray strokes =
        layer.raw.value("drawing").toObject().value("frames").toArray().at(shown).toObject().value("strokes").toArray();
    for (const QJsonValue &value : strokes) {
        const QJsonObject stroke = value.toObject();
        const QJsonArray points = stroke.value("points").toArray();
        QPen pen(QColor(stroke.value("color").toString("#ffffff")), stroke.value("width").toDouble(4), Qt::SolidLine, Qt::RoundCap,
                 Qt::RoundJoin);
        painter.setPen(pen);
        if (points.size() == 1) {
            const QJsonArray p = points.first().toArray();
            painter.drawPoint(QPointF(p.at(0).toDouble(), p.at(1).toDouble()));
            continue;
        }
        // Smoothed through the middles of its points, as a hand draws.
        QPainterPath path;
        auto at = [&](int i) {
            const QJsonArray p = points.at(i).toArray();
            return QPointF(p.at(0).toDouble(), p.at(1).toDouble());
        };
        path.moveTo(at(0));
        for (int i = 1; i + 1 < points.size(); ++i)
            path.quadTo(at(i), (at(i) + at(i + 1)) / 2);
        path.lineTo(at(int(points.size()) - 1));
        painter.drawPath(path);
    }
    painter.restore();
}

void applyLook(QImage &frame, const QJsonObject &look) {
    if (look.isEmpty() || frame.isNull())
        return;
    const double exposure = std::pow(2.0, look.value("exposure").toDouble(0));
    const double contrast = look.value("contrast").toDouble(1), saturation = look.value("saturation").toDouble(1);
    const double temperature = std::clamp(look.value("temperature").toDouble(0), -1.0, 1.0);
    const double tint = std::clamp(look.value("tint").toDouble(0), -1.0, 1.0);
    const double vignette = std::clamp(look.value("vignette").toDouble(0), 0.0, 1.0);
    const double bloom = std::clamp(look.value("bloom").toDouble(0), 0.0, 1.0);
    const double threshold = std::clamp(look.value("bloomThreshold").toDouble(.75), 0.0, .99);
    if (frame.format() != QImage::Format_ARGB32_Premultiplied)
        frame = frame.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    // Bloom first, from the frame as it is: its bright parts, small and
    // softened, laid back over it as light (screen).
    QImage glow;
    if (bloom > 0) {
        const QSize small = (frame.size() / 4).expandedTo(QSize(1, 1));
        glow = frame.scaled(small, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        for (int y = 0; y < glow.height(); ++y) {
            QRgb *line = reinterpret_cast<QRgb *>(glow.scanLine(y));
            for (int x = 0; x < glow.width(); ++x) {
                const QRgb p = line[x];
                const double lum = (.2126 * qRed(p) + .7152 * qGreen(p) + .0722 * qBlue(p)) / 255;
                const double k = std::clamp((lum - threshold) / (1 - threshold), 0.0, 1.0);
                line[x] = qRgba(int(qRed(p) * k), int(qGreen(p) * k), int(qBlue(p) * k), int(qAlpha(p) * k));
            }
        }
        soften(glow, std::max(2.0, small.width() * .04));
        glow = glow.scaled(frame.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }

    // Each channel's grade as a table: exposure, warmth and tint, contrast.
    const double gains[3] = {1 + .12 * temperature - .04 * tint, 1 + .08 * tint, 1 - .12 * temperature - .04 * tint};
    uchar table[3][256];
    for (int c = 0; c < 3; ++c)
        for (int v = 0; v < 256; ++v) {
            double x = v / 255.0 * exposure * gains[c];
            x = (x - .5) * contrast + .5;
            table[c][v] = uchar(std::clamp(int(std::lround(x * 255)), 0, 255));
        }
    const double cx = frame.width() / 2.0, cy = frame.height() / 2.0, reach = std::hypot(cx, cy);
    for (int y = 0; y < frame.height(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(frame.scanLine(y));
        const QRgb *light = glow.isNull() ? nullptr : reinterpret_cast<const QRgb *>(glow.constScanLine(y));
        for (int x = 0; x < frame.width(); ++x) {
            const QRgb p = line[x];
            const int a = qAlpha(p);
            if (a == 0 && !light)
                continue;
            // Graded as colours, not as premultiplied ones.
            auto un = [a](int v) { return a ? std::min(255, v * 255 / a) : 0; };
            double r = table[0][un(qRed(p))], g = table[1][un(qGreen(p))], b = table[2][un(qBlue(p))];
            if (saturation != 1) {
                const double lum = .2126 * r + .7152 * g + .0722 * b;
                r = lum + (r - lum) * saturation, g = lum + (g - lum) * saturation, b = lum + (b - lum) * saturation;
            }
            if (vignette > 0) {
                const double d = std::hypot(x - cx, y - cy) / reach;
                const double s = std::clamp((d - .45) / .7, 0.0, 1.0);
                const double dark = 1 - vignette * s * s * (3 - 2 * s);
                r *= dark, g *= dark, b *= dark;
            }
            int na = a;
            if (light) {
                const QRgb l = light[x];
                const double k = bloom * 1.5;
                r = 255 - (255 - r) * (1 - std::min(1.0, qRed(l) * k / 255));
                g = 255 - (255 - g) * (1 - std::min(1.0, qGreen(l) * k / 255));
                b = 255 - (255 - b) * (1 - std::min(1.0, qBlue(l) * k / 255));
                na = std::max(a, std::min(255, int(qAlpha(l) * k)));
            }
            auto pre = [na](double v) { return std::clamp(int(std::lround(v * na / 255)), 0, na); };
            line[x] = qRgba(pre(r), pre(g), pre(b), na);
        }
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
    look.factor = f;
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

QTransform cameraTransform(const Scene &scene, double follow, double t) {
    const Layer &camera = scene.camera;
    const QPointF centre(scene.width / 2.0, scene.height / 2.0);
    // A layer that follows the camera partly sees a camera moved, zoomed and
    // turned partly: far things drift less than near ones.
    const QPointF pan = (QPointF(scene.value(camera, "x", t), scene.value(camera, "y", t)) - centre) * follow;
    const double zoom = 1 + (scene.value(camera, "zoom", t) - 1) * follow;
    QTransform transform;
    transform.translate(centre.x(), centre.y());
    transform.rotate(-scene.value(camera, "rotation", t) * follow);
    transform.scale(zoom, zoom);
    transform.translate(-(centre.x() + pan.x()), -(centre.y() + pan.y()));
    return transform;
}

double followOf(const Scene &scene, int index) {
    QSet<int> seen;
    while (index >= 0 && index < scene.layers.size() && !seen.contains(index)) {
        seen.insert(index);
        const Layer &layer = scene.layers.at(index);
        if (!std::isnan(layer.follow))
            return layer.follow;
        index = layer.parent.isEmpty() ? -1 : scene.indexOf(layer.parent);
    }
    return 1;
}

double depthOf(const Scene &scene, int index, double t) {
    double depth = 0;
    QSet<int> seen;
    while (index >= 0 && index < scene.layers.size() && !seen.contains(index)) {
        seen.insert(index);
        const Layer &layer = scene.layers.at(index);
        depth += scene.value(layer, "z", t);
        index = layer.parent.isEmpty() ? -1 : scene.indexOf(layer.parent);
    }
    return depth;
}

QTransform perspectiveTransform(const Scene &scene, double depth) {
    if (depth == 0)
        return {};
    // A card facing the camera, depth further off than where it's placed:
    // everything on it closes in on the vanishing point by the same factor.
    // Held short of the camera, where it would grow without end.
    const double d = scene.focalDistance();
    const double f = d / std::max(d + depth, d / 50);
    const QPointF v = scene.vanishPoint();
    return QTransform(f, 0, 0, f, v.x() * (1 - f), v.y() * (1 - f));
}

QTransform layerTransform(const Scene &scene, AssetCache &assets, int index, double t) {
    return chainTransform(scene, assets, index, t) * perspectiveTransform(scene, depthOf(scene, index, t)) *
           cameraTransform(scene, followOf(scene, index), t);
}

QTransform parentTransform(const Scene &scene, AssetCache &assets, int index, double t) {
    if (index < 0 || index >= scene.layers.size())
        return {};
    const int parent = scene.indexOf(scene.layers.at(index).parent);
    // Its own depth too: x and y move it in a world that far back.
    return (parent >= 0 ? chainTransform(scene, assets, parent, t) : QTransform()) *
           perspectiveTransform(scene, depthOf(scene, index, t)) * cameraTransform(scene, followOf(scene, index), t);
}

QTransform chainTransform(const Scene &scene, AssetCache &assets, int index, double t) {
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

bool isBent(const Scene &scene, const Layer &layer, double t) {
    for (int i = 0; i < layer.pins.size(); ++i)
        if (scene.value(layer, QString("pin%1x").arg(i), t) != 0 || scene.value(layer, QString("pin%1y").arg(i), t) != 0)
            return true;
    return false;
}

PinSet pinsAt(const Scene &scene, const Layer &layer, double t) {
    PinSet pins;
    for (int i = 0; i < layer.pins.size(); ++i) {
        pins.p.append(layer.pins.at(i));
        pins.q.append(layer.pins.at(i) + QPointF(scene.value(layer, QString("pin%1x").arg(i), t),
                                                 scene.value(layer, QString("pin%1y").arg(i), t)));
    }
    return pins;
}

QPointF bendWith(const PinSet &pins, const QPointF &v) {
    const QList<QPointF> &p = pins.p, &q = pins.q;
    const int n = int(p.size());
    if (n == 0)
        return v;
    if (n == 1)
        return v + (q[0] - p[0]);  // One pin just carries the image along.
    // Moving least squares (Schaefer, McPhail and Warren): each point moves
    // as rigidly as it can to follow the pins, the nearer ones most.
    QVarLengthArray<double, 16> w(n);
    double total = 0;
    QPointF pStar, qStar;
    for (int i = 0; i < n; ++i) {
        const QPointF d = p[i] - v;
        const double distance = QPointF::dotProduct(d, d);
        if (distance < 1e-9)
            return q[i];
        w[i] = 1 / distance;
        total += w[i];
        pStar += w[i] * p[i];
        qStar += w[i] * q[i];
    }
    pStar /= total;
    qStar /= total;
    const QPointF d = v - pStar;
    QPointF bent(0, 0);
    for (int i = 0; i < n; ++i) {
        const QPointF a = p[i] - pStar, b = q[i] - qStar;
        // b · A_i, with A_i = w_i [a; -a⊥] [d; -d⊥]ᵀ and x⊥ = (-y, x).
        const double m11 = a.x() * d.x() + a.y() * d.y(), m12 = a.x() * d.y() - a.y() * d.x();
        const double m21 = a.y() * d.x() - a.x() * d.y(), m22 = a.y() * d.y() + a.x() * d.x();
        bent += w[i] * QPointF(b.x() * m11 + b.y() * m21, b.x() * m12 + b.y() * m22);
    }
    const double length = std::hypot(bent.x(), bent.y());
    if (length < 1e-9)
        return v + (qStar - pStar);
    return qStar + bent * (std::hypot(d.x(), d.y()) / length);
}

QPointF bend(const Scene &scene, const Layer &layer, const QPointF &v, double t) {
    return bendWith(pinsAt(scene, layer, t), v);
}

QPointF unbend(const Scene &scene, const Layer &layer, const QPointF &target, double t) {
    // Where on the image ends up at target: walked towards, a step at a time,
    // by how far its bent place is off. Bends are smooth, so it gets there.
    const PinSet pins = pinsAt(scene, layer, t);
    QPointF guess = target;
    for (int i = 0; i < 24; ++i) {
        const QPointF off = target - bendWith(pins, guess);
        guess += off;
        if (QPointF::dotProduct(off, off) < 1e-6)
            break;
    }
    return guess;
}

namespace {

// Draws a pinned layer's image (and its glow) bent: a mesh of small
// triangles, each mapped from where it was in the image to where the pins
// take it, into a scratch image first, then laid on once with the layer's
// opacity and blend, so where triangles meet nothing is drawn twice.
void drawBent(QPainter &painter, const Scene &scene, const Layer &layer, double t, const QImage &image, const QImage &glow,
              double factor, int margin, const QSizeF &size, const QTransform &toOutput, double opacity,
              QPainter::CompositionMode blend) {
    const PinSet pins = pinsAt(scene, layer, t);
    // About a triangle side every 40 output pixels, over the padding too, so
    // a blur's or glow's halo bends with the picture.
    const double drawn = std::max(size.width(), size.height()) *
                         std::max(std::hypot(toOutput.m11(), toOutput.m12()), std::hypot(toOutput.m21(), toOutput.m22()));
    const int cells = std::clamp(int(drawn / 40), 6, 32);
    const double pad = margin / factor;
    const QRectF spread(-pad, -pad, size.width() + 2 * pad, size.height() + 2 * pad);
    const double cw = spread.width() / cells, ch = spread.height() / cells;
    QList<QPointF> from((cells + 1) * (cells + 1)), to((cells + 1) * (cells + 1));
    QRectF reach;
    for (int y = 0; y <= cells; ++y)
        for (int x = 0; x <= cells; ++x) {
            const int i = y * (cells + 1) + x;
            from[i] = spread.topLeft() + QPointF(x * cw, y * ch);
            to[i] = bendWith(pins, from[i]);
            const QPointF out = toOutput.map(to[i]);
            reach |= QRectF(out, QSizeF(1, 1));
        }
    const QRect area = reach.toAlignedRect().adjusted(-2, -2, 2, 2) &
                       QRect(0, 0, painter.device()->width(), painter.device()->height());
    if (area.isEmpty())
        return;
    auto copyAt = [&](const QPointF &p) { return p * factor + QPointF(margin, margin); };

    auto mesh = [&](const QImage &picture) {
        QImage scratch(area.size(), QImage::Format_ARGB32_Premultiplied);
        scratch.fill(Qt::transparent);
        QPainter into(&scratch);
        into.setRenderHint(QPainter::SmoothPixmapTransform);
        // Each triangle replaces what's under it: where the slightly bigger
        // triangles overlap, see-through pixels (an edge, a glow) aren't
        // laid twice.
        into.setCompositionMode(QPainter::CompositionMode_Source);
        const QTransform shift = QTransform::fromTranslate(-area.x(), -area.y());
        for (int y = 0; y < cells; ++y)
            for (int x = 0; x < cells; ++x) {
                const int i = y * (cells + 1) + x;
                const int corners[4] = {i, i + 1, i + cells + 2, i + cells + 1};
                for (const auto &tri : {std::array<int, 3>{0, 1, 2}, std::array<int, 3>{0, 2, 3}}) {
                    QPolygonF source, target;
                    for (int k : tri) {
                        source << copyAt(from[corners[k]]);
                        target << to[corners[k]];
                    }
                    // Three points fix the map from the image to where it's bent to.
                    const QPointF u = source[1] - source[0], v = source[2] - source[0];
                    const QPointF U = target[1] - target[0], V = target[2] - target[0];
                    const double det = u.x() * v.y() - u.y() * v.x();
                    if (std::abs(det) < 1e-12)
                        continue;
                    const QTransform linear((U.x() * v.y() - V.x() * u.y()) / det, (U.y() * v.y() - V.y() * u.y()) / det,
                                            (V.x() * u.x() - U.x() * v.x()) / det, (V.y() * u.x() - U.y() * v.x()) / det, 0, 0);
                    const QTransform map = QTransform::fromTranslate(-source[0].x(), -source[0].y()) * linear *
                                           QTransform::fromTranslate(target[0].x(), target[0].y());
                    // A hair bigger than the triangle, so neighbours meet.
                    const QPointF middle = (source[0] + source[1] + source[2]) / 3;
                    QPolygonF clip;
                    for (const QPointF &corner : source)
                        clip << corner + (corner - middle) * (1.2 / std::max(1.0, QLineF(corner, middle).length()));
                    into.setTransform(map * toOutput * shift);
                    QPainterPath path;
                    path.addPolygon(clip);
                    into.setClipPath(path);
                    const QRectF bounds = clip.boundingRect().adjusted(-1, -1, 1, 1) & QRectF(picture.rect());
                    into.drawImage(bounds, picture, bounds);
                }
            }
        return scratch;
    };

    painter.save();
    painter.setTransform(QTransform());
    painter.setOpacity(opacity);
    painter.setCompositionMode(blend);
    painter.drawImage(area.topLeft(), mesh(image));
    if (!glow.isNull()) {
        painter.setCompositionMode(QPainter::CompositionMode_Screen);
        painter.drawImage(area.topLeft(), mesh(glow));
    }
    painter.restore();
}

}

namespace {
// One particle as its kind looks: a streak along its way for rain, a soft
// glowing dot for dust, embers and fireflies, a tumbling leaf, a dot.
void drawOne(QPainter &painter, const QString &kind, const QPointF &at, double r, const QColor &c, const QPointF &way,
             double streak, double turn, double squash) {
    if (kind == "rain") {
        const double length = std::hypot(way.x(), way.y());
        const QPointF along = length > 0 ? way / length : QPointF(0, 1);
        painter.setPen(QPen(c, r * .45, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(at, at - along * streak);
        painter.setPen(Qt::NoPen);
    } else if (kind == "fireflies" || kind == "embers" || kind == "dust") {
        const double reach = r * (kind == "dust" ? 1.5 : 3.5);
        QRadialGradient light(at, reach);
        light.setColorAt(0, c);
        QColor edge = c;
        edge.setAlpha(0);
        light.setColorAt(kind == "dust" ? 1 : .35, kind == "dust" ? edge : c);
        light.setColorAt(1, edge);
        painter.setBrush(light);
        painter.drawEllipse(at, reach, reach);
    } else if (kind == "leaves") {
        painter.save();
        painter.translate(at);
        painter.rotate(turn);
        painter.scale(1, squash);
        painter.setBrush(c);
        painter.drawEllipse(QPointF(0, 0), r, r * .5);
        painter.restore();
    } else {
        painter.setBrush(c);
        painter.drawEllipse(at, r, r);
    }
}

// An emitter's particles, each from its birth on the source layer, worked
// out from the time alone so any frame can be drawn by itself.
void drawEmitted(QPainter &painter, const Scene &scene, const QJsonObject &settings, double t, AssetCache &assets,
                 const QTransform &canvasToOutput) {
    const int source = scene.indexOf(settings.value("from").toString());
    if (source < 0)
        return;
    const Layer &from = scene.layers.at(source);
    const QString kind = settings.value("kind").toString();
    const double rate = std::max(0.0, settings.value("rate").toDouble(40));
    const double life = std::max(.05, settings.value("life").toDouble(1.5));
    if (rate <= 0)
        return;
    const QSizeF size = assets.size(from.source);
    const QJsonArray at = settings.value("at").toArray();
    const QPointF point = at.size() == 2 ? QPointF(at.at(0).toDouble(), at.at(1).toDouble())
                                         : QPointF(from.anchor.x() * size.width(), from.anchor.y() * size.height());
    const double speed = settings.value("speed").toDouble(), r0 = std::max(.2, settings.value("size").toDouble());
    const double direction = qDegreesToRadians(settings.value("direction").toDouble());
    const double spread = qDegreesToRadians(settings.value("spread").toDouble());
    const double inherit = settings.value("inherit").toDouble(.5);
    const double gravity = settings.value("gravity").toDouble(kind == "rain" ? 900 : kind == "embers" ? -120 : kind == "dust" ? -20 : 80);
    const double wind = settings.value("wind").toDouble(0), drag = std::max(0.0, settings.value("drag").toDouble(1));
    const double twinkle = std::clamp(settings.value("twinkle").toDouble(), 0.0, 1.0);
    const QColor colour(settings.value("color").toString("#ffffff"));
    const quint32 seed = quint32(settings.value("seed").toInt(1));
    const double step = 1 / std::max(1.0, scene.fps);
    auto emitter = [&](double when) { return layerTransform(scene, assets, source, when).map(point); };
    const qint64 first = std::max<qint64>(0, qint64(std::floor((t - life) * rate))), last = qint64(std::floor(t * rate));
    painter.save();
    painter.setTransform(canvasToOutput, true);
    painter.setPen(Qt::NoPen);
    for (qint64 i = first; i <= last && i - first < 20000; ++i) {
        const double born = i / rate, age = t - born;
        if (age < 0 || age > life)
            continue;
        QRandomGenerator random(seed * 2654435761u + quint32(i) * 40503u);
        const double pace = .6 + .8 * random.generateDouble();
        const double angle = direction + spread * (random.generateDouble() * 2 - 1);
        const double grow = .5 + random.generateDouble(), flicker = random.generateDouble() * 2 * M_PI, turn = random.generateDouble();
        const QPointF start = emitter(born);
        const QPointF carried = (emitter(born + step) - emitter(std::max(0.0, born - step))) / (born >= step ? 2 * step : step);
        const QPointF velocity = QPointF(std::cos(angle), std::sin(angle)) * speed * pace + carried * inherit;
        // Slowed by the air, and pulled down and along.
        const double slowed = drag > 0 ? (1 - std::exp(-drag * age)) / drag : age;
        const QPointF place = start + velocity * slowed + QPointF(wind, gravity) * (.5 * age * age);
        const QPointF way = velocity * std::exp(-drag * age) + QPointF(wind, gravity) * age;
        const double grown = age / life;
        QColor c = colour;
        const double glow = 1 - twinkle * (.5 + .5 * std::sin(6 * age + flicker));
        c.setAlphaF(std::clamp(c.alphaF() * glow * std::min(1.0, age / .08) * std::pow(1 - grown, 1.2), 0.0, 1.0));
        const double r = r0 * grow * (kind == "dust" ? 1 + 2.5 * grown : 1);
        drawOne(painter, kind, place, r, c, way, 8 + std::hypot(way.x(), way.y()) * .03, 360 * (turn + age * .7), .55 + .45 * std::cos(4 * age + flicker));
    }
    painter.restore();
}
}

void drawParticles(QPainter &painter, const Scene &scene, const Layer &layer, double t, double follow, AssetCache *assets,
                   const QTransform &toOutput, const QTransform &canvasToOutput) {
    Q_UNUSED(toOutput)
    QJsonObject settings = Scene::particleDefaults(layer.particles.value("kind").toString());
    for (auto it = layer.particles.begin(); it != layer.particles.end(); ++it)
        settings.insert(it.key(), it.value());
    if (!settings.value("from").toString().isEmpty()) {
        if (assets) {
            // On the canvas, wherever the particle layer itself is.
            painter.save();
            painter.setTransform(QTransform());
            drawEmitted(painter, scene, settings, t, *assets, canvasToOutput);
            painter.restore();
        }
        return;
    }
    const QString kind = settings.value("kind").toString();
    const int count = std::clamp(settings.value("count").toInt(), 0, 5000);
    const double size = std::max(.2, settings.value("size").toDouble());
    const double speed = settings.value("speed").toDouble();
    const double direction = qDegreesToRadians(settings.value("direction").toDouble());
    const double spread = qDegreesToRadians(settings.value("spread").toDouble());
    const double sway = settings.value("sway").toDouble();
    const double twinkle = std::clamp(settings.value("twinkle").toDouble(), 0.0, 1.0);
    const QColor color(settings.value("color").toString("#ffffff"));
    const double d = std::max(.01, scene.duration);
    // They live in a box around everything the camera shows over the whole
    // scene, so panning never runs out of them, and a little more, so they
    // come and go off screen.
    const double margin = std::max({60.0, size * 6, std::abs(sway) + 20});
    QRectF seen;
    for (int i = 0; i <= 32; ++i) {
        bool invertible = false;
        const QTransform toWorld = cameraTransform(scene, follow, d * i / 32).inverted(&invertible);
        if (invertible)
            seen |= toWorld.mapRect(QRectF(0, 0, scene.width, scene.height));
    }
    if (seen.isEmpty())
        seen = QRectF(0, 0, scene.width, scene.height);
    const QRectF box = seen.adjusted(-margin, -margin, margin, margin);
    auto wrap = [](double v, double length) {
        const double r = std::fmod(v, length);
        return r < 0 ? r + length : r;
    };
    // The same particles every frame: a fixed sequence of random numbers.
    QRandomGenerator random(quint32(settings.value("seed").toInt(1)));
    painter.save();
    painter.setPen(Qt::NoPen);
    for (int i = 0; i < count; ++i) {
        const double across = random.generateDouble(), along0 = random.generateDouble();
        const double pace = .6 + .8 * random.generateDouble();
        const double angle = direction + spread * (random.generateDouble() * 2 - 1);
        const double phase = random.generateDouble() * 2 * M_PI, flicker = random.generateDouble() * 2 * M_PI;
        const double grow = .5 + random.generateDouble(), turn = random.generateDouble();
        // Each one runs along its own track across the box at its own speed.
        // The track's length is fitted so the scene holds a whole number of
        // runs, which is what makes it loop: a slow one that can't cross in
        // time gets a short track and fades in and out at its ends.
        const QPointF ahead(std::cos(angle), std::sin(angle)), side(-ahead.y(), ahead.x());
        const double reach = std::abs(ahead.x()) * box.width() + std::abs(ahead.y()) * box.height();
        const double breadth = std::abs(side.x()) * box.width() + std::abs(side.y()) * box.height();
        const double v = std::abs(speed) * pace;
        const double runs = std::floor(v * d / reach);
        const double track = runs >= 1 ? v * d / runs : std::max(v * d, 1e-6);
        const double travelled = wrap(along0 * track + v * t * (speed < 0 ? -1 : 1), track);
        QPointF at = box.center() + ahead * (travelled - track / 2) + side * ((across - .5) * breadth);
        double fade = 1;
        if (runs < 1 && v > 0) {
            const double edge = std::min(travelled, track - travelled) / (track * .15);
            fade = std::clamp(edge, 0.0, 1.0);
        }
        const int swings = std::max(1, int(std::round(d / (2.5 + 3 * turn))));
        double x = at.x(), y = at.y();
        x += sway * grow * std::sin(2 * M_PI * swings * t / d + phase);
        if (kind == "fireflies")
            y += sway * .6 * grow * std::cos(2 * M_PI * swings * t / d + flicker);
        const double glow = 1 - twinkle * (.5 + .5 * std::sin(2 * M_PI * std::max(1, swings * 2) * t / d + flicker));
        const double r = size * grow;
        QColor c = color;
        c.setAlphaF(std::clamp(c.alphaF() * glow * fade, 0.0, 1.0));

        if (kind == "rain") {
            // Streaks along their fall, longer the faster.
            const QPointF along(std::cos(angle), std::sin(angle));
            painter.setPen(QPen(c, r * .45, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(QPointF(x, y), QPointF(x, y) - along * (8 + std::abs(speed) * pace * .03) * grow);
            painter.setPen(Qt::NoPen);
        } else if (kind == "fireflies" || kind == "embers" || kind == "dust") {
            // A soft dot that glows out.
            QRadialGradient light(QPointF(x, y), r * (kind == "dust" ? 1.5 : 3.5));
            light.setColorAt(0, c);
            QColor edge = c;
            edge.setAlpha(0);
            light.setColorAt(kind == "dust" ? 1 : .35, kind == "dust" ? edge : c);
            light.setColorAt(1, edge);
            painter.setBrush(light);
            painter.drawEllipse(QPointF(x, y), r * (kind == "dust" ? 1.5 : 3.5), r * (kind == "dust" ? 1.5 : 3.5));
        } else if (kind == "leaves") {
            // Little leaves, tumbling a whole number of turns.
            painter.save();
            painter.translate(x, y);
            const int turns = std::max(1, int(std::round(d / (3 + 4 * turn))));
            painter.rotate(360.0 * (turn + turns * t / d));
            painter.scale(1, .55 + .45 * std::cos(2 * M_PI * turns * t / d + phase));
            QColor leaf = c;
            leaf = leaf.lighter(80 + int(40 * turn));
            painter.setBrush(leaf);
            painter.drawEllipse(QPointF(0, 0), r, r * .5);
            painter.restore();
        } else {
            painter.setBrush(c);
            painter.drawEllipse(QPointF(x, y), r, r);
        }
    }
    painter.restore();
}

namespace {
WorldDrawer worldDrawer;
}

void setWorldRenderer(WorldDrawer drawer) { worldDrawer = std::move(drawer); }

namespace {
// Depth of field on a flat scene: out of focus, a layer blurs by how far it
// is set back from the focus (its z), aperture pixels for each hundred.
double defocus(const Scene &scene, int index, double t) {
    const QJsonObject look = scene.raw.value("look").toObject();
    const double aperture = look.value("aperture").toDouble(0);
    if (aperture <= 0 || World::info(scene).valid)
        return 0;
    return aperture * std::abs(depthOf(scene, index, t) - look.value("focus").toDouble(0)) / 100;
}
}

void renderScene(QPainter &painter, const Scene &scene, AssetCache &assets, double t, bool overWorld, int first,
                 int end) {
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setRenderHint(QPainter::Antialiasing);
    const QTransform base = painter.transform();
    // Output pixels per canvas pixel: the view's zoom, or the export's size.
    const double outputScale = std::sqrt(std::abs(base.determinant()));
    if (!overWorld && first <= 0)
        painter.fillRect(QRectF(0, 0, scene.width, scene.height), scene.background);
    painter.setClipRect(QRectF(0, 0, scene.width, scene.height));
    const QList<int> inWorld = overWorld ? World::layers(scene) : QList<int>();
    const int last = end < 0 ? int(scene.layers.size()) : std::min(end, int(scene.layers.size()));
    for (int i = std::max(0, first); i < last; ++i) {
        if (inWorld.contains(i))
            continue;
        const Layer &layer = scene.layers.at(i);
        const double opacity = clamp01(scene.value(layer, "opacity", t));
        if (!layer.visible || opacity <= 0)
            continue;
        if (!layer.particles.isEmpty()) {
            // Particles live in the world, seen through the camera; their
            // look (blur, glow) is worked on the frame they make.
            const QTransform toOutput = layerTransform(scene, assets, i, t) * base;
            const double blur = std::max(0.0, scene.value(layer, "blur", t)) * outputScale;
            const double glowSize = std::max(0.0, scene.value(layer, "glow", t)) * outputScale;
            painter.setOpacity(opacity);
            if (blur <= 0 && glowSize <= 0) {
                painter.setTransform(toOutput);
                painter.setCompositionMode(compositionFor(layer.blend));
                drawParticles(painter, scene, layer, t, followOf(scene, i), &assets, toOutput, base);
                continue;
            }
            const QRect device(0, 0, painter.device()->width(), painter.device()->height());
            QImage frame(device.size(), QImage::Format_ARGB32_Premultiplied);
            frame.fill(Qt::transparent);
            {
                QPainter into(&frame);
                into.setRenderHint(QPainter::Antialiasing);
                into.setTransform(toOutput);
                drawParticles(into, scene, layer, t, followOf(scene, i), &assets, toOutput, base);
            }
            soften(frame, blur);
            painter.setTransform(QTransform());
            painter.setCompositionMode(compositionFor(layer.blend));
            painter.drawImage(QPointF(0, 0), frame);
            if (glowSize > 0) {
                soften(frame, glowSize);
                painter.setCompositionMode(QPainter::CompositionMode_Screen);
                painter.drawImage(QPointF(0, 0), frame);
            }
            continue;
        }
        // A drawing: its strokes, where the layer is.
        if (layer.raw.contains("drawing")) {
            painter.setTransform(layerTransform(scene, assets, i, t) * base);
            painter.setOpacity(opacity);
            painter.setCompositionMode(compositionFor(layer.blend));
            drawStrokes(painter, layer, t);
            continue;
        }
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
                     saturation = scene.value(layer, "saturation", t), blur = std::max(0.0, scene.value(layer, "blur", t)) + defocus(scene, i, t),
                     glowSize = std::max(0.0, scene.value(layer, "glow", t));
        if (brightness != 0 || contrast != 1 || saturation != 1 || blur > 0 || glowSize > 0) {
            const double canvasPerPixel = scale / factor / std::max(outputScale, 1e-6);
            const AssetCache::Look &look =
                assets.look(layer.source, factor, brightness, contrast, saturation, blur, glowSize, canvasPerPixel);
            image = look.image;
            glow = look.glow;
            margin = look.margin;
            factor = look.factor;
        }

        painter.setOpacity(opacity);
        painter.setCompositionMode(compositionFor(layer.blend));
        if (!layer.pins.isEmpty() && isBent(scene, layer, t)) {
            drawBent(painter, scene, layer, t, image, glow, factor, margin, assets.size(layer.source), toOutput, opacity,
                     compositionFor(layer.blend));
            continue;
        }
        // The copy is drawn back up to the image's full size, less its padding.
        const QTransform fromCopy(1 / factor, 0, 0, 1 / factor, -margin / factor, -margin / factor);
        painter.setTransform(fromCopy * toOutput);
        painter.drawImage(QPointF(0, 0), image);
        if (!glow.isNull()) {
            painter.setCompositionMode(QPainter::CompositionMode_Screen);
            painter.drawImage(QPointF(0, 0), glow);
        }
    }
    painter.restore();
}

namespace {
// Sums of a shutter's moments, a channel at a time, and their average: in
// integers, as 16 moments of 255 fit in 16 bits.
struct Exposure {
    QSize size;
    std::vector<quint16> sum;
    int moments = 0;
    explicit Exposure(const QSize &size) : size(size), sum(size_t(size.width()) * size.height() * 4, 0) {}
    void add(const QImage &moment) {
        for (int y = 0; y < size.height(); ++y) {
            const uchar *line = moment.constScanLine(y);
            quint16 *into = sum.data() + size_t(y) * size.width() * 4;
            for (int x = 0; x < size.width() * 4; ++x)
                into[x] += line[x];
        }
        ++moments;
    }
    QImage average() const {
        QImage image(size, QImage::Format_ARGB32_Premultiplied);
        const int n = std::max(1, moments);
        for (int y = 0; y < size.height(); ++y) {
            uchar *line = image.scanLine(y);
            const quint16 *from = sum.data() + size_t(y) * size.width() * 4;
            // Rounded to the nearest, halves up.
            for (int x = 0; x < size.width() * 4; ++x)
                line[x] = uchar((2 * from[x] + n) / (2 * n));
        }
        return image;
    }
};

// What a layer looks like at a moment, as numbers: the same at two moments
// means it's drawn the same. Particles always move.
QList<double> layerState(const Scene &scene, AssetCache &assets, int index, double t) {
    const Layer &layer = scene.layers.at(index);
    if (!layer.visible)
        return {};
    if (!layer.particles.isEmpty())
        return {t};
    const QTransform m = layerTransform(scene, assets, index, t);
    QList<double> state{m.m11(), m.m12(), m.m13(), m.m21(), m.m22(), m.m23(), m.m31(), m.m32(), m.m33()};
    for (const QString &property : Scene::propertiesOf(layer))
        state << scene.value(layer, property, t);
    return state;
}

// The bottom layer that changes across these moments; -1 if none does.
int firstMoving(const Scene &scene, AssetCache &assets, const QList<double> &times) {
    for (int i = 0; i < scene.layers.size(); ++i) {
        const QList<double> start = layerState(scene, assets, i, times.first());
        for (int k = 1; k < times.size(); ++k)
            if (layerState(scene, assets, i, times.at(k)) != start)
                return i;
    }
    return -1;
}

// Where layers from first up land on an output this size across the
// moments; the whole of it for what's hard to bound (particles, bent,
// blurred or glowing layers).
QRect coverage(const Scene &scene, AssetCache &assets, int first, const QList<double> &times, const QSize &size) {
    const QRect all(QPoint(), size);
    const QTransform toOutput = QTransform::fromScale(double(size.width()) / scene.width, double(size.height()) / scene.height);
    QRectF area;
    for (int i = first; i < scene.layers.size(); ++i) {
        const Layer &layer = scene.layers.at(i);
        if (!layer.visible)
            continue;
        for (const double t : times) {
            if (scene.value(layer, "opacity", t) <= 0)
                continue;
            if (!layer.particles.isEmpty() || (!layer.pins.isEmpty() && isBent(scene, layer, t)) ||
                scene.value(layer, "blur", t) > 0 || scene.value(layer, "glow", t) > 0)
                return all;
            area |= (layerTransform(scene, assets, i, t) * toOutput).mapRect(QRectF(QPointF(), assets.size(layer.source)));
        }
    }
    // A pixel more each side for smoothing at the edges.
    return area.toAlignedRect().adjusted(-2, -2, 2, 2) & all;
}
}

QImage renderFrame(const Scene &scene, AssetCache &assets, double t, const QSize &size) {
    const QSize target = size.isValid() ? size : QSize(scene.width, scene.height);
    QImage frame(target, QImage::Format_ARGB32_Premultiplied);
    frame.fill(Qt::transparent);
    QPainter painter(&frame);
    // A world is drawn in 3D first, the other layers over it.
    QImage world;
    if (worldDrawer && World::info(scene).valid) {
        // A camera's shutter is open for part of each frame, so what moves
        // fast smears along its way: several moments across the shutter,
        // averaged. 180° (half a frame) unless the world says otherwise.
        const double shutter = scene.raw.value("world").toObject().value("shutter").toDouble(180) / 360 / scene.fps;
        // As many as it takes for what moves most to leave no gaps wider
        // than a pixel and a half, up to 16: one when nothing moves.
        const double from = std::max(0.0, t - shutter / 2), to = std::max(0.0, t + shutter / 2);
        const int samples =
            shutter > 0 ? std::clamp(int(std::ceil(World::motion(scene, assets, from, to, target) / 1.5)) + 1, 1, 16) : 1;
        std::optional<Exposure> exposure;
        for (int i = 0; i < samples; ++i) {
            const double at = samples == 1 ? t : std::max(0.0, t + shutter * (double(i) / (samples - 1) - .5));
            const QImage moment = worldDrawer(scene, assets, at, target);
            if (moment.isNull())
                break;
            if (samples == 1) {
                world = moment;
                break;
            }
            if (!exposure)
                exposure.emplace(moment.size());
            exposure->add(moment.convertToFormat(QImage::Format_ARGB32_Premultiplied));
            if (i == samples - 1)
                world = exposure->average();
        }
        static bool warned = false;
        if (world.isNull() && !warned) {
            warned = true;
            qWarning("omotion: can't draw the world in 3D here (it needs a GPU and a desktop session); drawing it flat.");
        }
    }
    if (!world.isNull()) {
        painter.fillRect(frame.rect(), scene.background);
        painter.drawImage(0, 0, world);
    }
    painter.scale(double(target.width()) / scene.width, double(target.height()) / scene.height);
    // A flat scene can ask for a shutter too ("shutter" in degrees): moments
    // across it are drawn and averaged, so what moves fast smears.
    const double flatShutter = world.isNull() ? scene.raw.value("shutter").toDouble(0) / 360 / scene.fps : 0;
    if (flatShutter > 0) {
        constexpr int samples = 12;
        QList<double> times;
        for (int i = 0; i < samples; ++i)
            times << std::max(0.0, t + flatShutter * (double(i) / (samples - 1) - .5));
        // Layers that hold still while the shutter is open come out the same
        // in every moment: they're drawn once, and only from the first one
        // that moves up is drawn for each moment.
        const int first = firstMoving(scene, assets, times);
        if (first < 0) {
            renderScene(painter, scene, assets, times.first());
        } else {
            renderScene(painter, scene, assets, times.first(), false, 0, first);
            // From the bottom layer up, each moment has the background too,
            // so it's drawn whole.
            bool plain = first > 0;
            for (int i = first; i < scene.layers.size(); ++i)
                plain = plain && compositionFor(scene.layers.at(i).blend) == QPainter::CompositionMode_SourceOver;
            // Laid over what's below as they are, the moving layers can be
            // averaged on their own and laid over it once: over is linear
            // in what's on top. Just where they reach is drawn.
            const QRect area = plain ? coverage(scene, assets, first, times, target) : frame.rect();
            Exposure exposure(area.size());
            for (const double at : times) {
                if (area.isEmpty())
                    break;
                const bool fresh = plain || first == 0;
                QImage moment = fresh ? QImage(area.size(), QImage::Format_ARGB32_Premultiplied) : frame.copy();
                if (fresh)
                    moment.fill(Qt::transparent);
                QPainter into(&moment);
                into.translate(-area.topLeft());
                into.scale(double(target.width()) / scene.width, double(target.height()) / scene.height);
                renderScene(into, scene, assets, at, false, first);
                into.end();
                exposure.add(moment);
            }
            if (!area.isEmpty()) {
                painter.save();
                painter.resetTransform();
                if (!plain)
                    painter.setCompositionMode(QPainter::CompositionMode_Source);
                painter.drawImage(area.topLeft(), exposure.average());
                painter.restore();
            }
        }
    } else {
        renderScene(painter, scene, assets, t, !world.isNull());
    }
    painter.end();
    // The scene's look over everything.
    applyLook(frame, scene.raw.value("look").toObject());
    // Film grain over a world's frame, the painting and what moves in it
    // alike, so the two share one surface. Seeded by the frame's time, so a
    // frame always comes out the same.
    if (!world.isNull()) {
        const double grain = scene.raw.value("world").toObject().value("grain").toDouble(.025);
        if (grain > 0) {
            quint32 seed = quint32(std::lround(t * 1000)) * 2654435761u + 1;
            const int amount = int(std::lround(grain * 255));
            for (int y = 0; y < frame.height(); ++y) {
                QRgb *line = reinterpret_cast<QRgb *>(frame.scanLine(y));
                for (int x = 0; x < frame.width(); ++x) {
                    seed = seed * 1664525u + 1013904223u;
                    const int n = int((seed >> 24) & 0xff) - 128;
                    const int d = n * amount / 128, a = qAlpha(line[x]);
                    // Premultiplied: the grain is scaled by how solid the pixel is.
                    const int e = d * a / 255;
                    line[x] = qRgba(std::clamp(qRed(line[x]) + e, 0, a), std::clamp(qGreen(line[x]) + e, 0, a),
                                    std::clamp(qBlue(line[x]) + e, 0, a), a);
                }
            }
        }
    }
    return frame;
}

int layerAt(const Scene &scene, AssetCache &assets, double t, const QPointF &point, const QList<int> &skip) {
    for (int i = scene.layers.size() - 1; i >= 0; --i) {
        const Layer &layer = scene.layers.at(i);
        if (skip.contains(i) || !layer.visible || scene.value(layer, "opacity", t) <= 0)
            continue;
        // A quarter-size copy is plenty to tell what's under the mouse.
        double factor = 1;
        const QImage image = assets.mip(layer.source, .25, &factor);
        bool invertible = false;
        const QTransform inverse = layerTransform(scene, assets, i, t).inverted(&invertible);
        if (image.isNull() || !invertible)
            continue;
        QPointF unbent = inverse.map(point);
        if (!layer.pins.isEmpty() && isBent(scene, layer, t))
            unbent = unbend(scene, layer, unbent, t);
        const QPoint local = (unbent * factor).toPoint();
        if (image.rect().contains(local) && qAlpha(image.pixel(local)) > 16)
            return i;
    }
    return -1;
}
