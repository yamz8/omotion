#pragma once

// The animation itself: a canvas, and a stack of image layers whose
// properties are either constant or keyframed over time. It reads and writes
// scene.json, which is meant to be edited by hand (or by an agent) as much as
// by the app, so unknown fields are carried through untouched.

#include <QColor>
#include <QDateTime>
#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QTransform>

class QPainter;

struct Key {
    double t = 0;
    double v = 0;
    QString ease = "linear";
};

// A column of keyframes on one layer: every property's key at that time.
struct KeyRef {
    int layer = -1;
    double t = 0;
    bool operator==(const KeyRef &other) const;
};

// Keyframes lifted off a layer, with times relative to the earliest.
struct KeyClip {
    QString property;
    Key key;
};

struct Layer {
    QString name;
    QString source;   // Relative to the project folder.
    QString parent;   // Name of the layer this one moves with, or empty.
    bool visible = true;
    QPointF anchor{0.5, 0.5};  // Pivot, as a fraction of the image size.
    QHash<QString, double> base;
    QMap<QString, QList<Key>> keys;
    QJsonObject raw;  // Everything as read, so unknown fields survive a save.
};

struct Scene {
    int width = 1280;
    int height = 720;
    double fps = 30;
    double duration = 4;
    QColor background = QColor("#000000");
    QList<Layer> layers;
    QJsonObject raw;

    static const QStringList &properties();
    static const QStringList &eases();
    static double defaultValue(const QString &property);
    static double ease(const QString &name, double progress);

    static Scene fromJson(const QJsonObject &json, QString *error = nullptr);
    QJsonObject toJson() const;

    int indexOf(const QString &name) const;
    QString uniqueName(const QString &wanted) const;
    int frameCount() const;
    double frameTime(int frame) const;

    // A property's value on a layer at a time: the keyframes when it has
    // any, otherwise its constant value.
    double value(const Layer &layer, const QString &property, double t) const;

    // Sets the value at time t: on a keyframe when the property is animated,
    // otherwise as its constant value.
    void setValue(int layer, const QString &property, double t, double value);
    // Sets a keyframe at time t holding the property's current value,
    // starting its animation if it had none.
    void keyProperty(int layer, const QString &property, double t);
    // Keyframes every property at time t.
    void keyAll(int layer, double t);
    // Removes the keyframes within half a frame of t; true when any were removed.
    bool removeKeysAt(int layer, double t);
    // Cycles the easing of the keyframes at t; returns the new easing or empty.
    QString cycleEaseAt(int layer, double t);
    // Sets the easing of the keyframes at t; true when there were any.
    bool setEaseAt(int layer, double t, const QString &ease);
    // Moves keyframe columns by delta seconds together, snapped to frames.
    // Keys they land on are replaced. Returns where the columns ended up.
    QList<KeyRef> shiftKeys(const QList<KeyRef> &refs, double delta);
    QList<KeyClip> copyKeys(const QList<KeyRef> &refs) const;
    // Pastes keyframes onto a layer starting at t, replacing keys they land on.
    QList<KeyRef> pasteKeys(int layer, double t, const QList<KeyClip> &clip);
    double snap(double t) const;
    QList<double> keyTimes(int layer) const;
};

// Loads the layer images on demand and reloads them when they change on disk.
class AssetCache {
public:
    explicit AssetCache(const QString &directory = {}) : m_directory(directory) {}

    void setDirectory(const QString &directory);
    QString directory() const { return m_directory; }
    QImage image(const QString &source);
    QSizeF size(const QString &source);
    QStringList missing() const { return m_missing; }
    void clearMissing() { m_missing.clear(); }

private:
    struct Entry {
        QImage image;
        QDateTime modified;
    };

    QString m_directory;
    QHash<QString, Entry> m_entries;
    QStringList m_missing;
};

// Maps a layer's image pixels to canvas pixels at time t, through its parents.
QTransform layerTransform(const Scene &scene, AssetCache &assets, int index, double t);
void renderScene(QPainter &painter, const Scene &scene, AssetCache &assets, double t);
QImage renderFrame(const Scene &scene, AssetCache &assets, double t, const QSize &size = {});
// The top-most visible layer with an opaque pixel at a canvas point, or -1.
int layerAt(const Scene &scene, AssetCache &assets, double t, const QPointF &point);
