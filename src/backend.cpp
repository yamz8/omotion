#include "backend.h"

#include <QDesktopServices>
#include <QStandardPaths>
#include <QtConcurrent>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLineF>
#include <QPainter>
#include <QProcess>
#include <QCoreApplication>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <limits>

#include "project.h"

namespace {
constexpr int UndoLimit = 200;

QString localPath(const QString &pathOrUrl) {
    const QUrl url(pathOrUrl);
    return url.isLocalFile() ? url.toLocalFile() : pathOrUrl;
}
}

Backend::Backend(FilePicker *picker, QObject *parent) : QObject(parent), m_picker(picker) {
    // The window draws big pictures small; keep just the smaller copies.
    m_assets.setKeepFull(false);
    m_workAssets.setKeepFull(false);
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
    connect(&m_download, &ModelDownload::progress, this, [this](qint64 received, qint64 total) {
        m_downloadProgress = total > 0 ? double(received) / total : 0;
        emit downloadChanged();
    });
    connect(&m_download, &ModelDownload::finished, this, [this](bool ok, const QString &message) {
        m_downloadProgress = 0;
        emit downloadChanged();
        if (ok)
            setNotice("The AI is ready: D splits a picture by depth, and W selects things.");
        else
            setError(message);
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

Backend::~Backend() {
    // The AI's job uses this backend's cache; let it finish first.
    m_job.waitForFinished();
    flush();
}

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
            {"background", m_scene.background.name(QColor::HexArgb)},
            {"cameraKeys", [this] {
                 QVariantList times;
                 for (double t : m_scene.keyTimes(Scene::CameraIndex))
                     times.append(t);
                 return times;
             }()}};
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
    auto easeHere = [this](const Layer &layer) {
        QString ease;
        for (const auto &keys : layer.keys)
            for (const Key &key : keys)
                if (std::abs(key.t - m_time) < .5 / m_scene.fps)
                    ease = key.ease;
        return ease;
    };
    if (m_cameraSelected) {
        const Layer &camera = m_scene.camera;
        QVariantMap map{{"name", "Camera"}, {"camera", true}};
        QVariantList animated;
        for (const QString &property : Scene::cameraProperties()) {
            map.insert(property, m_scene.value(camera, property, m_time));
            if (camera.keys.contains(property))
                animated.append(property);
        }
        map.insert("animated", animated);
        map.insert("keyEase", easeHere(camera));
        map.insert("keyCurve", QVariant::fromValue(Scene::curveOf(easeHere(camera))));
        return map;
    }
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
    map.insert("follow", followOf(m_scene, m_selected));
    map.insert("ownFollow", !std::isnan(layer.follow));
    // The easing of a keyframe under the playhead, for the inspector.
    map.insert("keyEase", easeHere(layer));
    map.insert("keyCurve", QVariant::fromValue(Scene::curveOf(easeHere(layer))));
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
    m_aiTake.clear();
    m_aiLeave.clear();
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
    if (index == m_selected && !m_cameraSelected)
        return;
    ++m_aiGeneration;
    m_selected = index;
    m_cameraSelected = false;
    emit selectedChanged();
    emit valuesChanged();
}

void Backend::selectCamera() {
    if (!hasProject() || m_cameraSelected)
        return;
    if (m_tool != "move")
        setTool("move");
    ++m_aiGeneration;
    m_selected = -1;
    m_cameraSelected = true;
    emit selectedChanged();
    emit valuesChanged();
    setNotice("Camera: drag the canvas to pan, the wheel zooms, [ ] turns, K keyframes it.");
}

void Backend::setFollow(double follow) {
    if (m_selected < 0 || (follow >= 0 && !std::isfinite(follow)))
        return;
    checkpoint();
    m_scene.layers[m_selected].follow = follow < 0 ? std::numeric_limits<double>::quiet_NaN() : std::clamp(follow, 0.0, 2.0);
    changed();
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
        // Its pieces become plain layers: left naming it, a new layer of the
        // same name would take them over.
        if (layer.raw.value("cut").toObject().value("from").toString() == name) {
            layer.raw.remove("cut");
            layer.raw.remove("uncut");
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

void Backend::setProperty(const QString &name, double value) {
    const QString property = propertyFor(name);
    const QStringList &valid = m_cameraSelected ? Scene::cameraProperties() : Scene::properties();
    if (!targetLayer() || !valid.contains(property) || !std::isfinite(value))
        return;
    if (property == "zoom" && value <= 0)
        return;
    checkpoint();
    m_scene.setValue(target(), property, m_time, value);
    changed();
}

void Backend::adjust(const QString &property, double delta) {
    if (const Layer *layer = targetLayer())
        setProperty(property, m_scene.value(*layer, propertyFor(property), m_time) + delta);
}

void Backend::multiply(const QString &property, double factor) {
    if (const Layer *layer = targetLayer())
        setProperty(property, m_scene.value(*layer, propertyFor(property), m_time) * factor);
}

void Backend::keyProperty(const QString &name) {
    const QString property = propertyFor(name);
    const QStringList &valid = m_cameraSelected ? Scene::cameraProperties() : Scene::properties();
    if (!targetLayer() || !valid.contains(property))
        return;
    checkpoint();
    m_scene.keyProperty(target(), property, m_time);
    changed();
}

void Backend::keyAll() {
    if (!targetLayer())
        return;
    checkpoint();
    m_scene.keyAll(target(), m_time);
    changed();
    setNotice(QString("Keyframed %1 at %2s").arg(m_cameraSelected ? QString("the camera") : targetLayer()->name).arg(m_time, 0, 'f', 2));
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

void Backend::setCurve(double x1, double y1, double x2, double y2) {
    const QList<KeyRef> refs = targetKeys();
    if (refs.isEmpty() || !std::isfinite(x1 + y1 + x2 + y2))
        return;
    const QString ease = Scene::cubicEase(x1, std::clamp(y1, -2.0, 3.0), x2, std::clamp(y2, -2.0, 3.0));
    checkpoint();
    for (const KeyRef &ref : refs)
        m_scene.setEaseAt(ref.layer, ref.t, ease);
    changed();
}

void Backend::setSceneValue(const QString &name, const QVariant &value) {
    if (!hasProject())
        return;
    Scene scene = m_scene;
    if (name == "width" || name == "height") {
        const int v = value.toInt();
        if (v < 2 || v > 8192)
            return setError("The canvas must be between 2 and 8192 pixels on each side.");
        const QString axis = name == "width" ? "x" : "y";
        int &side = name == "width" ? scene.width : scene.height;
        if (scene.camera.base.value(axis) == side / 2.0 && !scene.camera.keys.contains(axis))
            scene.camera.base[axis] = v / 2.0;
        side = v;
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
    if (!targetLayer())
        return {};
    for (double t : m_scene.keyTimes(target()))
        if (std::abs(t - m_time) < .5 / m_scene.fps)
            return {KeyRef{target(), t}};
    return {};
}

void Backend::selectKey(int layer, double t, bool add) {
    if (!m_scene.at(layer))
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
    if (layer == Scene::CameraIndex)
        selectCamera();
    else
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
    if (!targetLayer())
        return setNotice("Select the layer to paste onto.");
    // Pasting a layer's keys on the camera keeps only what a camera has.
    QList<KeyClip> clip = m_clipboard;
    if (m_cameraSelected) {
        for (KeyClip &item : clip)
            if (item.property == "scale")
                item.property = "zoom";
        clip.erase(std::remove_if(clip.begin(), clip.end(),
                                  [](const KeyClip &item) { return !Scene::cameraProperties().contains(item.property); }),
                   clip.end());
    }
    checkpoint();
    setKeySelection(m_scene.pasteKeys(target(), m_time, clip));
    changed();
}

void Backend::beginGesture() {
    if (m_inGesture || !hasProject())
        return;
    // The undo step is recorded at the end, and only if something changed:
    // a click that moves nothing shouldn't cost the redo history.
    m_gestureStart = m_scene.toJson();
    m_inGesture = true;
}

void Backend::endGesture() {
    if (!m_inGesture)
        return;
    m_inGesture = false;
    if (m_scene.toJson() == m_gestureStart)
        return;
    m_undo.append(m_gestureStart);
    if (m_undo.size() > UndoLimit)
        m_undo.removeFirst();
    m_redo.clear();
    emit historyChanged();
}

void Backend::beginDrag(double x, double y) {
    if (!targetLayer())
        return;
    beginGesture();
    const Layer &layer = *targetLayer();
    m_dragStart = QPointF(x, y);
    m_dragOrigin = QPointF(m_scene.value(layer, "x", m_time), m_scene.value(layer, "y", m_time));
    m_dragging = true;
}

void Backend::dragTo(double x, double y) {
    if (!m_dragging || !targetLayer())
        return;
    if (m_cameraSelected) {
        // Dragging the view: the picture follows the hand, so the camera goes
        // the other way, by the distance in world pixels.
        const double zoom = std::max(1e-6, m_scene.value(m_scene.camera, "zoom", m_time));
        QTransform turn;
        turn.rotate(m_scene.value(m_scene.camera, "rotation", m_time));
        const QPointF moved = turn.map(QPointF(x, y) - m_dragStart) / zoom;
        m_scene.setValue(Scene::CameraIndex, "x", m_time, m_dragOrigin.x() - moved.x());
        m_scene.setValue(Scene::CameraIndex, "y", m_time, m_dragOrigin.y() - moved.y());
        changed();
        return;
    }
    // Positions are in the parent's pixels, so the mouse's movement on the
    // canvas is turned back through the parent before it's applied.
    bool invertible = true;
    const QTransform inverse = parentTransform(m_scene, m_assets, m_selected, m_time).inverted(&invertible);
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
    if (tool != "move" && tool != "cut" && tool != "pivot" && tool != "wand" && tool != "brush")
        return;
    if (tool != "brush") {
        m_brushPiece.clear();
        m_brushMask = QImage();
        m_brushPreview = QImage();
        m_brushing = false;
    }
    if (tool != "move" && m_selected < 0) {
        setNotice(tool == "cut" ? "Select the layer to cut a piece out of first." : "Select a layer first.");
        return;
    }
    if (m_playing)
        togglePlay();
    // A smart selection carries over into the cut tool, to fine-tune its points.
    const bool keepShape = tool == "cut" && m_tool == "wand" && !m_cut.isEmpty();
    m_tool = tool;
    ++m_aiGeneration;
    m_seeds.clear();
    m_seedLevels.clear();
    m_tolerance = -1;
    m_aiTake.clear();
    m_aiLeave.clear();
    m_aiChoice = 0;
    m_aiChoices = 0;
    if (!keepShape) {
        m_cut.clear();
        m_reshaping.clear();
        m_fill = defaultFill();
    }
    emit toolChanged();
}

void Backend::smartSelect(double x, double y, bool add) {
    if (m_tool != "wand" || m_selected < 0)
        return;
    if (m_wandMode == "ai") {
        if (!needAi())
            return;
        if (!add) {
            m_aiTake.clear();
            m_aiLeave.clear();
            m_aiChoice = 0;
        }
        m_aiTake.append(QPointF(x, y));
        return runAiSelect();
    }
    if (!add) {
        m_seeds.clear();
        m_seedLevels.clear();
        m_tolerance = -1;
    }
    m_seeds.append(QPointF(x, y));
    reselect();
}

void Backend::adjustTolerance(int delta) {
    if (m_wandMode == "ai" || m_seeds.isEmpty())
        return;
    m_tolerance = std::clamp((m_tolerance < 0 ? m_usedTolerance : m_tolerance) + delta, 1, 160);
    reselect();
}

void Backend::cycleTolerance() {
    // In AI select, T steps through its guesses instead: part, thing, whole.
    if (m_wandMode == "ai") {
        if (m_aiChoices < 2 || m_aiTake.isEmpty())
            return;
        m_aiChoice = (m_aiChoice + 1) % m_aiChoices;
        return runAiSelect();
    }
    // Automatic, then by hand from tight to loose, then back to automatic.
    static const QList<int> steps{8, 16, 24, 32, 48, 64};
    if (m_tolerance < 0) {
        m_tolerance = steps.first();
    } else {
        int next = 0;
        while (next < steps.size() && steps.at(next) <= m_tolerance)
            ++next;
        m_tolerance = next < steps.size() ? steps.at(next) : -1;
    }
    reselect();
}

void Backend::reselect() {
    if (m_seeds.isEmpty() || m_selected < 0) {
        emit toolChanged();
        return;
    }
    const Layer &layer = m_scene.layers.at(m_selected);
    int regions = 0;
    const QPolygonF outline = Project::smartOutline(m_scene, m_assets, m_selected, m_seeds, m_time, m_tolerance,
                                                    &regions, &m_usedTolerance, &m_seedLevels);
    m_cut = outline;
    emit toolChanged();
    if (outline.isEmpty())
        return setNotice(QString("That's not on %1. Click a part of it.").arg(layer.name));
    if (regions > 1)
        setNotice("Only areas that touch make one shape; this is the biggest.");
}

void Backend::setBusy(const QString &busy) {
    if (busy == m_busy)
        return;
    m_busy = busy;
    emit busyChanged();
}

bool Backend::needAi() {
    Ai::instance().retry();
    const QString problem = Ai::instance().problem();
    if (problem.isEmpty())
        return true;
    if (Ai::instance().runtimeMissing()) {
        // Omarchy can install it in a terminal; elsewhere, say how.
        m_aiPrompt = QStandardPaths::findExecutable("omarchy-launch-floating-terminal-with-presentation").isEmpty()
                         ? QString()
                         : QString("install");
        setError(problem);
    } else {
        m_aiPrompt = "download";
    }
    emit aiPromptChanged();
    return false;
}

void Backend::acceptAiPrompt() {
    const QString prompt = m_aiPrompt;
    dismissAiPrompt();
    if (prompt == "download") {
        m_download.start();
        emit downloadChanged();
    } else if (prompt == "install") {
        // The Omarchy way: a terminal where sudo can ask, and Done at the end.
        QProcess::startDetached("omarchy-launch-floating-terminal-with-presentation",
                                {"echo 'Installing ONNX Runtime for omotion...'; omarchy-pkg-add onnxruntime-cpu"});
        setError({});
        setNotice("Once it's installed, try again.");
    }
}

void Backend::dismissAiPrompt() {
    if (m_aiPrompt.isEmpty())
        return;
    m_aiPrompt.clear();
    emit aiPromptChanged();
}

void Backend::cancelDownload() { m_download.cancel(); }

void Backend::cleanUp() {
    if (!hasProject() || m_job.isRunning())
        return;
    flush();
    QList<QJsonObject> scenes{m_scene.toJson()};
    scenes += m_undo;
    scenes += m_redo;
    int count = 0;
    QString error;
    const qint64 freed = Project::cleanAssets(m_directory, scenes, &count, &error);
    if (freed < 0)
        return setError(error);
    if (!error.isEmpty())
        setError(error);
    setNotice(count == 0 ? QString("Every image in the project is in use.")
                         : QString("Moved %1 unused image%2 (%3 MB) to the trash.")
                               .arg(count)
                               .arg(count == 1 ? "" : "s")
                               .arg(freed / 1e6, 0, 'f', 1));
}

void Backend::toggleWandMode() {
    m_wandMode = m_wandMode == "ai" ? "colour" : "ai";
    m_seeds.clear();
    m_seedLevels.clear();
    m_aiTake.clear();
    m_aiLeave.clear();
    m_cut.clear();
    emit toolChanged();
    setNotice(m_wandMode == "ai" ? "AI select: click a thing." : "Colour select: click an area of colour.");
}

void Backend::startBrush() {
    if (m_selected < 0)
        return setNotice("Select a piece to paint its shape.");
    const Layer &layer = m_scene.layers.at(m_selected);
    QTransform toCanvas;
    const QImage mask = Project::cutMask(m_scene, m_assets, m_selected, m_time, &toCanvas);
    if (mask.isNull())
        return setNotice(QString("%1 wasn't cut out here, so it has no shape to paint. Press C or W to cut from it.").arg(layer.name));
    if (m_playing)
        togglePlay();
    setTool("brush");
    m_brushPiece = layer.name;
    m_brushMask = mask;
    m_brushToCanvas = toCanvas;
    // The preview is at most about a thousand pixels across.
    m_brushPreviewScale = std::min(1.0, 1024.0 / std::max(mask.width(), mask.height()));
    const QImage small = mask.scaled(mask.size() * m_brushPreviewScale, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    m_brushPreview = QImage(small.size(), QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < small.height(); ++y) {
        const uchar *from = small.constScanLine(y);
        QRgb *to = reinterpret_cast<QRgb *>(m_brushPreview.scanLine(y));
        for (int x = 0; x < small.width(); ++x)
            to[x] = qRgba(from[x], from[x], from[x], from[x]);
    }
    emit toolChanged();
}

QTransform Backend::brushPreviewToCanvas() const {
    return QTransform::fromScale(1 / m_brushPreviewScale, 1 / m_brushPreviewScale) * m_brushToCanvas;
}

void Backend::paintBrush(const QPointF &fromCanvas, const QPointF &toCanvas) {
    bool invertible = false;
    const QTransform toImage = m_brushToCanvas.inverted(&invertible);
    if (!invertible || m_brushMask.isNull())
        return;
    const QPointF a = toImage.map(fromCanvas), b = toImage.map(toCanvas);
    const double radius = m_brushRadius / std::max(1e-6, std::sqrt(std::abs(m_brushToCanvas.determinant())));
    // The same stroke on the shape and on its preview.
    auto stroke = [&](QImage &image, double scale, const QColor &color) {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        if (m_brushErase)
            painter.setCompositionMode(QPainter::CompositionMode_Clear);
        painter.setPen(QPen(color, 2 * radius * scale, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (a == b) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(color);
            painter.drawEllipse(a * scale, radius * scale, radius * scale);
        } else {
            painter.drawLine(a * scale, b * scale);
        }
    };
    stroke(m_brushMask, 1, Qt::black);
    stroke(m_brushPreview, m_brushPreviewScale, Qt::white);
    emit toolChanged();
}

void Backend::beginBrush(double x, double y, bool erase) {
    if (m_tool != "brush")
        return;
    m_brushErase = erase;
    m_brushing = true;
    m_brushLast = QPointF(x, y);
    paintBrush(m_brushLast, m_brushLast);
}

void Backend::brushTo(double x, double y) {
    if (!m_brushing)
        return;
    paintBrush(m_brushLast, QPointF(x, y));
    m_brushLast = QPointF(x, y);
}

void Backend::endBrush() { m_brushing = false; }

void Backend::adjustBrush(double factor) {
    m_brushRadius = std::clamp(m_brushRadius * factor, 2.0, 400.0);
    emit toolChanged();
}

void Backend::finishBrush() {
    if (m_tool != "brush" || m_job.isRunning())
        return;
    const QString piece = m_brushPiece;
    const QImage mask = m_brushMask;
    const QByteArray started = Project::serialize(m_scene);
    auto result = std::make_shared<std::tuple<Scene, bool, QString>>(m_scene, false, QString());
    const QString directory = m_directory;
    setBusy("Reshaping…");
    m_job.disconnect(this);
    connect(&m_job, &QFutureWatcher<void>::finished, this, [this, result, started, piece] {
        setBusy({});
        m_workAssets.clear();
        const auto &[scene, ok, error] = *result;
        if (!ok)
            return setError(error);
        if (Project::serialize(m_scene) != started)
            return setError("The scene changed while reshaping, so it wasn't kept. Press Enter to apply it again.");
        checkpoint();
        m_scene = scene;
        m_selected = m_scene.indexOf(piece);
        setKeySelection({});
        setTool("move");
        emit selectedChanged();
        changed();
        rearmWatcher();
        setNotice(QString("Repainted %1's shape.").arg(piece));
    });
    m_job.setFuture(QtConcurrent::run([this, result, directory, piece, mask] {
        m_workAssets.setDirectory(directory);
        auto &[scene, ok, error] = *result;
        ok = Project::reshapeCutMask(scene, m_workAssets, scene.indexOf(piece), mask, &error);
    }));
}

void Backend::excludeAt(double x, double y) {
    if (m_tool != "wand" || m_wandMode != "ai" || m_aiTake.isEmpty())
        return;
    m_aiLeave.append(QPointF(x, y));
    runAiSelect();
}

void Backend::runAiSelect() {
    if (m_job.isRunning()) {
        // One at a time; the latest clicks run when this one's done.
        m_aiAgain = true;
        return;
    }
    m_aiAgain = false;
    const Scene scene = m_scene;
    const int layer = m_selected;
    const QList<QPointF> take = m_aiTake, leave = m_aiLeave;
    const double t = m_time;
    const int choice = m_aiChoice;
    const QString directory = m_directory;
    auto result = std::make_shared<std::tuple<QPolygonF, int, QString>>();
    setBusy(m_aiChoices == 0 && m_aiTake.size() == 1 ? "Looking at the picture…" : "Selecting…");
    const QString layerName = selectedName();
    const int generation = ++m_aiGeneration;
    m_job.disconnect(this);
    connect(&m_job, &QFutureWatcher<void>::finished, this, [this, result, layerName, generation] {
        setBusy({});
        if (m_aiAgain)
            return runAiSelect();
        // The tool, the layer or the clicks changed meanwhile: it's stale.
        if (m_tool != "wand" || m_wandMode != "ai" || selectedName() != layerName || generation != m_aiGeneration)
            return;
        const auto &[outline, choices, error] = *result;
        m_aiChoices = choices;
        m_cut = outline;
        emit toolChanged();
        if (outline.isEmpty())
            setNotice(error.isEmpty() ? "The AI didn't find anything there." : error);
    });
    m_job.setFuture(QtConcurrent::run([this, scene, layer, take, leave, t, choice, directory, result] {
        m_workAssets.setDirectory(directory);
        auto &[outline, choices, error] = *result;
        outline = Project::aiOutline(scene, m_workAssets, layer, take, leave, t, choice, &choices, &error);
    }));
}

void Backend::splitByDepth(int count) {
    if (!hasProject() || m_job.isRunning())
        return;
    int layer = m_selected >= 0 ? m_selected : (m_scene.layers.isEmpty() ? -1 : 0);
    if (layer < 0)
        return setNotice("Add an image to split first.");
    // A piece just cut out is selected; splitting means the picture it came
    // from, which the piece then goes with.
    for (int guard = 0; guard < 16 && layer >= 0; ++guard) {
        const QJsonObject cut = m_scene.layers.at(layer).raw.value("cut").toObject();
        const int from = m_scene.indexOf(cut.value("from").toString());
        if (cut.isEmpty() || cut.contains("depth") || from < 0)
            break;
        layer = from;
    }
    if (!needAi())
        return;
    if (m_playing)
        togglePlay();
    const Scene scene = m_scene;
    const QString directory = m_directory;
    // What the scene was, to tell if it's edited meanwhile. (Not a repaint
    // count: the split's own new images refresh the view as they land.)
    const QByteArray started = Project::serialize(m_scene);
    auto result = std::make_shared<std::pair<Scene, QString>>();
    setBusy("Finding the depth…");
    m_job.disconnect(this);
    connect(&m_job, &QFutureWatcher<void>::finished, this, [this, result, started, count] {
        setBusy({});
        m_workAssets.clear();
        const auto &[split, error] = *result;
        if (!error.isEmpty())
            return setError(error);
        // Something changed while it worked; better to redo than to undo that.
        if (Project::serialize(m_scene) != started)
            return setError("The scene changed while splitting it, so the split wasn't kept. Press D to split again.");
        checkpoint();
        m_scene = split;
        setKeySelection({});
        changed();
        rearmWatcher();
        setNotice(QString("Split into %1 layers by depth. Press A to make it move.").arg(count));
    });
    m_job.setFuture(QtConcurrent::run([this, scene, layer, count, directory, result] {
        m_workAssets.setDirectory(directory);
        Scene edited = scene;
        QString error;
        const QImage depth = Project::depthOf(edited, m_workAssets, layer, &error);
        if (depth.isNull()) {
            result->second = error;
            return;
        }
        QMetaObject::invokeMethod(this, [this] { setBusy("Splitting into layers…"); }, Qt::QueuedConnection);
        if (Project::splitByDepth(edited, m_workAssets, layer, depth, count, 2, &error).isEmpty()) {
            result->second = error;
            return;
        }
        result->first = edited;
    }));
}

void Backend::animateParallax() {
    if (!hasProject() || m_scene.layers.isEmpty())
        return;
    Scene scene = m_scene;
    const bool lengthened = scene.duration < 10;
    QString error;
    if (!Project::animateParallax(scene, m_assets, m_selected >= 0 ? m_selected : 0, 36, &error))
        return setError(error);
    checkpoint();
    m_scene = scene;
    changed();
    setNotice(QString("Added a parallax camera move%1. Space plays it; Ctrl+Z takes it back.")
                  .arg(lengthened ? ", over 10 seconds" : ""));
}

void Backend::applyPreset(const QString &preset) {
    if (m_selected < 0)
        return setNotice("Select the layer to move first.");
    Scene scene = m_scene;
    QString error;
    if (!Project::applyPreset(scene, m_assets, m_selected, preset, m_time, 1, &error))
        return setError(error);
    checkpoint();
    m_scene = scene;
    setKeySelection({});
    changed();
    const QString name = m_scene.layers.at(m_selected).name;
    if (preset == "sway")
        setNotice(QString("%1 sways. P moves where it turns from, like the foot of a tree.").arg(name));
    else
        setNotice(QString("%1: %2. Space plays it; Ctrl+Z takes it back.").arg(name, preset));
}

void Backend::waitForJobs() {
    while (m_job.isRunning() || !m_busy.isEmpty())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
}

void Backend::startReshape() {
    if (m_selected < 0)
        return setNotice("Select a piece to reshape.");
    if (!Project::isCut(m_scene, m_selected)) {
        const Layer &layer = m_scene.layers.at(m_selected);
        if (layer.raw.value("cut").toObject().contains("mask"))
            return startBrush();
        return setNotice(QString("%1 wasn't cut out here, so it has no shape to change. Press C to cut from it.").arg(layer.name));
    }
    double feather = 0;
    int fill = 0;
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

int Backend::defaultFill() {
    // OMOTION_FILL=smooth (or none) skips the AI's fill, which takes seconds a cut.
    const QByteArray chosen = qgetenv("OMOTION_FILL");
    if (chosen == "none")
        return 0;
    if (chosen == "smooth")
        return 1;
    return Ai::instance().canInpaint() ? 2 : 1;
}

void Backend::toggleFill() {
    // AI, smooth, off; without the AI, just smooth and off.
    if (m_fill < 0)
        m_fill = defaultFill();
    const bool ai = Ai::instance().canInpaint();
    m_fill = m_fill == 2 ? 1 : m_fill == 1 ? 0 : (ai ? 2 : 1);
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
    if ((m_tool != "cut" && m_tool != "wand") || m_job.isRunning())
        return;
    const QString reshaping = m_reshaping;
    const QPolygonF shape(m_cut);
    const double t = m_time, feather = m_feather;
    const int fill = m_fill < 0 ? defaultFill() : m_fill;
    const int selected = m_selected;
    // The cut itself: a new piece, or a piece given a new shape. Returns the
    // piece's index, or -1 with the reason in error.
    auto cut = [reshaping, shape, t, feather, fill, selected, punch](Scene &scene, AssetCache &assets, QString &error) {
        if (reshaping.isEmpty())
            return Project::cutOut(scene, assets, selected, shape, t, punch, &error, {}, feather, fill > 0, fill == 2);
        const int index = scene.indexOf(reshaping);
        return Project::reshapeCut(scene, assets, index, shape, t, feather, &error, fill) ? index : -1;
    };
    auto apply = [this, reshaping](const Scene &scene, int index, const QString &error) {
        if (index < 0)
            return setError(error);
        checkpoint();
        m_scene = scene;
        m_selected = index;
        setKeySelection({});
        m_tool = "move";
        m_cut.clear();
        m_reshaping.clear();
        emit toolChanged();
        emit selectedChanged();
        changed();
        rearmWatcher();
        setNotice(!reshaping.isEmpty()
                      ? QString("Reshaped %1.").arg(m_scene.layers.at(index).name)
                      : QString("Cut out %1. Press P to set where it pivots, then K to start animating it.")
                            .arg(m_scene.layers.at(index).name));
    };

    // Small pictures cut in a blink; big ones take seconds, which the window
    // shouldn't freeze for.
    const int from = reshaping.isEmpty()
                         ? selected
                         : m_scene.indexOf(m_scene.layers.value(m_scene.indexOf(reshaping)).raw.value("cut").toObject()
                                               .value("from").toString());
    const QSizeF size = from >= 0 ? m_assets.size(m_scene.layers.at(from).source) : QSizeF();
    // The AI's fill takes seconds whatever the size.
    if (size.width() * size.height() <= 4e6 && fill != 2) {
        Scene scene = m_scene;
        QString error;
        const int index = cut(scene, m_assets, error);
        return apply(scene, index, error);
    }

    const QByteArray started = Project::serialize(m_scene);
    auto result = std::make_shared<std::tuple<Scene, int, QString>>(m_scene, -1, QString());
    const QString directory = m_directory;
    setBusy(fill == 2 ? (reshaping.isEmpty() ? "Cutting, and painting in behind…" : "Reshaping, and painting in behind…")
                      : (reshaping.isEmpty() ? "Cutting…" : "Reshaping…"));
    m_job.disconnect(this);
    connect(&m_job, &QFutureWatcher<void>::finished, this, [this, result, started, apply] {
        setBusy({});
        m_workAssets.clear();
        const auto &[scene, index, error] = *result;
        if (index >= 0 && Project::serialize(m_scene) != started)
            return setError("The scene changed while cutting, so the cut wasn't kept. Press Enter to cut again.");
        apply(scene, index, error);
    });
    m_job.setFuture(QtConcurrent::run([this, result, directory, cut] {
        m_workAssets.setDirectory(directory);
        auto &[scene, index, error] = *result;
        index = cut(scene, m_workAssets, error);
    }));
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

void Backend::exportAs(const QString &format, double scale) {
    if (!hasProject() || m_exporter.busy())
        return;
    if (m_diskBroken)
        return setError("Fix scene.json before exporting; " + m_error);
    if (m_playing)
        togglePlay();
    const QDir renders(QDir(m_directory).filePath("renders"));
    const QString name = projectName() + (scale < 1 ? "-small" : "");
    const QString output = format == "png" ? renders.filePath(name + "-frames") : renders.filePath(name + "." + format);
    if (!m_exporter.start(m_scene, m_directory, output, scale))
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
    // Someone else wrote scene.json since we last did, and the reload hasn't
    // caught up yet: take their version rather than write over it. Ours is
    // kept in undo, so Ctrl+Z brings it back.
    QFile file(Project::sceneFile(m_directory));
    if (file.open(QIODevice::ReadOnly) && file.readAll() != m_written) {
        file.close();
        m_reloadTimer.stop();
        reloadFromDisk();
        return;
    }
    file.close();
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
    m_saveTimer.stop();
    m_dragging = false;
    m_keyDragging = false;
    endGesture();
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
    if (index < 0 && !name.isEmpty() && !m_scene.layers.isEmpty())
        index = std::clamp(m_selected, 0, int(m_scene.layers.size()) - 1);
    m_selected = index;
    emit selectedChanged();
}
