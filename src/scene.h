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
#include <limits>

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
    QString blend = "normal";  // How it mixes with the layers below.
    // How much the camera moves it: 1 fully, 0 not at all (stuck to the
    // screen), between for depth. NaN when not set: then its parent's.
    double follow = std::numeric_limits<double>::quiet_NaN();
    // A particle layer has no image: rain, snow and the like, drawn from
    // these settings (see Scene::particleKinds). Empty for picture layers.
    QJsonObject particles;
    // Puppet pins: points on the image (in its pixels) that bend it. Pin n's
    // offset is the properties "pin<n>x" and "pin<n>y", keyframed like any.
    QList<QPointF> pins;
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
    // The camera: what part of the world the canvas shows. x and y are the
    // world point at the canvas centre, zoom and rotation turn the view; all
    // keyframed like a layer's. Its index, where an index is wanted, is
    // CameraIndex.
    Layer camera;
    QJsonObject raw;
    static constexpr int CameraIndex = -2;
    static const QStringList &cameraProperties();
    // The kinds of particles, and each one's settings to start from.
    static const QStringList &particleKinds();
    static QJsonObject particleDefaults(const QString &kind);
    // A layer by index, or the camera for CameraIndex; null when neither.
    Layer *at(int index);
    const Layer *at(int index) const;

    static const QStringList &properties();
    // The properties that change how a layer looks rather than where it is.
    static const QStringList &lookProperties();
    static const QStringList &eases();
    static const QStringList &blends();
    static double defaultValue(const QString &property);
    // Whether a property names a puppet pin's offset that a layer has.
    static bool isPinProperty(const Layer &layer, const QString &property);
    // Every property a layer can animate: the usual ones, and its pins'.
    static QStringList propertiesOf(const Layer &layer);
    static double ease(const QString &name, double progress);
    // Custom easing: "cubic(x1,y1,x2,y2)", a curve from (0,0) to (1,1) with
    // those two handles, as in CSS. y can go past 0..1, to overshoot.
    static bool cubic(const QString &ease, double *x1, double *y1, double *x2, double *y2);
    static QString cubicEase(double x1, double y1, double x2, double y2);
    static bool validEase(const QString &ease);
    // An easing's curve handles, named ones included, for showing and editing.
    static QList<double> curveOf(const QString &ease);

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
    // Keyframes every transform property at time t, and the look properties
    // the layer uses.
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
    // Whether to keep full pictures once their smaller copies are made. A
    // window that only shows them small can let them go and save memory.
    void setKeepFull(bool keep) { m_keepFull = keep; }
    void clear();
    // The image shrunk by halves to the smallest copy still at least scale of
    // full size, so drawing it smaller is quick and doesn't shimmer. factor is
    // that copy's size relative to the full image.
    QImage mip(const QString &source, double scale, double *factor);

    // A layer image with its look applied, cached for the next frame.
    struct Look {
        QImage image;
        QImage glow;
        int margin = 0;  // Padding added around the image, for blur to spread into.
    };
    const Look &look(const QString &source, double factor, double brightness, double contrast,
                     double saturation, double blur, double glow, double canvasPerPixel);
    QStringList missing() const { return m_missing; }
    void clearMissing() { m_missing.clear(); }

private:
    struct Entry {
        QImage image;  // The full picture, read when it's wanted.
        QSize size;
        QDateTime modified;
        QList<QImage> mips;  // Half, quarter, eighth... made as they're needed.
    };
    // The cache entry for a file, current with the file on disk, or null.
    Entry *entry(const QString &source);
    bool m_keepFull = true;

    QString m_directory;
    QHash<QString, Entry> m_entries;
    QHash<QString, Look> m_looks;
    QStringList m_lookOrder;
    QStringList m_missing;
};

// Maps the world to the canvas for a layer that follows the camera this
// much (1 fully, 0 not at all).
QTransform cameraTransform(const Scene &scene, double follow, double t);
// How much a layer follows the camera: its own amount, or its parent's.
double followOf(const Scene &scene, int index);
// Maps a layer's image pixels to canvas pixels at time t, through its parents
// and the camera.
QTransform layerTransform(const Scene &scene, AssetCache &assets, int index, double t);
// The same without the camera: a layer's pixels in the world.
QTransform chainTransform(const Scene &scene, AssetCache &assets, int index, double t);
// Where a layer's x and y live: its parent's pixels (or the world) mapped to
// the canvas, through the camera as this layer sees it.
QTransform parentTransform(const Scene &scene, AssetCache &assets, int index, double t);
void renderScene(QPainter &painter, const Scene &scene, AssetCache &assets, double t);
// Where a point of a pinned layer's image goes, bent by its pins at time t
// (moving least squares, as rigid as can be); the point itself if unpinned.
QPointF bend(const Scene &scene, const Layer &layer, const QPointF &point, double t);
bool isBent(const Scene &scene, const Layer &layer, double t);
// A particle layer's particles at time t, in world pixels.
void drawParticles(QPainter &painter, const Scene &scene, const Layer &layer, double t, double follow = 1);
QImage renderFrame(const Scene &scene, AssetCache &assets, double t, const QSize &size = {});
// Softens an Alpha8 or 32-bit image in place, spreading edges about width
// pixels: three box blurs, which come close to a Gaussian.
void soften(QImage &image, double width);
// The top-most visible layer with an opaque pixel at a canvas point, or -1.
int layerAt(const Scene &scene, AssetCache &assets, double t, const QPointF &point);
