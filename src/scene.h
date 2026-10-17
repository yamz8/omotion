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
#include <functional>
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

// A property driven by another, so a scene can say how things relate instead
// of keyframing each one: a wheel turns as its car travels, dust thickens with
// speed, a body leans as it speeds up. The source's value (or how fast it
// changes) times a factor is added to the property's own value.
struct Link {
    QString from;             // "layer.property", "camera.property" or "time".
    QString of = "value";     // value, speed (a second) or acceleration.
    double times = 1;
    double lag = 0;           // Seconds behind the source.
    double smooth = 0;        // Seconds speed is measured over; 0 is a frame.
    bool abs = false;         // Ignore the direction: backwards counts too.
    // Follow on a spring this many times a second (0: follow exactly), so
    // what's driven lags, overshoots and settles like a weight on a spring.
    double spring = 0;
    double damping = .5;      // 1 settles without overshoot, near 0 rings.
    double min = -std::numeric_limits<double>::infinity();
    double max = std::numeric_limits<double>::infinity();
    QJsonObject raw;          // As read, so unknown fields survive a save.
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
    QMap<QString, Link> links;  // At most one per property.
    QJsonObject raw;  // Everything as read, so unknown fields survive a save.
};

struct Scene {
    int width = 1280;
    int height = 720;
    double fps = 30;
    double duration = 4;
    QColor background = QColor("#000000");
    // Perspective for layers set back with z: they shrink toward the
    // vanishing point, by distance / (distance + z), as a camera sees things
    // further off. NaN until set: then the canvas centre, and its width.
    QPointF vanish{std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN()};
    double distance = std::numeric_limits<double>::quiet_NaN();
    QPointF vanishPoint() const;
    double focalDistance() const;
    QList<Layer> layers;
    // The camera: what part of the world the canvas shows. x and y are the
    // world point at the canvas centre, zoom and rotation turn the view; all
    // keyframed like a layer's. Its index, where an index is wanted, is
    // CameraIndex.
    Layer camera;
    QJsonObject raw;
    static constexpr int CameraIndex = -2;
    static const QStringList &cameraProperties();
    // Moving and turning in a world, in meters and degrees: worldX right,
    // worldY up, worldZ away from the camera; yaw right, pitch up, roll
    // clockwise. The camera takes all but roll (its rotation is that).
    static const QStringList &worldProperties();
    // The kinds of particles, and each one's settings to start from.
    static const QStringList &particleKinds();
    static QJsonObject particleDefaults(const QString &kind);
    // A layer by index, or the camera for CameraIndex; null when neither.
    Layer *at(int index);
    const Layer *at(int index) const;

    static const QStringList &properties();
    // The properties that change how a layer looks rather than where it is.
    static const QStringList &lookProperties();
    // Properties left out of scene.json at their default, and keyed by K only
    // once they're used: the looks, and z.
    static const QStringList &optionalProperties();
    static const QStringList &eases();
    static const QStringList &blends();
    static double defaultValue(const QString &property);
    // Whether a property names a puppet pin's offset that a layer has.
    static bool isPinProperty(const Layer &layer, const QString &property);
    // Every property a layer can animate: the usual ones, and its pins'.
    static QStringList propertiesOf(const Layer &layer);
    static double ease(const QString &name, double progress);
    // Physical easings with an amount: spring(n) overshoots and settles in n
    // wobbles, bounce(n) lands and bounces n times, arc(h) is a jump that
    // rises h pixels at the middle (on y; other properties go straight).
    static bool physical(const QString &ease, QString *kind, double *amount);
    // How far an arc lifts y at progress p, in pixels; 0 for other easings.
    static double arcLift(const QString &ease, double progress);
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

    // A property's value on a layer at a time: its own value (the keyframes
    // when it has any, otherwise its constant value) plus what a link adds.
    // A constraint (see constrained) takes the own value's place.
    double value(const Layer &layer, const QString &property, double t) const;
    // Constraints, in a layer's "constraints" (as Blender's): what they make
    // a property at time t, when one sets it. "ik" on an upper bone turns it
    // and its lower bone (a child) so the lower's tip reaches a target layer;
    // "lookAt" turns a layer toward a target; "path" moves it along points
    // over a time (and turns it with the way, with turn). Targets and paths
    // are in the layer's parent's pixels: a target shares the parent.
    bool constrained(const Layer &layer, const QString &property, double t, double *v) const;
    // The keyframes or constant alone, without the link.
    double ownValue(const Layer &layer, const QString &property, double t) const;
    // What a property's link adds at time t; 0 when it has none.
    double linkValue(const Layer &layer, const QString &property, double t) const;

    static const QStringList &linkMeasures();
    // Where a link's "from" points: a layer index (or CameraIndex) and its
    // property. False when it names nothing; "time" gives index -1.
    bool linkSource(const QString &from, int *index, QString *property) const;
    // Why the scene's links can't work (a missing source, a loop), or empty.
    QString checkLinks() const;
    // Links a layer's property, or unlinks it with an empty from. Refused,
    // with the reason, when it would loop or names nothing.
    bool setLink(int index, const QString &property, const Link &link, QString *error = nullptr);
    // Points links at a renamed layer.
    void renameLinkSource(const QString &from, const QString &to);
    // Drops the links reading a deleted layer; returns how many went.
    int dropLinksTo(const QString &name);
    // A link as one line, "speed of car.x × 0.05, lag 0.1", and back: the
    // inspector edits links this way.
    static QString linkText(const Link &link);
    static bool parseLink(const QString &text, Link *link, QString *error = nullptr);
    static Link linkFromJson(const QJsonObject &json);
    static QJsonObject linkToJson(const Link &link);

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
        // The size of the copy it was made from, relative to the full image:
        // what it's drawn back up by.
        double factor = 1;
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
    QStringList m_frameOrder;
    QHash<QString, Look> m_looks;
    QStringList m_lookOrder;
    QStringList m_missing;
};

// Maps the world to the canvas for a layer that follows the camera this
// much (1 fully, 0 not at all).
QTransform cameraTransform(const Scene &scene, double follow, double t);
// How far a layer is set back at time t: its z and its parents'.
double depthOf(const Scene &scene, int index, double t);
// Shrinks the world toward the vanishing point for things that far back.
QTransform perspectiveTransform(const Scene &scene, double depth);
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
// Draws the scene's layers over its background. Over a world (drawn first,
// by renderFrame or the window), the world's own layers and the background
// are left out. first and end draw only those layers (end -1 for the top);
// the background goes under the bottom one.
void renderScene(QPainter &painter, const Scene &scene, AssetCache &assets, double t, bool overWorld = false,
                 int first = 0, int end = -1);
// What draws a scene's world for renderFrame: set by the program, which
// links the 3D side; without it, or when it can't draw here, worlds are
// drawn flat.
using WorldDrawer = std::function<QImage(const Scene &, AssetCache &, double, const QSize &)>;
void setWorldRenderer(WorldDrawer drawer);
// Where a point of a pinned layer's image goes, bent by its pins at time t
// (moving least squares, as rigid as can be); the point itself if unpinned.
QPointF bend(const Scene &scene, const Layer &layer, const QPointF &point, double t);
// The point of the image that the bend takes to target.
QPointF unbend(const Scene &scene, const Layer &layer, const QPointF &target, double t);
// A layer's pins at time t, where they started (p) and where they are (q).
struct PinSet {
    QList<QPointF> p, q;
};
PinSet pinsAt(const Scene &scene, const Layer &layer, double t);
QPointF bendWith(const PinSet &pins, const QPointF &point);
bool isBent(const Scene &scene, const Layer &layer, double t);
// A particle layer's particles at time t, in world pixels. With "from" in
// its settings it's an emitter: they come from a point on that layer (its
// "at", in its pixels; its pivot when left out), "rate" a second, each
// living "life" seconds, starting off with "inherit" of the layer's own
// speed and pulled by "gravity" (down) and "wind" (right), slowed by
// "drag"; drawn on the canvas, so assets and the particle layer's own
// transform to the output (toOutput) are needed for them.
void drawParticles(QPainter &painter, const Scene &scene, const Layer &layer, double t, double follow = 1,
                   AssetCache *assets = nullptr, const QTransform &toOutput = {}, const QTransform &canvasToOutput = {});
QImage renderFrame(const Scene &scene, AssetCache &assets, double t, const QSize &size = {});
// The picture a layer shows at time t: a video layer's frame for then (its
// "video": frames in a folder of the project, from "start" seconds into
// the scene at "speed", looping or holding its last), else its source.
QString sourceAt(const Layer &layer, double t);
// A drawing layer's strokes (its "drawing" in scene.json), as Blender's
// Grease Pencil: frames of strokes, each shown from its time until the
// next; each stroke a colour, a width and points in the layer's pixels.
void drawStrokes(QPainter &painter, const Layer &layer, double t);
// The frame of a drawing shown at time t: its index, or -1.
int drawingFrameAt(const Layer &layer, double t);
// The scene's look (its "look" in scene.json), over a finished frame:
// exposure (stops), contrast, saturation, temperature and tint (-1 to 1,
// warmer and greener), a vignette (0 to 1), and bloom: the brightest parts
// (over bloomThreshold, 0 to 1) glowing out by bloom (0 to 1). Nothing to
// do without one.
void applyLook(QImage &frame, const QJsonObject &look);
// Softens an Alpha8 or 32-bit image in place, spreading edges about width
// pixels: three box blurs, which come close to a Gaussian.
void soften(QImage &image, double width);
// The top-most visible layer with an opaque pixel at a canvas point, or -1;
// layers in skip don't count.
int layerAt(const Scene &scene, AssetCache &assets, double t, const QPointF &point, const QList<int> &skip = {});
