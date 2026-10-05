#include "backend.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLineF>
#include <QUrl>
#include <algorithm>
#include <cmath>

#include "project.h"

namespace {
constexpr int UndoLimit = 200;

QString localPath(const QString &pathOrUrl) {
    const QUrl url(pathOrUrl);
    return url.isLocalFile() ? url.toLocalFile() : pathOrUrl;
}
}

Backend::Backend(FilePicker *picker, QObject *parent) : QObject(parent), m_picker(picker) {
    m_playTimer.setInterval(16);
    connect(&m_playTimer, &QTimer::timeout, this, &Backend::tick);

    // Saves are batched, so a drag writes the file a few times, not per pixel.
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(250);
    connect(&m_saveTimer, &QTimer::timeout, this, &Backend::save);

    m_reloadTimer.setSingleShot(true);
    m_reloadTimer.setInterval(120);
    connect(&m_reloadTimer, &QTimer::timeout, this, &Backend::reloadFromDisk);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_reloadTimer.start(); });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_reloadTimer.start(); });

    m_noticeTimer.setSingleShot(true);
    m_noticeTimer.setInterval(3500);
    connect(&m_noticeTimer, &QTimer::timeout, this, [this] { setNotice({}); });

    connect(&m_exporter, &Exporter::progress, this, [this](double fraction) {
        m_exportProgress = fraction;
        emit exportChanged();
    });
    connect(&m_exporter, &Exporter::finished, this, [this](bool ok, const QString &message) {
        m_exportProgress = 0;
        emit exportChanged();
        if (ok)
            setNotice("Exported " + QDir(m_directory).relativeFilePath(message));
        else if (message == "Export cancelled.")
            setNotice(message);
        else
            setError(message);
    });

    if (m_picker) {
        connect(m_picker, &FilePicker::failed, this, &Backend::setError);
        connect(m_picker, &FilePicker::selected, this, [this](FilePicker::Purpose purpose, const QUrl &url) {
            if (purpose == FilePicker::Purpose::ImportImage)
                importImage(url.toLocalFile());
            else
                open(url.toLocalFile());
        });
    }
}

Backend::~Backend() { flush(); }

QString Backend::projectName() const {
    QString name = QFileInfo(m_directory).fileName();
    if (name.endsWith(".omotion"))
        name.chop(8);
    return name;
}

QVariantMap Backend::sceneInfo() const {
    return {{"width", m_scene.width},
            {"height", m_scene.height},
            {"fps", m_scene.fps},
            {"duration", m_scene.duration},
            {"frames", m_scene.frameCount()},
            {"background", m_scene.background.name(QColor::HexArgb)}};
}

QVariantList Backend::layers() const {
    QVariantList list;
    for (int i = 0; i < m_scene.layers.size(); ++i) {
        const Layer &layer = m_scene.layers.at(i);
        int depth = 0;
        for (QString parent = layer.parent; !parent.isEmpty() && depth < 16; ++depth) {
            const int index = m_scene.indexOf(parent);
            parent = index < 0 ? QString() : m_scene.layers.at(index).parent;
        }
        QVariantList times;
        for (double t : m_scene.keyTimes(i))
            times.append(t);
        list.append(QVariantMap{{"name", layer.name},
                                {"source", layer.source},
                                {"parent", layer.parent},
                                {"visible", layer.visible},
                                {"depth", depth},
                                {"keyTimes", times}});
    }
    return list;
}

QVariantMap Backend::values() const {
    if (m_selected < 0 || m_selected >= m_scene.layers.size())
        return {};
    const Layer &layer = m_scene.layers.at(m_selected);
    QVariantMap map{{"name", layer.name},
                    {"source", layer.source},
                    {"parent", layer.parent},
                    {"anchorX", layer.anchor.x()},
                    {"blend", layer.blend},
                    {"anchorY", layer.anchor.y()}};
    QVariantList animated;
    for (const QString &property : Scene::properties()) {
        map.insert(property, m_scene.value(layer, property, m_time));
        if (layer.keys.contains(property))
            animated.append(property);
    }
    map.insert("animated", animated);

    // The easing of a keyframe under the playhead, for the inspector.
    QString ease;
    for (const auto &keys : layer.keys)
        for (const Key &key : keys)
            if (std::abs(key.t - m_time) < .5 / m_scene.fps)
                ease = key.ease;
    map.insert("keyEase", ease);
    return map;
}

int Backend::frame() const { return int(std::lround(m_time * m_scene.fps)); }

QVariantList Backend::cutPoints() const {
    QVariantList list;
    for (const QPointF &point : m_cut)
        list.append(point);
    return list;
}

bool Backend::open(const QString &pathOrUrl) {
    const QString path = localPath(pathOrUrl);
    QString error;
    const QString directory = Project::open(path, &error);
    if (directory.isEmpty()) {
        setError(error);
        return false;
    }

    flush();
    if (m_playing)
        togglePlay();
    QByteArray bytes;
    Scene scene = Project::load(directory, &error, &bytes);
    m_directory = QFileInfo(directory).absoluteFilePath();
    m_assets.setDirectory(m_directory);
    m_written = bytes;
    m_diskBroken = !error.isEmpty();
    m_scene = m_diskBroken ? Scene() : scene;
    m_undo.clear();
    m_redo.clear();
    m_cut.clear();
    m_reshaping.clear();
    m_keySelection.clear();
    m_clipboard.clear();
    m_tool = "move";
    m_time = 0;
    m_selected = m_scene.layers.isEmpty() ? -1 : m_scene.layers.size() - 1;
    setError(error);
    rearmWatcher();

    emit projectChanged();
    emit historyChanged();
    emit toolChanged();
    emit selectedChanged();
    emit timeChanged();
    changed(false);
    setNotice("Opened " + QDir(m_directory).dirName());
    return !m_diskBroken;
}

void Backend::openImageDialog() {
    if (m_picker)
        m_picker->choose(FilePicker::Purpose::OpenImage);
    else
        setError("There's no file chooser available.");
}

void Backend::openFolderDialog() {
    if (m_picker)
        m_picker->choose(FilePicker::Purpose::OpenFolder);
    else
        setError("There's no file chooser available.");
}

void Backend::importImageDialog() {
    if (!hasProject())
        return openImageDialog();
    if (m_picker)
        m_picker->choose(FilePicker::Purpose::ImportImage);
    else
        setError("There's no file chooser available.");
}

bool Backend::importImage(const QString &path) {
    if (!hasProject())
        return open(path);
    Scene scene = m_scene;
    QString error;
    const int index = Project::importImage(scene, m_directory, localPath(path), &error);
    if (index < 0) {
        setError(error);
        return false;
    }
    checkpoint();
    m_scene = scene;
    m_selected = index;
    emit selectedChanged();
    changed();
    rearmWatcher();
    setNotice("Added " + m_scene.layers.at(index).name);
    return true;
}

void Backend::seek(double t) {
    if (!hasProject())
        return;
    // Scrubbing lands on whole frames, so keys set there line up with export.
    t = std::round(std::clamp(t, 0.0, m_scene.duration) * m_scene.fps) / m_scene.fps;
    if (t == m_time)
        return;
    m_time = t;
    emit timeChanged();
    emit valuesChanged();
}

void Backend::togglePlay() {
    if (!hasProject())
        return;
    m_playing = !m_playing;
    if (m_playing) {
        if (m_time >= m_scene.duration - .5 / m_scene.fps)
            m_time = 0;
        m_clock.start();
        m_playTimer.start();
    } else {
        m_playTimer.stop();
        seek(m_time);
    }
    emit playingChanged();
}

void Backend::tick() {
    m_time += m_clock.restart() / 1000.0;
    if (m_time >= m_scene.duration)
        m_time = std::fmod(m_time, m_scene.duration);
    emit timeChanged();
    emit valuesChanged();
}

void Backend::stepFrames(int frames) {
    if (m_playing)
        togglePlay();
    seek(m_time + frames / m_scene.fps);
}

void Backend::jumpKey(int direction) {
    if (m_playing)
        togglePlay();
    QList<double> times;
    if (m_selected >= 0)
        times = m_scene.keyTimes(m_selected);
    else
        for (int i = 0; i < m_scene.layers.size(); ++i)
            times += m_scene.keyTimes(i);
    std::sort(times.begin(), times.end());
    const double epsilon = .5 / m_scene.fps;
    if (direction > 0) {
        for (double t : times)
            if (t > m_time + epsilon)
                return seek(t);
        seek(m_scene.duration);
    } else {
        for (auto it = times.rbegin(); it != times.rend(); ++it)
            if (*it < m_time - epsilon)
                return seek(*it);
        seek(0);
    }
}

void Backend::jumpTo(bool end) {
    if (m_playing)
        togglePlay();
    seek(end ? m_scene.duration : 0);
}

void Backend::select(int index) {
    if (index < -1 || index >= m_scene.layers.size())
        index = -1;
    if (index == m_selected)
        return;
    m_selected = index;
    emit selectedChanged();
    emit valuesChanged();
}

void Backend::selectNext(int direction) {
    const int count = m_scene.layers.size();
    if (count == 0)
        return;
    select(m_selected < 0 ? (direction > 0 ? 0 : count - 1) : (m_selected + direction + count) % count);
}

void Backend::selectAt(double x, double y) { select(layerAt(m_scene, m_assets, m_time, QPointF(x, y))); }

void Backend::toggleVisible(int index) {
    if (index < 0 || index >= m_scene.layers.size())
        return;
    checkpoint();
    m_scene.layers[index].visible = !m_scene.layers[index].visible;
    changed();
}

void Backend::moveLayer(int direction) {
    const int target = m_selected + direction;
    if (m_selected < 0 || target < 0 || target >= m_scene.layers.size())
        return;
    setKeySelection({});
    checkpoint();
    m_scene.layers.swapItemsAt(m_selected, target);
    m_selected = target;
    emit selectedChanged();
    changed();
}

void Backend::deleteLayer() {
    if (m_selected < 0)
        return;
    setKeySelection({});
    checkpoint();
    const QString name = m_scene.layers.at(m_selected).name;
    m_scene.layers.removeAt(m_selected);
    bool unlinked = false;
    for (Layer &layer : m_scene.layers) {
        if (layer.parent == name) {
            layer.parent.clear();
            unlinked = true;
        }
    }
    m_selected = std::min(m_selected, int(m_scene.layers.size()) - 1);
    emit selectedChanged();
    changed();
    setNotice(unlinked ? QString("Deleted %1 and unlinked its pieces. Ctrl+Z brings it back.").arg(name)
                       : QString("Deleted %1. Ctrl+Z brings it back.").arg(name));
}

bool Backend::renameLayer(int index, const QString &wanted) {
    const QString name = wanted.trimmed();
    if (index < 0 || index >= m_scene.layers.size() || name.isEmpty())
        return false;
    const QString old = m_scene.layers.at(index).name;
    if (name == old)
        return true;
    if (m_scene.indexOf(name) >= 0) {
        setError(QString("There's already a layer named \"%1\".").arg(name));
        return false;
    }
    checkpoint();
    m_scene.layers[index].name = name;
    for (Layer &layer : m_scene.layers)
        if (layer.parent == old)
            layer.parent = name;
    Project::renameCutSource(m_scene, old, name);
    changed();
    return true;
}

void Backend::duplicateLayer() {
    if (m_selected < 0)
        return;
    checkpoint();
    Layer copy = m_scene.layers.at(m_selected);
    copy.name = m_scene.uniqueName(copy.name + " copy");
    // The copy is just a layer: it wasn't cut out, and has no holes of its own
    // to redraw, even if the original does.
    copy.raw.remove("cut");
    copy.raw.remove("uncut");
    m_scene.layers.insert(m_selected + 1, copy);
    m_selected += 1;
    setKeySelection({});
    emit selectedChanged();
    changed();
    setNotice("Duplicated as " + copy.name);
}

void Backend::setBlend(const QString &blend) {
    if (m_selected < 0 || !Scene::blends().contains(blend) || m_scene.layers.at(m_selected).blend == blend)
        return;
    checkpoint();
    m_scene.layers[m_selected].blend = blend;
    changed();
    setNotice("Blend: " + blend);
}

void Backend::cycleBlend() {
    if (m_selected < 0)
        return;
    const QStringList &blends = Scene::blends();
    setBlend(blends.at((blends.indexOf(m_scene.layers.at(m_selected).blend) + 1) % blends.size()));
}

void Backend::setProperty(const QString &property, double value) {
    if (m_selected < 0 || !Scene::properties().contains(property) || !std::isfinite(value))
        return;
    checkpoint();
    m_scene.setValue(m_selected, property, m_time, value);
    changed();
}

void Backend::adjust(const QString &property, double delta) {
    if (m_selected < 0)
        return;
    setProperty(property, m_scene.value(m_scene.layers.at(m_selected), property, m_time) + delta);
}

void Backend::multiply(const QString &property, double factor) {
    if (m_selected < 0)
        return;
    setProperty(property, m_scene.value(m_scene.layers.at(m_selected), property, m_time) * factor);
}

void Backend::keyProperty(const QString &property) {
    if (m_selected < 0 || !Scene::properties().contains(property))
        return;
    checkpoint();
    m_scene.keyProperty(m_selected, property, m_time);
    changed();
}

void Backend::keyAll() {
    if (m_selected < 0)
        return;
    checkpoint();
    m_scene.keyAll(m_selected, m_time);
    changed();
    setNotice(QString("Keyframed %1 at %2s").arg(m_scene.layers.at(m_selected).name).arg(m_time, 0, 'f', 2));
}

void Backend::removeKeys() {
    const QList<KeyRef> refs = targetKeys();
    Scene scene = m_scene;
    bool removed = false;
    for (const KeyRef &ref : refs)
        removed |= scene.removeKeysAt(ref.layer, ref.t);
    if (!removed)
        return setNotice("No keyframes at the playhead.");
    checkpoint();
    m_scene = scene;
    setKeySelection({});
    changed();
}

void Backend::cycleEase() {
    const QList<KeyRef> refs = targetKeys();
    if (refs.isEmpty())
        return setNotice("No keyframes at the playhead to ease.");
    Scene scene = m_scene;
    // Every target takes the easing after the first one's, so a mixed
    // selection ends up all alike.
    const QString ease = scene.cycleEaseAt(refs.first().layer, refs.first().t);
    if (ease.isEmpty())
        return setNotice("No keyframes at the playhead to ease.");
    for (const KeyRef &ref : refs)
        scene.setEaseAt(ref.layer, ref.t, ease);
    checkpoint();
    m_scene = scene;
    changed();
    setNotice("Easing out of these keyframes: " + ease);
}

void Backend::setSceneValue(const QString &name, const QVariant &value) {
    if (!hasProject())
        return;
    Scene scene = m_scene;
    if (name == "width" || name == "height") {
        const int v = value.toInt();
        if (v < 2 || v > 8192)
            return setError("The canvas must be between 2 and 8192 pixels on each side.");
        (name == "width" ? scene.width : scene.height) = v;
    } else if (name == "fps") {
        const double v = value.toDouble();
        if (v <= 0 || v > 240)
            return setError("fps must be between 0 and 240.");
        scene.fps = v;
    } else if (name == "duration") {
        const double v = value.toDouble();
        if (v <= 0)
            return setError("The duration must be more than 0 seconds.");
        scene.duration = v;
    } else if (name == "background") {
        const QColor color(value.toString());
        if (!color.isValid())
            return setError(QString("\"%1\" isn't a color.").arg(value.toString()));
        scene.background = color;
    } else {
        return;
    }
    checkpoint();
    m_scene = scene;
    if (m_time > m_scene.duration)
        seek(m_scene.duration);
    changed();
}

QVariantList Backend::keySelection() const {
    QVariantList list;
    for (const KeyRef &ref : m_keySelection)
        list.append(QVariantMap{{"layer", ref.layer}, {"t", ref.t}});
    return list;
}

void Backend::setKeySelection(const QList<KeyRef> &refs) {
    if (refs == m_keySelection)
        return;
    m_keySelection = refs;
    emit keySelectionChanged();
}

QList<KeyRef> Backend::targetKeys() const {
    if (!m_keySelection.isEmpty())
        return m_keySelection;
    if (m_selected < 0)
        return {};
    for (double t : m_scene.keyTimes(m_selected))
        if (std::abs(t - m_time) < .5 / m_scene.fps)
            return {KeyRef{m_selected, t}};
    return {};
}

void Backend::selectKey(int layer, double t, bool add) {
    if (layer < 0 || layer >= m_scene.layers.size())
        return;
    const KeyRef ref{layer, t};
    QList<KeyRef> refs = m_keySelection;
    if (add) {
        if (refs.contains(ref))
            refs.removeAll(ref);
        else
            refs.append(ref);
    } else if (!refs.contains(ref)) {
        // Pressing on a key that's already selected keeps the rest, so the
        // whole selection can be dragged from any of its keys.
        refs = {ref};
    }
    select(layer);
    setKeySelection(refs);
}

void Backend::clearKeySelection() { setKeySelection({}); }

void Backend::beginKeyDrag() {
    if (m_keySelection.isEmpty())
        return;
    if (m_playing)
        togglePlay();
    beginGesture();
    m_keyDragBase = m_scene;
    m_keyDragFrom = m_keySelection;
    m_keyDragging = true;
}

void Backend::dragKeys(double delta) {
    if (!m_keyDragging)
        return;
    // Keep the whole selection inside the animation, so spacing never squashes
    // against either end.
    double first = m_scene.duration, last = 0;
    for (const KeyRef &ref : m_keyDragFrom) {
        first = std::min(first, ref.t);
        last = std::max(last, ref.t);
    }
    delta = std::clamp(delta, -first, m_scene.duration - last);
    delta = std::round(delta * m_scene.fps) / m_scene.fps;
    m_scene = m_keyDragBase;
    setKeySelection(m_scene.shiftKeys(m_keyDragFrom, delta));
    changed();
    setNotice(QString("%1%2 s").arg(delta >= 0 ? "+" : "").arg(delta, 0, 'f', 2));
}

void Backend::endKeyDrag() {
    m_keyDragging = false;
    endGesture();
}

void Backend::copyKeys() {
    const QList<KeyRef> refs = targetKeys();
    if (refs.isEmpty())
        return setNotice("Select keyframes to copy, or put the playhead on some.");
    m_clipboard = m_scene.copyKeys(refs);
    setNotice(QString("Copied %1 keyframe%2").arg(m_clipboard.size()).arg(m_clipboard.size() == 1 ? "" : "s"));
}

void Backend::pasteKeys() {
    if (m_clipboard.isEmpty())
        return setNotice("Nothing copied yet.");
    if (m_selected < 0)
        return setNotice("Select the layer to paste onto.");
    checkpoint();
    setKeySelection(m_scene.pasteKeys(m_selected, m_time, m_clipboard));
    changed();
}

void Backend::beginGesture() {
    checkpoint();
    m_inGesture = true;
}

void Backend::endGesture() { m_inGesture = false; }

void Backend::beginDrag(double x, double y) {
    if (m_selected < 0)
        return;
    beginGesture();
    const Layer &layer = m_scene.layers.at(m_selected);
    m_dragStart = QPointF(x, y);
    m_dragOrigin = QPointF(m_scene.value(layer, "x", m_time), m_scene.value(layer, "y", m_time));
    m_dragging = true;
}

void Backend::dragTo(double x, double y) {
    if (!m_dragging || m_selected < 0)
        return;
    // Positions are in the parent's pixels, so the mouse's movement on the
    // canvas is turned back through the parent before it's applied.
    const int parent = m_scene.indexOf(m_scene.layers.at(m_selected).parent);
    bool invertible = true;
    const QTransform inverse =
        parent < 0 ? QTransform() : layerTransform(m_scene, m_assets, parent, m_time).inverted(&invertible);
    if (!invertible)
        return;
    const QPointF delta = inverse.map(QPointF(x, y)) - inverse.map(m_dragStart);
    m_scene.setValue(m_selected, "x", m_time, m_dragOrigin.x() + delta.x());
    m_scene.setValue(m_selected, "y", m_time, m_dragOrigin.y() + delta.y());
    changed();
}

void Backend::endDrag() {
    m_dragging = false;
    endGesture();
}

void Backend::setTool(const QString &tool) {
    if (tool != "move" && tool != "cut" && tool != "pivot")
        return;
    if (tool != "move" && m_selected < 0) {
        setNotice(tool == "cut" ? "Select the layer to cut a piece out of first." : "Select a layer first.");
        return;
    }
    if (m_playing)
        togglePlay();
    m_tool = tool;
    m_cut.clear();
    m_reshaping.clear();
    m_fill = true;
    emit toolChanged();
}

void Backend::startReshape() {
    if (m_selected < 0)
        return setNotice("Select a piece to reshape.");
    if (!Project::isCut(m_scene, m_selected))
        return setNotice(QString("%1 wasn't cut out here, so it has no shape to change. Press C to cut from it.")
                             .arg(m_scene.layers.at(m_selected).name));
    double feather = 0;
    bool fill = false;
    const QPolygonF outline = Project::cutOutline(m_scene, m_assets, m_selected, m_time, &feather, &fill);
    if (outline.isEmpty())
        return setError(QString("The layer %1 was cut from is gone, so it can't be reshaped.")
                            .arg(m_scene.layers.at(m_selected).name));
    if (m_playing)
        togglePlay();
    m_tool = "cut";
    m_cut = outline;
    m_reshaping = m_scene.layers.at(m_selected).name;
    m_feather = std::round(feather * 10) / 10;
    m_fill = fill;
    emit toolChanged();
}

void Backend::toggleFill() {
    m_fill = !m_fill;
    emit toolChanged();
}

void Backend::cycleFeather() {
    static const QList<double> steps{0, 1, 3, 8, 20};
    int next = 0;
    while (next < steps.size() && steps.at(next) <= m_feather + 1e-6)
        ++next;
    m_feather = next < steps.size() ? steps.at(next) : steps.first();
    emit toolChanged();
}

void Backend::addCutPoint(double x, double y) {
    if (m_tool != "cut")
        return;
    m_cut.append(QPointF(x, y));
    emit toolChanged();
}

int Backend::cutPointNear(double x, double y, double radius) const {
    int best = -1;
    double bestDistance = radius;
    for (int i = 0; i < m_cut.size(); ++i) {
        const double d = QLineF(m_cut.at(i), QPointF(x, y)).length();
        if (d <= bestDistance) {
            best = i;
            bestDistance = d;
        }
    }
    return best;
}

int Backend::insertCutPoint(double x, double y, double radius) {
    if (m_tool != "cut" || m_cut.size() < 3)
        return -1;
    // The nearest edge of the closed shape, measured to the segment itself.
    const QPointF point(x, y);
    int best = -1;
    double bestDistance = radius;
    for (int i = 0; i < m_cut.size(); ++i) {
        const QPointF a = m_cut.at(i), b = m_cut.at((i + 1) % m_cut.size());
        const QPointF ab = b - a;
        const double length = QPointF::dotProduct(ab, ab);
        const double along = length <= 0 ? 0 : std::clamp(QPointF::dotProduct(point - a, ab) / length, 0.0, 1.0);
        const double d = QLineF(a + ab * along, point).length();
        if (d <= bestDistance) {
            best = i + 1;
            bestDistance = d;
        }
    }
    if (best >= 0) {
        m_cut.insert(best, point);
        emit toolChanged();
    }
    return best;
}

void Backend::moveCutPoint(int index, double x, double y) {
    if (index < 0 || index >= m_cut.size())
        return;
    m_cut[index] = QPointF(x, y);
    emit toolChanged();
}

void Backend::removeCutPointAt(int index) {
    if (index < 0 || index >= m_cut.size())
        return;
    m_cut.removeAt(index);
    emit toolChanged();
}

void Backend::removeCutPoint() {
    if (m_cut.isEmpty())
        return setTool("move");
    m_cut.removeLast();
    emit toolChanged();
}

void Backend::finishCut(bool punch) {
    if (m_tool != "cut")
        return;
    Scene scene = m_scene;
    QString error;
    int index = -1;
    if (!m_reshaping.isEmpty()) {
        index = scene.indexOf(m_reshaping);
        if (!Project::reshapeCut(scene, m_assets, index, QPolygonF(m_cut), m_time, m_feather, &error, m_fill ? 1 : 0))
            return setError(error);
    } else {
        index = Project::cutOut(scene, m_assets, m_selected, QPolygonF(m_cut), m_time, punch, &error, {}, m_feather,
                                m_fill);
        if (index < 0)
            return setError(error);
    }
    const bool reshaped = !m_reshaping.isEmpty();
    checkpoint();
    m_scene = scene;
    m_selected = index;
    m_tool = "move";
    m_cut.clear();
    m_reshaping.clear();
    emit toolChanged();
    emit selectedChanged();
    changed();
    rearmWatcher();
    setNotice(reshaped ? QString("Reshaped %1.").arg(m_scene.layers.at(index).name)
                       : QString("Cut out %1. Press P to set where it pivots, then K to start animating it.")
                             .arg(m_scene.layers.at(index).name));
}

void Backend::setPivotAt(double x, double y) {
    if (m_selected < 0)
        return;
    checkpoint();
    Project::setPivot(m_scene, m_assets, m_selected, QPointF(x, y), m_time);
    m_tool = "move";
    emit toolChanged();
    changed();
}

void Backend::undo() {
    if (m_undo.isEmpty())
        return;
    setKeySelection({});
    const QString name = selectedName();
    m_redo.append(m_scene.toJson());
    m_scene = Scene::fromJson(m_undo.takeLast());
    restoreSelection(name);
    emit historyChanged();
    changed();
}

void Backend::redo() {
    if (m_redo.isEmpty())
        return;
    setKeySelection({});
    const QString name = selectedName();
    m_undo.append(m_scene.toJson());
    m_scene = Scene::fromJson(m_redo.takeLast());
    restoreSelection(name);
    emit historyChanged();
    changed();
}

void Backend::exportAs(const QString &format) {
    if (!hasProject() || m_exporter.busy())
        return;
    if (m_diskBroken)
        return setError("Fix scene.json before exporting; " + m_error);
    if (m_playing)
        togglePlay();
    const QDir renders(QDir(m_directory).filePath("renders"));
    const QString output = format == "png" ? renders.filePath(projectName() + "-frames")
                                           : renders.filePath(projectName() + "." + format);
    if (!m_exporter.start(m_scene, m_directory, output))
        return setError(m_exporter.error());
    m_exportProgress = 0;
    emit exportChanged();
}

void Backend::cancelExport() { m_exporter.cancel(); }

void Backend::openRenders() {
    const QString renders = QDir(m_directory).filePath("renders");
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo::exists(renders) ? renders : m_directory));
}

void Backend::dismissError() { setError({}); }

void Backend::flush() {
    if (m_saveTimer.isActive()) {
        m_saveTimer.stop();
        save();
    }
}

void Backend::setError(const QString &error) {
    if (error == m_error)
        return;
    m_error = error;
    emit errorChanged();
}

void Backend::setNotice(const QString &notice) {
    m_notice = notice;
    emit noticeChanged();
    if (!notice.isEmpty())
        m_noticeTimer.start();
}

void Backend::checkpoint() {
    if (m_inGesture || !hasProject())
        return;
    m_undo.append(m_scene.toJson());
    if (m_undo.size() > UndoLimit)
        m_undo.removeFirst();
    m_redo.clear();
    emit historyChanged();
}

void Backend::changed(bool saveSoon) {
    ++m_revision;
    reportMissing();
    emit sceneChanged();
    emit valuesChanged();
    if (saveSoon)
        m_saveTimer.start();
}

void Backend::save() {
    // While scene.json is broken on disk, someone is mid-edit in it; saving
    // would throw their text away.
    if (!hasProject() || m_diskBroken)
        return;
    QString error;
    if (!Project::save(m_directory, m_scene, &error, &m_written))
        setError(error);
}

void Backend::reloadFromDisk() {
    if (!hasProject())
        return;
    rearmWatcher();
    QFile file(Project::sceneFile(m_directory));
    // Mid-rename the file can be briefly gone; the next change brings it back.
    if (!file.exists())
        return;
    QString error;
    QByteArray bytes;
    const Scene scene = Project::load(m_directory, &error, &bytes);
    if (bytes == m_written && !m_diskBroken) {
        // Ours, or only an image changed: repaint with whatever's on disk now.
        changed(false);
        return;
    }
    if (!error.isEmpty()) {
        m_diskBroken = true;
        setError(error);
        return;
    }
    const bool wasBroken = m_diskBroken;
    m_diskBroken = false;
    if (wasBroken)
        setError({});
    const QString name = selectedName();
    checkpoint();
    setKeySelection({});
    m_scene = scene;
    m_written = bytes;
    restoreSelection(name);
    if (m_time > m_scene.duration)
        m_time = m_scene.duration;
    emit timeChanged();
    changed(false);
    setNotice("Reloaded scene.json");
}

void Backend::rearmWatcher() {
    const QStringList watched = m_watcher.files() + m_watcher.directories();
    if (!watched.isEmpty())
        m_watcher.removePaths(watched);
    if (!hasProject())
        return;
    const QDir dir(m_directory);
    QStringList paths{m_directory, Project::sceneFile(m_directory), dir.filePath("assets")};
    for (const Layer &layer : m_scene.layers)
        paths.append(dir.absoluteFilePath(layer.source));
    paths.removeDuplicates();
    for (const QString &path : paths)
        if (QFileInfo::exists(path))
            m_watcher.addPath(path);
}

void Backend::reportMissing() {
    QStringList missing;
    for (const Layer &layer : m_scene.layers)
        if (!QFileInfo::exists(QDir(m_directory).absoluteFilePath(layer.source)))
            missing.append(QString("%1 (layer \"%2\")").arg(layer.source, layer.name));
    if (!missing.isEmpty())
        setError("Can't find " + missing.join(", ") + " in the project folder.");
    else if (m_error.startsWith("Can't find "))
        setError({});
}

QString Backend::selectedName() const {
    return m_selected >= 0 && m_selected < m_scene.layers.size() ? m_scene.layers.at(m_selected).name : QString();
}

void Backend::restoreSelection(const QString &name) {
    int index = m_scene.indexOf(name);
    if (index < 0 && !m_scene.layers.isEmpty())
        index = std::clamp(m_selected, 0, int(m_scene.layers.size()) - 1);
    m_selected = index;
    emit selectedChanged();
}
