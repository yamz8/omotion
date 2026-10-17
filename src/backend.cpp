#include "backend.h"
#include "world.h"

#include <QDesktopServices>
#include <QStandardPaths>
#include <QtConcurrent>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QFileInfo>
#include <QLineF>
#include <QPainter>
#include <QProcess>
#include <QCoreApplication>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <limits>

#include "cloth.h"
#include "film.h"
#include "gltf.h"
#include "physics.h"
#include "project.h"
#include "steps.h"

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
    // The 3D view follows the scene and the playhead.
    connect(this, &Backend::sceneChanged, this, [this] { m_world.setScene(m_scene, m_directory); });
    connect(this, &Backend::timeChanged, this, [this] { m_world.setTime(m_time); });
    // Another project, maybe another film beside it.
    connect(this, &Backend::projectChanged, this, &Backend::filmChanged);
    // The free view follows the world: off when there's none.
    connect(this, &Backend::sceneChanged, this, [this] {
        if (m_freeView && !World::info(m_scene).valid) {
            m_freeView = false;
            emit viewChanged();
        }
        syncView();
    });

    // Saves are batched, so a drag writes the file a few times, not per pixel.
    // A film plays as fast as its frames draw, keeping to the clock.
    m_filmTimer.setInterval(15);
    connect(&m_filmTimer, &QTimer::timeout, this, [this] {
        if (!m_preview)
            return stopFilmPreview();
        m_filmTime = std::min(m_preview->duration(), m_filmFrom + m_filmClock.elapsed() / 1000.0);
        if (m_filmTime >= m_preview->duration())
            m_filmTimer.stop();
        emit filmPreviewChanged();
        drawFilm(m_filmTime);
    });
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
    connect(&m_peaksJob, &QFutureWatcher<QList<double>>::finished, this, [this] {
        m_peaks = m_peaksJob.result();
        emit audioChanged();
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
        // A film's sound goes in once its picture is written.
        if (m_film) {
            const std::shared_ptr<Film> film = std::move(m_film);
            QString error;
            if (ok && !film->addSound(m_filmOutput, &error))
                setError(error);
        }
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
            else if (purpose == FilePicker::Purpose::ImportAudio)
                setAudioFile(url.toLocalFile());
            else
                open(url.toLocalFile());
        });
    }
}

Backend::~Backend() {
    // The AI's job uses this backend's cache; let it finish first.
    m_job.waitForFinished();
    m_peaksJob.waitForFinished();
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
            {"vanishX", m_scene.vanishPoint().x()},
            {"vanishY", m_scene.vanishPoint().y()},
            {"distance", m_scene.focalDistance()},
            {"look", m_scene.raw.value("look").toObject().toVariantMap()},
            {"looked", !m_scene.raw.value("look").toObject().isEmpty()},
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

QVariantList Backend::outliner() const {
    QVariantList rows;
    if (!hasProject())
        return rows;
    rows.append(QVariantMap{{"kind", "camera"}, {"index", Scene::CameraIndex}, {"name", "Camera"}, {"depth", 0},
                            {"visible", true}, {"animated", !m_scene.keyTimes(Scene::CameraIndex).isEmpty()}});
    const World::Info world = World::info(m_scene);
    const QList<int> inWorld = world.valid ? World::layers(m_scene) : QList<int>();
    // Each layer under what it was cut from (its parent), the top of the
    // stack first among brothers, as the timeline has them.
    auto add = [&](auto &&self, int index, int depth, const QList<int> &among) -> void {
        const Layer &layer = m_scene.layers.at(index);
        QString kind = "image", detail;
        if (layer.raw.value("empty").toBool()) {
            kind = "empty";
            detail = "empty";
        } else if (layer.raw.contains("model")) {
            kind = "model";
            detail = "3D model";
        } else if (layer.raw.contains("light")) {
            kind = "light";
            detail = "light";
        } else if (layer.raw.contains("drawing")) {
            kind = "drawing";
            detail = QString("%1 drawings").arg(layer.raw.value("drawing").toObject().value("frames").toArray().size());
        } else if (!layer.particles.isEmpty()) {
            kind = "particles";
            detail = layer.particles.value("kind").toString();
        } else if (inWorld.contains(index)) {
            const bool root = layer.name == world.picture;
            kind = root ? "picture" : !layer.raw.value("shape").toObject().isEmpty() ? "shape" : "piece";
            detail = root ? "the picture" : kind == "shape" ? "3D shape" : "flat";
        }
        rows.append(QVariantMap{{"kind", kind}, {"index", index}, {"name", layer.name}, {"depth", depth},
                                {"visible", layer.visible}, {"animated", !m_scene.keyTimes(index).isEmpty()},
                                {"detail", detail}});
        for (int i = m_scene.layers.size() - 1; i >= 0; --i)
            if (among.contains(i) && m_scene.layers.at(i).parent == layer.name && i != index)
                self(self, i, depth + 1, among);
    };
    auto tree = [&](const QList<int> &among, int depth) {
        for (int i = m_scene.layers.size() - 1; i >= 0; --i) {
            if (!among.contains(i))
                continue;
            // Those whose parent is elsewhere (or nowhere) start a branch.
            const int parent = m_scene.indexOf(m_scene.layers.at(i).parent);
            if (parent < 0 || !among.contains(parent) || parent == i)
                add(add, i, depth, among);
        }
    };
    if (world.valid) {
        rows.append(QVariantMap{{"kind", "world"}, {"index", -1}, {"name", "World"}, {"depth", 0}, {"visible", true},
                                {"animated", false}, {"detail", QString("%1 pieces").arg(inWorld.size())}});
        tree(inWorld, 1);
    }
    QList<int> rest;
    for (int i = 0; i < m_scene.layers.size(); ++i)
        if (!inWorld.contains(i))
            rest << i;
    tree(rest, 0);
    return rows;
}

QVariantMap Backend::worldInfo() const {
    const World::Info info = World::info(m_scene);
    if (!info.valid)
        return {};
    const QJsonObject world = m_scene.raw.value("world").toObject();
    return {{"hazeColor", info.haze.name()},
            {"hazeDensity", info.hazeDensity},
            {"grain", world.value("grain").toDouble(.025)},
            {"shutter", world.value("shutter").toDouble(180)},
            {"dust", world.value("dust").toString()},
            {"scale", info.scale},
            {"focal", info.focal},
            {"height", info.height}};
}

QVariantList Backend::steps() const {
    QVariantList list;
    for (const Steps::Step &step : Steps::of(m_scene, const_cast<AssetCache &>(m_assets), m_selected))
        list.append(QVariantMap{{"name", step.name},
                                {"title", step.title},
                                {"detail", step.detail},
                                {"done", step.done},
                                {"stale", step.stale},
                                {"why", step.why},
                                {"actions", step.actions}});
    return list;
}

int Backend::staleSteps() const { return int(Steps::stale(m_scene, const_cast<AssetCache &>(m_assets)).size()); }

void Backend::runStep(const QString &step, const QString &action) {
    const int layer = m_selected;
    if (layer < 0 || !hasProject())
        return;
    if (step == "cut" && action == "reshape")
        return startReshape();
    if (Steps::Step::slow(step, action)) {
        const QString busy = step == "world" ? "Reading the picture in 3D again…"
                             : step == "shape" ? "Giving it a 3D shape… (about 20 seconds)"
                                               : "The AI is painting in behind it…";
        worldJob(busy, "Done.", [layer, step, action](Scene &scene, AssetCache &assets, QString *error) {
            return Steps::run(scene, assets, layer, step, action, error);
        });
        return;
    }
    Scene scene = m_scene;
    QString error;
    if (!Steps::run(scene, m_assets, layer, step, action, &error))
        return setError(error);
    checkpoint();
    m_scene = scene;
    changed();
    rearmWatcher();
}

void Backend::redoStale() {
    if (Steps::stale(m_scene, m_assets).isEmpty())
        return setNotice("Nothing is out of date.");
    worldJob("Redoing what's out of date…", "Everything is up to date.", [](Scene &scene, AssetCache &assets, QString *error) {
        return Steps::redoStale(scene, assets, error);
    });
}

QList<Backend::Curve> Backend::curves(int samples) const {
    QList<Curve> list;
    const Layer *layer = targetLayer();
    if (!layer)
        return list;
    for (auto it = layer->keys.begin(); it != layer->keys.end(); ++it) {
        if (it->isEmpty())
            continue;
        Curve curve;
        curve.property = it.key();
        curve.keys = *it;
        curve.low = std::numeric_limits<double>::max();
        curve.high = std::numeric_limits<double>::lowest();
        for (int i = 0; i <= samples; ++i) {
            const double v = m_scene.ownValue(*layer, curve.property, m_scene.duration * i / samples);
            curve.samples << v;
            curve.low = std::min(curve.low, v);
            curve.high = std::max(curve.high, v);
        }
        for (const Key &key : curve.keys) {
            curve.low = std::min(curve.low, key.v);
            curve.high = std::max(curve.high, key.v);
        }
        list << curve;
    }
    // In the order the inspector lists them.
    const QStringList order = Scene::propertiesOf(*layer) + Scene::cameraProperties();
    std::stable_sort(list.begin(), list.end(), [&](const Curve &a, const Curve &b) {
        return order.indexOf(a.property) < order.indexOf(b.property);
    });
    return list;
}

QVariantList Backend::sheet() const {
    QVariantList rows;
    for (const Curve &curve : curves(0)) {
        QVariantList times;
        for (const Key &key : curve.keys)
            times << key.t;
        rows << QVariantMap{{"property", curve.property}, {"times", times}};
    }
    return rows;
}

void Backend::retimeKey(const QString &property, int index, double t) {
    const Layer *layer = m_scene.at(target());
    if (!layer || index < 0 || index >= layer->keys.value(property).size())
        return;
    moveKey(property, index, t, layer->keys.value(property).at(index).v);
}

void Backend::removeKey(const QString &property, int index) {
    Layer *layer = m_scene.at(target());
    if (!layer || index < 0 || index >= layer->keys.value(property).size())
        return;
    checkpoint();
    // The last key going away leaves the property at its value.
    if (layer->keys.value(property).size() == 1)
        layer->base.insert(property, layer->keys.value(property).first().v);
    layer->keys[property].removeAt(index);
    if (layer->keys.value(property).isEmpty())
        layer->keys.remove(property);
    setKeySelection({});
    changed();
}

QStringList Backend::curveNames() const {
    QStringList names;
    for (const Curve &curve : curves(2))
        names << curve.property;
    return names;
}

void Backend::setBody(bool body, bool fixed, double bounce) {
    if (m_selected < 0)
        return;
    Scene scene = m_scene;
    QString error;
    // Its other settings (mass, friction, tumble) stay as they were.
    QJsonObject settings;
    if (body) {
        settings = m_scene.layers.at(m_selected).raw.value("body").toObject();
        if (!settings.contains("mass"))
            settings["mass"] = 1;
        settings["bounce"] = std::clamp(bounce, 0.0, 1.0);
        settings["fixed"] = fixed;
    }
    if (!Physics::setBody(scene, m_selected, settings, &error))
        return setError(error);
    checkpoint();
    m_scene = scene;
    changed();
}

void Backend::setBodyValue(const QString &name, const QVariant &value) {
    if (m_selected < 0)
        return;
    QJsonObject settings = m_scene.layers.at(m_selected).raw.value("body").toObject();
    if (settings.isEmpty())
        return;
    settings[name] = QJsonValue::fromVariant(value);
    Scene scene = m_scene;
    QString error;
    if (!Physics::setBody(scene, m_selected, settings, &error))
        return setError(error);
    checkpoint();
    m_scene = scene;
    changed();
}

void Backend::simulate() {
    Scene scene = m_scene;
    QString error;
    const int moved = Physics::bake(scene, m_assets, m_time, m_scene.duration, &error);
    if (moved < 0)
        return setError(error);
    checkpoint();
    m_scene = scene;
    changed();
    setNotice(QString("%1 bodies fall from here to the end. Space plays it; Ctrl+Z takes it back.").arg(moved));
}

void Backend::setKeyCurve(const QString &property, int index, double x1, double y1, double x2, double y2) {
    Layer *layer = m_scene.at(target());
    if (!layer || !layer->keys.contains(property) || index < 0 || index >= layer->keys.value(property).size())
        return;
    checkpoint();
    layer->keys[property][index].ease = Scene::cubicEase(x1, y1, x2, y2);
    changed();
}

void Backend::setModifier(const QString &property, const QString &type, double amount, double speed) {
    Scene scene = m_scene;
    QString error;
    if (!Project::setModifier(scene, target(), property, type, amount, speed, &error))
        return setError(error);
    checkpoint();
    m_scene = scene;
    changed();
}

void Backend::addClip(const QString &motion, int repeat) {
    Scene scene = m_scene;
    QString error;
    if (!Project::addClip(scene, target(), motion, m_time, repeat, 1, 1, &error))
        return setError(error);
    checkpoint();
    m_scene = scene;
    changed();
    setNotice(QString("%1 is a clip from %2 s: it adds to the animation without changing its keys.").arg(motion).arg(m_time, 0, 'f', 2));
}

void Backend::setClip(int clip, const QString &name, const QVariant &value) {
    Scene scene = m_scene;
    QString error;
    if (!Project::setClip(scene, target(), clip, name, QJsonValue::fromVariant(value), &error))
        return setError(error);
    checkpoint();
    m_scene = scene;
    changed();
}

void Backend::removeClip(int clip) {
    Scene scene = m_scene;
    if (!Project::removeClip(scene, target(), clip))
        return;
    checkpoint();
    m_scene = scene;
    changed();
}

void Backend::moveKey(const QString &property, int index, double t, double v) {
    Layer *layer = m_scene.at(target());
    if (!layer || !layer->keys.contains(property) || !std::isfinite(t) || !std::isfinite(v))
        return;
    QList<Key> &keys = layer->keys[property];
    if (index < 0 || index >= keys.size())
        return;
    checkpoint();
    // A frame's room either side, so keys keep their order.
    const double frame = 1 / std::max(1.0, m_scene.fps);
    const double earliest = index > 0 ? keys.at(index - 1).t + frame : 0;
    const double latest = index + 1 < keys.size() ? keys.at(index + 1).t - frame : m_scene.duration;
    keys[index].t = std::clamp(t, earliest, std::max(earliest, latest));
    keys[index].v = v;
    changed();
}

QString Backend::filmPath() const {
    return hasProject() ? QFileInfo(m_directory).absoluteDir().filePath("film.json") : QString();
}

QJsonObject Backend::readFilm() const {
    QFile file(filmPath());
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

bool Backend::writeFilm(const QJsonObject &film) {
    QSaveFile file(filmPath());
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(film).toJson()) < 0 || !file.commit()) {
        setError(QString("Couldn't write %1.").arg(filmPath()));
        return false;
    }
    emit filmChanged();
    return true;
}

QVariantList Backend::filmShots() const {
    QVariantList rows;
    const QString here = QFileInfo(m_directory).fileName();
    for (const QJsonValue &value : readFilm().value("shots").toArray()) {
        QVariantMap row = value.toObject().toVariantMap();
        row["this"] = row.value("project").toString() == here;
        rows << row;
    }
    return rows;
}

void Backend::addToFilm() {
    if (!hasProject())
        return;
    QJsonObject film = readFilm();
    QJsonArray shots = film.value("shots").toArray();
    QJsonObject shot{{"project", QFileInfo(m_directory).fileName()}};
    if (!shots.isEmpty())
        shot["transition"] = "cut";
    shots.append(shot);
    film["shots"] = shots;
    if (!film.contains("fps"))
        film["fps"] = m_scene.fps;
    if (writeFilm(film))
        setNotice(QString("A shot of the film in %1, %2 of %3.").arg(filmPath()).arg(shots.size()).arg(shots.size()));
}

void Backend::setShot(int index, const QString &name, const QVariant &value) {
    QJsonObject film = readFilm();
    QJsonArray shots = film.value("shots").toArray();
    if (index < 0 || index >= shots.size())
        return;
    QJsonObject shot = shots.at(index).toObject();
    if (name == "transition") {
        if (!QStringList{"cut", "dissolve", "fade", "wipe"}.contains(value.toString()))
            return;
        shot["transition"] = value.toString();
        if (value.toString() != "cut" && !shot.contains("length"))
            shot["length"] = .5;
    } else if (name == "length" || name == "from" || name == "to") {
        bool ok = false;
        const double number = value.toDouble(&ok);
        if (!ok || number < 0)
            return;
        shot[name] = number;
    } else {
        return;
    }
    shots[index] = shot;
    film["shots"] = shots;
    writeFilm(film);
}

void Backend::moveShot(int index, int direction) {
    QJsonObject film = readFilm();
    QJsonArray shots = film.value("shots").toArray();
    const int to = index + direction;
    if (index < 0 || index >= shots.size() || to < 0 || to >= shots.size())
        return;
    const QJsonValue moving = shots.at(index);
    shots.removeAt(index);
    shots.insert(to, moving);
    film["shots"] = shots;
    writeFilm(film);
}

void Backend::removeShot(int index) {
    QJsonObject film = readFilm();
    QJsonArray shots = film.value("shots").toArray();
    if (index < 0 || index >= shots.size())
        return;
    shots.removeAt(index);
    film["shots"] = shots;
    writeFilm(film);
}

void Backend::exportFilm() {
    if (!hasProject() || m_exporter.busy())
        return;
    // Every shot as saved: this one too.
    flush();
    auto film = std::make_shared<Film>();
    QString error;
    if (!film->load(filmPath(), &error))
        return setError(error);
    m_film = film;
    const QDir renders(QDir(m_directory).filePath("renders"));
    QDir().mkpath(renders.path());
    m_filmOutput = renders.filePath("film.mp4");
    QSize size = film->size();
    size = QSize(std::max(2, size.width() - size.width() % 2), std::max(2, size.height() - size.height() % 2));
    const Exporter::Source source = [film](int frame, const QSize &at) { return film->frame(frame / film->fps(), at); };
    if (!m_exporter.startFrames(source, film->frameCount(), film->fps(), size, m_filmOutput))
        return setError(m_exporter.error());
    m_exportProgress = 0;
    emit exportChanged();
}

bool Backend::previewFilm() {
    if (!hasProject())
        return false;
    flush();
    QFile file(filmPath());
    if (!file.open(QIODevice::ReadOnly))
        return false;
    QString from = QString::fromUtf8(file.readAll());
    const QDir here = QFileInfo(filmPath()).absoluteDir();
    for (const QJsonValue &shot : readFilm().value("shots").toArray())
        from += "|" + QString::number(QFileInfo(here.filePath(shot.toObject().value("project").toString() + "/scene.json")).lastModified().toMSecsSinceEpoch());
    if (m_preview && from == m_previewMadeFrom)
        return true;
    auto film = std::make_shared<Film>();
    QString error;
    if (!film->load(filmPath(), &error)) {
        setError(error);
        return false;
    }
    m_preview = film;
    m_previewMadeFrom = from;
    return true;
}

double Backend::filmDuration() const { return m_preview ? m_preview->duration() : 0; }

void Backend::drawFilm(double t) {
    // A frame a turn of the event loop, on this thread, as exports draw: a
    // world's 3D can't be drawn from another. Asked again while one waits,
    // only the latest is drawn.
    m_filmWanted = t;
    if (m_filmDrawing)
        return;
    m_filmDrawing = true;
    QTimer::singleShot(0, this, [this] {
        m_filmDrawing = false;
        if (!m_preview || m_filmWanted < 0)
            return;
        const double at = m_filmWanted;
        m_filmWanted = -1;
        m_filmPicture = m_preview->frame(at, m_preview->size().scaled(QSize(640, 640), Qt::KeepAspectRatio));
        ++m_filmPictures;
        emit filmPreviewChanged();
    });
}

void Backend::showFilmAt(double t) {
    if (!previewFilm())
        return;
    m_filmTime = std::clamp(t, 0.0, m_preview->duration());
    emit filmPreviewChanged();
    drawFilm(m_filmTime);
}

void Backend::toggleFilmPlay() {
    if (m_filmTimer.isActive())
        return stopFilmPreview();
    if (!previewFilm())
        return;
    // From the start again when it had played to the end.
    if (m_filmTime >= m_preview->duration() - 1e-3)
        m_filmTime = 0;
    m_filmFrom = m_filmTime;
    m_filmClock.start();
    m_filmTimer.start();
    emit filmPreviewChanged();
}

void Backend::stopFilmPreview() {
    m_filmTimer.stop();
    emit filmPreviewChanged();
}

QString Backend::agentProgram() {
    const QString chosen = qEnvironmentVariable("OMOTION_AGENT");
    if (!chosen.isEmpty())
        return QFileInfo(chosen).isExecutable() ? chosen : QStandardPaths::findExecutable(chosen);
    // Started from the launcher, the PATH may not have where Claude Code
    // lives; its usual places are looked in too.
    const QString home = QDir::homePath();
    const QString found = QStandardPaths::findExecutable("claude");
    if (!found.isEmpty())
        return found;
    return QStandardPaths::findExecutable("claude", {home + "/.local/bin", home + "/.claude/local",
                                                     home + "/.local/share/mise/shims",
                                                     home + "/.local/share/mise/command-wrappers/bin", "/usr/local/bin"});
}

QVariantList Backend::rankCommands(const QString &query, const QStringList &names) const {
    QVariantList order;
    for (const int index : Project::rankMatches(query, names))
        order << index;
    return order;
}

void Backend::askAgent(const QString &request) {
    if (!hasProject() || m_agent || request.trimmed().isEmpty())
        return;
    const QString program = agentProgram();
    if (program.isEmpty())
        return setError("Saying what you want needs Claude Code: install it (claude.com/claude-code) and sign in, "
                        "or point OMOTION_AGENT at another agent.");
    if (m_playing)
        togglePlay();
    // What's on screen goes to disk first: the agent works from the file.
    flush();
    m_agentBefore = m_scene;
    m_agentUndoMark = int(m_undo.size());
    m_agentReview = false;
    m_agentCancelled = false;
    m_agentReply.clear();
    m_agentChanges.clear();
    const QString omotion = QCoreApplication::applicationFilePath();
    const QString selected = m_cameraSelected ? QString("the camera")
                             : m_selected >= 0  ? QString("the layer \"%1\"").arg(m_scene.layers.at(m_selected).name)
                                                : QString("nothing");
    const QString context =
        QString("You are directing an omotion animation project: the current folder. scene.json is the truth; "
                "the omotion window has it open and shows every change to it as it's saved. Work through the "
                "omotion command line, %1 (run \"%1 --help\" for all it does, and \"%1 info .\" for the scene "
                "with each layer's place), or by editing scene.json, keeping it valid JSON and every field you "
                "don't mean to change. Stay in this folder. The person is looking at %2, with the playhead at "
                "%3 s of %4 s. When you're done, reply with one short sentence on what you changed.")
            .arg(omotion, selected)
            .arg(m_time, 0, 'f', 2)
            .arg(m_scene.duration, 0, 'f', 2);
    m_agent = new QProcess(this);
    m_agent->setWorkingDirectory(m_directory);
    m_agent->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_agent, &QProcess::finished, this, &Backend::agentFinished);
    connect(m_agent, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            agentFinished();
    });
    m_agent->start(program, {"-p", request, "--append-system-prompt", context, "--permission-mode", "acceptEdits",
                             "--allowedTools", QString("Bash(%1:*)").arg(omotion), "Read", "Edit", "Write", "Glob", "Grep"});
    setBusy("Claude is on it: " + request.left(80) + (request.size() > 80 ? "…" : "") + "   ·   Esc stops");
    emit agentChanged();
}

void Backend::cancelAgent() {
    if (!m_agent)
        return;
    m_agentCancelled = true;
    m_agent->kill();
}

void Backend::agentFinished() {
    if (!m_agent)
        return;
    QProcess *agent = m_agent;
    m_agent = nullptr;
    const bool started = agent->error() != QProcess::FailedToStart;
    const QString reply = QString::fromUtf8(agent->readAllStandardOutput()).trimmed();
    const QString errors = QString::fromUtf8(agent->readAllStandardError()).trimmed();
    const bool ok = started && agent->exitStatus() == QProcess::NormalExit && agent->exitCode() == 0;
    agent->deleteLater();
    setBusy({});
    // Whatever it saved last is read now, not when the watcher gets to it.
    m_reloadTimer.stop();
    reloadFromDisk();
    // Everything it did is one step back to how it was.
    const QJsonObject before = m_agentBefore.toJson();
    while (m_undo.size() > m_agentUndoMark)
        m_undo.removeLast();
    const bool changedAnything = m_scene.toJson() != before;
    if (m_agentCancelled || !ok) {
        if (changedAnything) {
            m_scene = m_agentBefore;
            changed();
        }
        emit historyChanged();
        emit agentChanged();
        if (!m_agentCancelled)
            setError("The agent stopped: " + (errors.isEmpty() ? reply.isEmpty() ? QString("it didn't start.") : reply : errors.left(400)));
        else
            setNotice("Stopped; nothing it did was kept.");
        return;
    }
    if (changedAnything) {
        m_undo.append(before);
        m_redo.clear();
    }
    emit historyChanged();
    m_agentReply = reply.isEmpty() ? QString("Done.") : reply;
    m_agentChanges = changedAnything ? Project::describeChanges(m_agentBefore, m_scene) : QStringList{"Nothing in the scene changed."};
    m_agentReview = true;
    emit agentChanged();
}

void Backend::keepAgent() {
    if (!m_agentReview)
        return;
    m_agentReview = false;
    emit agentChanged();
}

void Backend::takeBackAgent() {
    if (!m_agentReview)
        return;
    m_agentReview = false;
    if (Project::serialize(m_scene) != Project::serialize(m_agentBefore))
        undo();
    emit agentChanged();
}

void Backend::setDrawColour(const QString &colour) {
    const QColor c(colour.trimmed());
    if (!c.isValid())
        return setError(QString("%1 isn't a colour; try #ffffff.").arg(colour));
    m_drawColour = c;
    emit toolChanged();
}

void Backend::startDrawing() {
    if (!hasProject())
        return;
    if (m_tool == "draw")
        return setTool("move");
    // On the selected drawing, or a new one on top.
    if (m_selected < 0 || !m_scene.layers.at(m_selected).raw.contains("drawing")) {
        checkpoint();
        const int index = Project::addDrawing(m_scene, "drawing");
        changed();
        select(index);
    }
    setTool("draw");
    setNotice("Draw: drag on the canvas; the wheel sizes the pen, right-click takes back a stroke. Each frame you draw on is a drawing of its own, shown until the next.");
}

void Backend::beginStroke(double x, double y) {
    if (m_tool != "draw")
        return;
    m_cut = {QPointF(x, y)};
    m_stroking = true;
    emit toolChanged();
}

void Backend::strokeTo(double x, double y) {
    if (!m_stroking || m_cut.isEmpty() || QLineF(m_cut.last(), QPointF(x, y)).length() < 1.5)
        return;
    m_cut.append(QPointF(x, y));
    emit toolChanged();
}

void Backend::endStroke() {
    if (!m_stroking)
        return;
    m_stroking = false;
    Scene scene = m_scene;
    QString error;
    if (!Project::addStroke(scene, m_assets, m_selected, m_time, m_cut, m_drawColour, m_brushRadius / 4, &error)) {
        m_cut.clear();
        emit toolChanged();
        return setError(error);
    }
    checkpoint();
    m_scene = scene;
    m_cut.clear();
    changed();
    emit toolChanged();
}

void Backend::undoStroke() {
    Scene scene = m_scene;
    if (!Project::removeStroke(scene, m_selected, m_time))
        return;
    checkpoint();
    m_scene = scene;
    changed();
}

void Backend::trackAt(double x, double y) {
    if (m_selected < 0 || !m_scene.layers.at(m_selected).raw.contains("video"))
        return setNotice("Select a video layer to track a point on.");
    Scene scene = m_scene;
    QString error;
    setBusy("Tracking…");
    QCoreApplication::processEvents();
    const int follower = Project::track(scene, m_assets, m_selected, QPointF(x, y), m_time, m_scene.duration, &error);
    setBusy({});
    if (follower < 0)
        return setError(error);
    checkpoint();
    m_scene = scene;
    setTool("move");
    changed();
    select(follower);
    setNotice(QString("%1 follows it. Parent a layer to it (or link to it) to stick it to the footage.").arg(m_scene.layers.at(follower).name));
}

void Backend::addTrackPoint(double x, double y) {
    if (m_tool != "track")
        return;
    m_cut << QPointF(x, y);
    emit toolChanged();
}

void Backend::finishTrack(bool stabilize) {
    if (m_tool != "track" || m_selected < 0 || !m_scene.layers.at(m_selected).raw.contains("video"))
        return setNotice("Select a video layer to track points on.");
    if (m_cut.isEmpty())
        return setNotice("Click points with some detail on the video first.");
    if (m_cut.size() == 1 && !stabilize)
        return trackAt(m_cut.first().x(), m_cut.first().y());
    Scene scene = m_scene;
    QString error;
    setBusy(stabilize ? "Holding the footage still…" : "Solving the footage's move…");
    QCoreApplication::processEvents();
    const int keyed = Project::solve(scene, m_assets, m_selected, m_cut, m_time, m_scene.duration, stabilize, &error);
    setBusy({});
    if (keyed < 0)
        return setError(error);
    checkpoint();
    m_scene = scene;
    const int points = int(m_cut.size());
    setTool("move");
    changed();
    select(keyed);
    setNotice(stabilize ? QString("The footage holds still from here, from %1 points: scale it up to hide its edges.").arg(points)
                        : QString("%1 moves, turns and grows as the footage does, from %2 points: parent layers to it to stick them on.")
                              .arg(m_scene.layers.at(keyed).name)
                              .arg(points));
}

void Backend::setCloth(const QString &name, const QVariant &value) {
    if (m_selected < 0)
        return;
    QJsonObject cloth = m_scene.layers.at(m_selected).raw.value("cloth").toObject();
    if (name == "on")
        cloth = value.toBool() ? QJsonObject{{"pin", "left"}, {"wind", 300}, {"gust", .5}} : QJsonObject();
    else if (cloth.isEmpty())
        return;
    else
        cloth[name] = QJsonValue::fromVariant(value);
    Scene scene = m_scene;
    QString error;
    if (!Cloth::set(scene, m_selected, cloth, &error))
        return setError(error);
    checkpoint();
    m_scene = scene;
    changed();
}

void Backend::solveCamera() {
    if (m_tool != "track" || m_selected < 0 || !m_scene.layers.at(m_selected).raw.contains("video"))
        return setNotice("Select a video layer to track points on.");
    Scene scene = m_scene;
    QString error;
    double residual = 0;
    setBusy("Solving the camera…");
    QCoreApplication::processEvents();
    const bool solved = Project::solveCamera(scene, m_assets, m_selected, m_cut, m_time, m_scene.duration, &error, &residual);
    setBusy({});
    if (!solved)
        return setError(error);
    checkpoint();
    m_scene = scene;
    setTool("move");
    changed();
    selectCamera();
    setNotice(QString("The camera moves as the footage's did, from here to the end (the points %1 px off on average): what you add to the world stays put in it.")
                  .arg(residual, 0, 'f', 1));
}

void Backend::setWarp(const QString &name, double value) {
    Scene scene = m_scene;
    QString error;
    if (!Project::setWarp(scene, m_selected, name, value, &error))
        return setError(error);
    checkpoint();
    m_scene = scene;
    changed();
}

void Backend::setVideo(const QString &name, const QVariant &value) {
    Scene scene = m_scene;
    QString error;
    if (!Project::setVideo(scene, m_selected, name, value, &error))
        return setError(error);
    checkpoint();
    m_scene = scene;
    changed();
}

void Backend::addLight() {
    if (!World::info(m_scene).valid)
        return setNotice("Lights light a world: make one first (Shift+D).");
    checkpoint();
    // A little in front of the camera, up a bit, where it'll be seen.
    const World::Frame now = World::frame(m_scene, m_assets, m_time);
    const QVector3D origin = World::pictureToWorld(World::info(m_scene)).map(QVector3D(0, 0, 0));
    const QVector3D ahead = now.cameraRotation.rotatedVector(QVector3D(0, 0, -1));
    const QVector3D at = now.cameraPosition + ahead * 4 + QVector3D(0, 1, 0) - origin;
    const int index = Project::addLight(m_scene, "light", QVector3D(at.x(), at.y(), -at.z()), {});
    changed();
    select(index);
    setNotice("A light: move it with its arrows (or G), set its colour, power and reach in the inspector; its opacity dims it, and keys.");
}

void Backend::setModelSize(double meters) {
    if (m_selected < 0 || !m_scene.layers.at(m_selected).raw.contains("model") || !(meters > 0))
        return;
    checkpoint();
    QJsonObject model = m_scene.layers.at(m_selected).raw.value("model").toObject();
    model["size"] = meters;
    m_scene.layers[m_selected].raw["model"] = model;
    changed();
}

void Backend::setLightValue(const QString &name, const QVariant &value) {
    Scene scene = m_scene;
    QString error;
    if (!Project::setLight(scene, m_selected, name, value, &error))
        return setError(error);
    checkpoint();
    m_scene = scene;
    changed();
}

void Backend::setMaterial(const QString &name, double value) {
    Scene scene = m_scene;
    QString error;
    if (!Project::setMaterial(scene, m_selected, name, value, &error))
        return setError(error);
    checkpoint();
    m_scene = scene;
    changed();
}

void Backend::setShapeModifier(const QString &type, int amount) {
    Scene scene = m_scene;
    QString error;
    // Nothing's an amount of zero: that modifier goes.
    if (amount <= 0) {
        QJsonObject shape = scene.layers.value(m_selected).raw.value("shape").toObject();
        QJsonArray kept;
        for (const QJsonValue &m : shape.value("modifiers").toArray())
            if (m.toObject().value("type").toString() != type)
                kept.append(m);
        if (m_selected < 0 || shape.isEmpty())
            return;
        if (kept.isEmpty())
            shape.remove("modifiers");
        else
            shape["modifiers"] = kept;
        scene.layers[m_selected].raw["shape"] = shape;
    } else if (!World::setShapeModifier(scene, m_selected, type, amount, &error)) {
        return setError(error);
    }
    checkpoint();
    m_scene = scene;
    changed();
}

void Backend::rigSelected(const QString &type, const QString &other) {
    if (m_selected < 0)
        return;
    Scene scene = m_scene;
    QString error;
    if (!Project::rig(scene, m_assets, m_selected, type, other, &error))
        return setError(error);
    checkpoint();
    m_scene = scene;
    changed();
    setNotice(type == "ik" ? QString("Rigged: move \"%1 target\" and the arm follows.").arg(other) : QString("Rigged."));
}

void Backend::unrigSelected() {
    if (m_selected < 0)
        return;
    Scene scene = m_scene;
    if (!Project::unrig(scene, m_selected))
        return;
    checkpoint();
    m_scene = scene;
    changed();
}

void Backend::addEmpty() {
    if (!hasProject())
        return;
    checkpoint();
    // In the middle of what's in view.
    const QPointF middle = cameraTransform(m_scene, 1, m_time).inverted().map(QPointF(m_scene.width / 2.0, m_scene.height / 2.0));
    const int index = Project::addEmpty(m_scene, "empty", {}, middle);
    changed();
    select(index);
}

void Backend::finishPath(bool turn) {
    if (m_tool != "path" || m_selected < 0)
        return;
    if (m_cut.size() < 2)
        return setNotice("Click two points or more for the path.");
    QJsonArray points;
    for (const QPointF &p : m_cut)
        points.append(QJsonArray{p.x(), p.y()});
    Scene scene = m_scene;
    QString error;
    // From the playhead to the end of the scene.
    const QJsonObject options{{"points", points}, {"start", m_time}, {"end", m_scene.duration}, {"turn", turn}};
    if (!Project::rig(scene, m_assets, m_selected, "path", {}, &error, options))
        return setError(error);
    checkpoint();
    m_scene = scene;
    m_cut.clear();
    setTool("move");
    changed();
    setNotice("It follows the path from here to the end; its rig in the inspector takes it away.");
}

void Backend::setLook(const QString &name, double value) {
    Scene scene = m_scene;
    QString error;
    if (!Project::setLook(scene, name, value, &error))
        return setError(error);
    checkpoint();
    m_scene = scene;
    changed();
}

void Backend::setWorldValue(const QString &name, const QVariant &value) {
    if (!World::info(m_scene).valid)
        return;
    QJsonObject world = m_scene.raw.value("world").toObject();
    const double number = value.toDouble();
    if (name == "hazeColor" || name == "dust") {
        const QColor colour(value.toString().trimmed());
        if (!colour.isValid()) {
            setError(QString("%1 isn't a colour; try #d9a08e.").arg(value.toString()));
            return;
        }
        checkpoint();
        if (name == "dust") {
            world["dust"] = colour.name();
        } else {
            QJsonObject haze = world.value("haze").toObject();
            haze["color"] = colour.name();
            world["haze"] = haze;
        }
    } else if (name == "hazeDensity" && std::isfinite(number) && number >= 0) {
        checkpoint();
        QJsonObject haze = world.value("haze").toObject();
        haze["density"] = std::min(number, 1.0);
        world["haze"] = haze;
    } else if (name == "grain" && std::isfinite(number) && number >= 0) {
        checkpoint();
        world["grain"] = std::min(number, .5);
    } else if (name == "shutter" && std::isfinite(number) && number >= 0) {
        checkpoint();
        world["shutter"] = std::min(number, 360.0);
    } else {
        return;
    }
    m_scene.raw["world"] = world;
    changed();
}

namespace {
QVariantMap linkTexts(const Layer &layer) {
    QVariantMap map;
    for (auto it = layer.links.begin(); it != layer.links.end(); ++it)
        map.insert(it.key(), Scene::linkText(it.value()));
    return map;
}
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
        map.insert("links", linkTexts(camera));
        map.insert("keyEase", easeHere(camera));
        map.insert("keyCurve", QVariant::fromValue(Scene::curveOf(easeHere(camera))));
        map.insert("modifiers", camera.raw.value("modifiers").toObject().toVariantMap());
        map.insert("clips", camera.raw.value("clips").toArray().toVariantList());
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
    map.insert("links", linkTexts(layer));
    map.insert("inWorld", World::layers(m_scene).contains(m_selected) || layer.raw.contains("light") || layer.raw.contains("model"));
    map.insert("model", layer.raw.value("model").toObject().toVariantMap());
    map.insert("light", layer.raw.value("light").toObject().toVariantMap());
    map.insert("material", layer.raw.value("material").toObject().toVariantMap());
    map.insert("follow", followOf(m_scene, m_selected));
    if (!layer.particles.isEmpty()) {
        QJsonObject settings = Scene::particleDefaults(layer.particles.value("kind").toString());
        for (auto it = layer.particles.begin(); it != layer.particles.end(); ++it)
            settings.insert(it.key(), it.value());
        // An emitter's settings left out are what it draws with.
        if (!settings.value("from").toString().isEmpty()) {
            const QString kind = settings.value("kind").toString();
            const QJsonObject emitter{{"rate", 40}, {"life", 1.5}, {"inherit", .5}, {"wind", 0},
                                      {"gravity", kind == "rain" ? 900 : kind == "embers" ? -120 : kind == "dust" ? -20 : 80}};
            for (auto it = emitter.begin(); it != emitter.end(); ++it)
                if (!settings.contains(it.key()))
                    settings.insert(it.key(), it.value());
        }
        map.insert("particles", settings.toVariantMap());
    }
    map.insert("ownFollow", !std::isnan(layer.follow));
    map.insert("empty", layer.raw.value("empty").toBool());
    map.insert("drawing", layer.raw.contains("drawing"));
    map.insert("video", layer.raw.value("video").toObject().toVariantMap());
    map.insert("warp", layer.raw.value("warp").toObject().toVariantMap());
    map.insert("cloth", layer.raw.value("cloth").toObject().toVariantMap());
    map.insert("shapeModifiers", layer.raw.value("shape").toObject().value("modifiers").toArray().toVariantList());
    map.insert("shaped", !layer.raw.value("shape").toObject().isEmpty());
    map.insert("modifiers", layer.raw.value("modifiers").toObject().toVariantMap());
    map.insert("body", layer.raw.value("body").toObject().toVariantMap());
    map.insert("clips", layer.raw.value("clips").toArray().toVariantList());
    // Its rig, in words: what it reaches for, looks at, rolls with, follows.
    QStringList rig;
    for (const QJsonValue &entry : layer.raw.value("constraints").toArray()) {
        const QJsonObject c = entry.toObject();
        const QString type = c.value("type").toString();
        if (type == "ik")
            rig << QString("Reaches with %1 for %2").arg(c.value("lower").toString(), c.value("target").toString());
        else if (type == "lookAt")
            rig << "Looks at " + c.value("target").toString();
        else if (type == "path")
            rig << QString("Follows a path from %1 s to %2 s").arg(c.value("start").toDouble()).arg(c.value("end").toDouble());
    }
    if (layer.raw.contains("wheel"))
        rig << "Rolls as " + layer.raw.value("wheel").toString() + " goes";
    map.insert("rig", rig);
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
    // A 3D model goes into the world, in front of the camera.
    if (Project::isModel(localPath(path))) {
        if (!World::info(m_scene).valid)
            return setError("A 3D model goes into a world: make one first (Shift+D)."), false;
        const World::Frame now = World::frame(m_scene, m_assets, m_time);
        const QVector3D origin = World::pictureToWorld(World::info(m_scene)).map(QVector3D(0, 0, 0));
        QVector3D ahead = now.cameraRotation.rotatedVector(QVector3D(0, 0, -1));
        ahead.setY(0);
        QVector3D at = now.cameraPosition + ahead.normalized() * 5 - origin;
        const double ground = World::groundAt(m_scene, m_assets, (origin + at).x(), (origin + at).z());
        at.setY(std::isnan(ground) ? 0.f : float(ground - origin.y()));
        const int index = Project::importModel(scene, m_directory, localPath(path), QVector3D(at.x(), at.y(), -at.z()), 1.5, &error);
        if (index < 0)
            return setError(error), false;
        checkpoint();
        m_scene = scene;
        changed();
        rearmWatcher();
        select(index);
        setNotice("Added " + m_scene.layers.at(index).name + ": move it with its arrows or G, turn it with its rings; its size in the inspector.");
        return true;
    }
    // A clip becomes a layer that plays; its frames take a moment to write.
    if (Project::isVideo(localPath(path)))
        setBusy("Reading the video's frames…");
    const int index = Project::isVideo(localPath(path)) ? Project::importVideo(scene, m_directory, localPath(path), &error)
                                                         : Project::importImage(scene, m_directory, localPath(path), &error);
    setBusy({});
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
    if (m_playing)
        syncAudio(true);
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
    syncAudio(true);
    emit playingChanged();
}

void Backend::tick() {
    m_time += m_clock.restart() / 1000.0;
    if (m_time >= m_scene.duration) {
        m_time = std::fmod(m_time, m_scene.duration);
        syncAudio(true);  // Round again: the sound goes back with it.
    }
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
    if (targetLayer()) {
        times = m_scene.keyTimes(target());
    } else {
        for (int i = 0; i < m_scene.layers.size(); ++i)
            times += m_scene.keyTimes(i);
        times += m_scene.keyTimes(Scene::CameraIndex);
    }
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
    const bool hadOthers = !m_also.isEmpty();
    m_also.clear();
    if (index == m_selected && !m_cameraSelected) {
        if (hadOthers)
            emit selectedChanged();
        return;
    }
    ++m_aiGeneration;
    m_selected = index;
    m_cameraSelected = false;
    emit selectedChanged();
    emit valuesChanged();
}

void Backend::selectAlso(int index) {
    if (index < 0 || index >= m_scene.layers.size())
        return;
    if (m_selected < 0 || m_cameraSelected)
        return select(index);
    if (index == m_selected) {
        // Out of the selection: the next one along leads it.
        if (m_also.isEmpty())
            return select(-1);
        m_selected = m_also.takeFirst();
    } else if (m_also.contains(index)) {
        m_also.removeAll(index);
    } else {
        m_also.append(index);
    }
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
    m_also.clear();
    m_cameraSelected = true;
    emit selectedChanged();
    emit valuesChanged();
    setNotice(World::info(m_scene).valid ? "Camera: drag to turn it, Shift+drag to move it, K keyframes it."
                                         : "Camera: drag the canvas to pan, the wheel zooms, [ ] turns, K keyframes it.");
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

int Backend::emptyNear(double x, double y, double reach) const {
    for (int i = int(m_scene.layers.size()) - 1; i >= 0; --i) {
        const Layer &layer = m_scene.layers.at(i);
        if (!layer.raw.value("empty").toBool() || !layer.visible)
            continue;
        const QPointF at = layerTransform(m_scene, const_cast<AssetCache &>(m_assets), i, m_time).map(QPointF());
        if (QLineF(at, QPointF(x, y)).length() <= reach)
            return i;
    }
    return -1;
}

void Backend::selectAt(double x, double y) {
    // Empties have nothing to click but their cross.
    const int empty = emptyNear(x, y, 12);
    select(empty >= 0 ? empty : layerAt(m_scene, m_assets, m_time, QPointF(x, y)));
}

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
    bool unlinked = m_scene.dropLinksTo(name) > 0;
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
    setNotice(unlinked ? QString("Deleted %1 and unlinked what was tied to it. Ctrl+Z brings it back.").arg(name)
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
    m_scene.renameLinkSource(old, name);
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

void Backend::addParticles(const QString &kind) {
    if (!hasProject())
        return;
    Scene scene = m_scene;
    QString error;
    const int index = Project::addParticles(scene, kind, {}, &error);
    if (index < 0)
        return setError(error);
    checkpoint();
    m_scene = scene;
    m_selected = index;
    m_cameraSelected = false;
    setKeySelection({});
    emit selectedChanged();
    changed();
    setNotice(QString("Added %1. Its settings are in the inspector; they loop with the scene.").arg(kind));
}

void Backend::setParticle(const QString &setting, const QVariant &value) {
    if (m_selected < 0 || m_scene.layers.at(m_selected).particles.isEmpty())
        return;
    QJsonValue json;
    QString error;
    if (!Project::particleSetting(setting, value, &json, &error))
        return setError(error);
    checkpoint();
    m_scene.layers[m_selected].particles.insert(setting, json);
    changed();
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

bool Backend::setLink(const QString &name, const QString &text) {
    const QString property = propertyFor(name);
    const Layer *layer = targetLayer();
    if (!layer)
        return false;
    Link link;
    QString error;
    if (!text.trimmed().isEmpty() && !Scene::parseLink(text.trimmed(), &link, &error)) {
        setError(error);
        return false;
    }
    const Link old = layer->links.value(property);
    if (link.from == old.from && Scene::linkText(link) == Scene::linkText(old))
        return true;
    // Fields the text doesn't cover, an agent's notes say, stay with the link.
    if (!link.from.isEmpty())
        link.raw = old.raw;
    // The property keeps looking as it did at the playhead: its own value
    // gives up what the new link adds, so linking doesn't make it jump.
    const double before = m_scene.value(*layer, property, m_time);
    Scene edited = m_scene;
    if (!edited.setLink(target(), property, link, &error)) {
        setError(error);
        return false;
    }
    checkpoint();
    m_scene = edited;
    if (!m_scene.at(target())->keys.contains(property))
        m_scene.setValue(target(), property, m_time, before);
    changed();
    setNotice(link.from.isEmpty() ? QString("Unlinked %1").arg(name)
                                  : QString("%1 follows %2").arg(name, Scene::linkText(link)));
    return true;
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

bool Backend::setEase(const QString &wanted) {
    const QString ease = wanted.simplified().remove(' ');
    const QList<KeyRef> refs = targetKeys();
    if (refs.isEmpty()) {
        setNotice("No keyframes at the playhead to ease.");
        return false;
    }
    if (!Scene::validEase(ease)) {
        setError(QString("\"%1\" isn't an easing; use one of %2, spring(n), bounce(n), arc(h), or cubic(x1,y1,x2,y2).")
                     .arg(wanted, Scene::eases().join(", ")));
        return false;
    }
    checkpoint();
    for (const KeyRef &ref : refs)
        m_scene.setEaseAt(ref.layer, ref.t, ease);
    changed();
    setNotice("Easing out of these keyframes: " + ease);
    return true;
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
    } else if (name == "vanishX" || name == "vanishY") {
        const double v = value.toDouble();
        QPointF point = scene.vanishPoint();
        (name == "vanishX" ? point.rx() : point.ry()) = v;
        scene.vanish = point;
    } else if (name == "distance") {
        const double v = value.toDouble();
        if (!(v > 0))
            return setError("The perspective distance must be more than 0 pixels.");
        scene.distance = v;
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
    pasteClip(m_clipboard);
}

void Backend::pasteClip(QList<KeyClip> clip) {
    if (!targetLayer())
        return setNotice("Select the layer to paste onto.");
    // Scene::pasteKeys keeps only what the target can animate.
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

void Backend::beginDrag(double x, double y, bool along) {
    if (!targetLayer())
        return;
    beginGesture();
    const Layer &layer = *targetLayer();
    m_dragStart = QPointF(x, y);
    m_dragOrigin = QPointF(m_scene.value(layer, "x", m_time), m_scene.value(layer, "y", m_time));
    m_dragOrigins.clear();
    for (const int other : m_also)
        if (other >= 0 && other < m_scene.layers.size())
            m_dragOrigins.insert(other, QPointF(m_scene.value(m_scene.layers.at(other), "x", m_time),
                                               m_scene.value(m_scene.layers.at(other), "y", m_time)));
    m_dragging = true;
    // In a world, the camera and the world's pieces move in 3D.
    m_worldDrag = World::info(m_scene).valid && (m_cameraSelected || World::layers(m_scene).contains(m_selected));
    m_dragAlong = along;
    if (m_worldDrag) {
        for (const QString &property : Scene::worldProperties())
            m_worldOrigin[property] = m_scene.value(layer, property, m_time);
        const World::Frame frame = World::frame(m_scene, m_assets, m_time);
        m_dragFocal = m_scene.height / 2.0 / std::tan(frame.fov / 2 * M_PI / 180);
        m_dragDistance = 1;
        if (!m_cameraSelected) {
            const QVector3D at = World::pivot(m_scene, m_assets, m_selected) +
                                 QVector3D(m_worldOrigin["worldX"], m_worldOrigin["worldY"], -m_worldOrigin["worldZ"]);
            m_dragDistance = std::max(.1f, (at - frame.cameraPosition).length());
        }
    }
}

void Backend::dragTo(double x, double y) {
    if (!m_dragging || !targetLayer())
        return;
    if (m_worldDrag) {
        const QPointF moved = QPointF(x, y) - m_dragStart;
        const int target = this->target();
        auto set = [&](const QString &property, double v) { m_scene.setValue(target, property, m_time, v); };
        const World::Frame frame = World::frame(m_scene, m_assets, m_time);
        QVector3D right = frame.cameraRotation.rotatedVector(QVector3D(1, 0, 0)), ahead = frame.cameraRotation.rotatedVector(QVector3D(0, 0, -1));
        right.setY(0), ahead.setY(0);
        right.normalize(), ahead.normalize();
        if (m_cameraSelected && !m_dragAlong) {
            // Turning: the picture follows the hand, so the camera turns the
            // other way, a pixel's worth of view for every pixel.
            const double perPixel = std::atan(1 / m_dragFocal) * 180 / M_PI;
            set("yaw", m_worldOrigin["yaw"] - moved.x() * perPixel);
            set("pitch", m_worldOrigin["pitch"] + moved.y() * perPixel);
        } else {
            // Moving: across and up at its distance, or with Shift, up and
            // down the screen is nearer and farther. The camera moves the
            // other way to what's in sight, as the picture follows the hand.
            const double meters = m_cameraSelected ? -1 / 200.0 : m_dragDistance / m_dragFocal;
            QVector3D shift;
            if (m_dragAlong)
                shift = right * float(moved.x() * meters) + ahead * float(-moved.y() * meters * 3);
            else
                shift = right * float(moved.x() * meters) + QVector3D(0, float(-moved.y() * meters), 0);
            set("worldX", m_worldOrigin["worldX"] + shift.x());
            set("worldY", m_worldOrigin["worldY"] + shift.y());
            set("worldZ", m_worldOrigin["worldZ"] - shift.z());
        }
        changed();
        return;
    }
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
    // The others selected go along, each through its own parent.
    for (const int other : m_also) {
        if (other < 0 || other >= m_scene.layers.size() || !m_dragOrigins.contains(other))
            continue;
        bool ok = true;
        const QTransform back = parentTransform(m_scene, m_assets, other, m_time).inverted(&ok);
        if (!ok)
            continue;
        const QPointF delta = back.map(QPointF(x, y)) - back.map(m_dragStart);
        m_scene.setValue(other, "x", m_time, m_dragOrigins.value(other).x() + delta.x());
        m_scene.setValue(other, "y", m_time, m_dragOrigins.value(other).y() + delta.y());
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
    if (tool != "move" && tool != "cut" && tool != "pivot" && tool != "wand" && tool != "brush" && tool != "pin" && tool != "path" &&
        tool != "draw" && tool != "track")
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
    if ((tool == "cut" || tool == "wand" || tool == "brush" || tool == "pin") && !m_scene.layers.at(m_selected).particles.isEmpty()) {
        setNotice("A particle layer has no picture to cut; select a picture layer.");
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

QVariantMap Backend::audio() const {
    const QJsonObject audio = m_scene.raw.value("audio").toObject();
    if (audio.value("source").toString().isEmpty())
        return {};
    QVariantList peaks;
    for (double peak : m_peaks)
        peaks.append(peak);
    return {{"source", audio.value("source").toString()},
            {"name", QFileInfo(audio.value("source").toString()).fileName()},
            {"offset", audio.value("offset").toDouble()},
            {"volume", audio.value("volume").toDouble(1)},
            {"peaks", peaks}};
}

void Backend::syncAudio(bool reseek) {
    const QJsonObject audio = m_scene.raw.value("audio").toObject();
    const QString source = audio.value("source").toString();
    const QString path = source.isEmpty() ? QString() : QDir(m_directory).absoluteFilePath(source);
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        if (m_player)
            m_player->stop();
        return;
    }
    if (!m_player) {
        m_player = new QMediaPlayer(this);
        m_audioOutput = new QAudioOutput(this);
        m_player->setAudioOutput(m_audioOutput);
        // A seek made while a file is still loading is lost; make it once
        // the file's ready.
        connect(m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
            if (m_pendingSeek >= 0 && (status == QMediaPlayer::LoadedMedia || status == QMediaPlayer::BufferedMedia)) {
                m_player->setPosition(m_pendingSeek);
                m_pendingSeek = -1;
                if (m_playing)
                    m_player->play();
            }
        });
    }
    const qint64 position = qint64((m_time + audio.value("offset").toDouble()) * 1000);
    if (m_playerSource != path) {
        m_player->setSource(QUrl::fromLocalFile(path));
        m_playerSource = path;
        m_pendingSeek = position;
        reseek = false;
    }
    m_audioOutput->setVolume(float(std::clamp(audio.value("volume").toDouble(1), 0.0, 1.0)));
    if (reseek) {
        if (m_pendingSeek >= 0)
            m_pendingSeek = position;
        else
            m_player->setPosition(position);
    }
    if (m_playing)
        m_player->play();
    else
        m_player->pause();
}

void Backend::importAudioDialog() {
    if (!hasProject())
        return;
    if (m_picker)
        m_picker->choose(FilePicker::Purpose::ImportAudio);
    else
        setError("There's no file chooser available.");
}

bool Backend::setAudioFile(const QString &path) {
    if (!hasProject())
        return false;
    Scene scene = m_scene;
    QString error;
    if (!Project::setAudio(scene, m_directory, localPath(path), &error)) {
        setError(error);
        return false;
    }
    checkpoint();
    m_scene = scene;
    changed();
    setNotice("Added the sound. It plays along, shows on the timeline, and goes into MP4 and WebM.");
    return true;
}

void Backend::setAudioValue(const QString &name, double value) {
    QJsonObject audio = m_scene.raw.value("audio").toObject();
    if (audio.isEmpty() || (name != "offset" && name != "volume") || !std::isfinite(value))
        return;
    checkpoint();
    audio[name] = name == "offset" ? std::max(0.0, value) : std::clamp(value, 0.0, 4.0);
    m_scene.raw["audio"] = audio;
    changed();
    syncAudio(name == "offset");
}

void Backend::removeAudio() {
    if (!m_scene.raw.contains("audio"))
        return;
    checkpoint();
    m_scene.raw.remove("audio");
    changed();
    syncAudio(false);
    setNotice("Took the sound away. Ctrl+Z brings it back.");
}

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

QVariantList Backend::pinPoints() const {
    QVariantList points;
    if (m_selected < 0 || m_selected >= m_scene.layers.size())
        return points;
    const Layer &layer = m_scene.layers.at(m_selected);
    if (layer.pins.isEmpty())
        return points;
    AssetCache &assets = const_cast<AssetCache &>(m_assets);
    const QTransform toCanvas = layerTransform(m_scene, assets, m_selected, m_time);
    for (const QPointF &pin : layer.pins)
        points.append(toCanvas.map(bend(m_scene, layer, pin, m_time)));
    return points;
}

int Backend::pinNear(double x, double y, double radius) const {
    const QVariantList points = pinPoints();
    int best = -1;
    double bestDistance = radius;
    for (int i = 0; i < points.size(); ++i) {
        const double d = QLineF(points.at(i).toPointF(), QPointF(x, y)).length();
        if (d <= bestDistance) {
            best = i;
            bestDistance = d;
        }
    }
    return best;
}

int Backend::addPinAt(double x, double y) {
    if (m_tool != "pin" || m_selected < 0)
        return -1;
    bool invertible = false;
    const QTransform toImage = layerTransform(m_scene, m_assets, m_selected, m_time).inverted(&invertible);
    if (!invertible)
        return -1;
    // The point of the image the bend has put under the mouse: the pin goes
    // there, already offset to where it's bent to, so nothing moves.
    const Layer &layer = m_scene.layers.at(m_selected);
    const QPointF bent = toImage.map(QPointF(x, y));
    const QPointF at = unbend(m_scene, layer, bent, m_time);
    const QSizeF size = m_assets.size(layer.source);
    if (!QRectF(QPointF(0, 0), size).contains(at)) {
        setNotice("Pins go on the layer; click on it.");
        return -1;
    }
    checkpoint();
    Layer &edited = m_scene.layers[m_selected];
    const int pin = int(edited.pins.size());
    edited.pins.append(at);
    const bool keyed = std::any_of(edited.keys.keyBegin(), edited.keys.keyEnd(),
                                   [](const QString &key) { return key.startsWith("pin"); });
    for (const auto &[axis, offset] : {std::pair{QString("x"), bent.x() - at.x()}, std::pair{QString("y"), bent.y() - at.y()}}) {
        const QString property = QString("pin%1%2").arg(pin).arg(axis);
        edited.base[property] = std::abs(offset) < 1e-6 ? 0 : offset;
        // Among keyed pins, the new one is keyed here too.
        if (keyed)
            m_scene.keyProperty(m_selected, property, m_time);
    }
    changed();
    const int count = int(m_scene.layers.at(m_selected).pins.size());
    setNotice(count == 1 ? "Pinned. Add another, then drag one to bend between them." : QString("%1 pins. Drag one to bend.").arg(count));
    return count - 1;
}

void Backend::beginPinDrag(int pin, double x, double y) {
    if (m_selected < 0 || pin < 0 || pin >= m_scene.layers.at(m_selected).pins.size())
        return;
    beginGesture();
    const Layer &layer = m_scene.layers.at(m_selected);
    m_pinDragging = pin;
    m_pinDragStart = QPointF(x, y);
    m_pinOffsetStart = QPointF(m_scene.value(layer, QString("pin%1x").arg(pin), m_time),
                               m_scene.value(layer, QString("pin%1y").arg(pin), m_time));
}

void Backend::pinDragTo(double x, double y) {
    if (m_pinDragging < 0 || m_selected < 0)
        return;
    // The mouse's movement, turned back into the image's own pixels.
    bool invertible = false;
    const QTransform toImage = layerTransform(m_scene, m_assets, m_selected, m_time).inverted(&invertible);
    if (!invertible)
        return;
    const QPointF moved = toImage.map(QPointF(x, y)) - toImage.map(m_pinDragStart);
    m_scene.setValue(m_selected, QString("pin%1x").arg(m_pinDragging), m_time, m_pinOffsetStart.x() + moved.x());
    m_scene.setValue(m_selected, QString("pin%1y").arg(m_pinDragging), m_time, m_pinOffsetStart.y() + moved.y());
    changed();
}

void Backend::endPinDrag() {
    m_pinDragging = -1;
    endGesture();
}

void Backend::removePin(int pin) {
    if (m_selected < 0 || pin < 0 || pin >= m_scene.layers.at(m_selected).pins.size())
        return;
    checkpoint();
    Layer &layer = m_scene.layers[m_selected];
    layer.pins.removeAt(pin);
    // The pins after it move down a number, and their offsets with them.
    for (int i = pin; i <= layer.pins.size(); ++i)
        for (const QString &axis : {QString("x"), QString("y")}) {
            const QString from = QString("pin%1%2").arg(i + 1).arg(axis), to = QString("pin%1%2").arg(i).arg(axis);
            layer.base.insert(to, layer.base.value(from, 0));
            if (layer.keys.contains(from))
                layer.keys.insert(to, layer.keys.take(from));
            else
                layer.keys.remove(to);
            layer.base.remove(from);
        }
    changed();
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
    if (!m_scene.layers.at(layer).particles.isEmpty())
        return setNotice("A particle layer has no picture to split; select a picture layer.");
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

void Backend::worldJob(const QString &busy, const QString &done, std::function<bool(Scene &, AssetCache &, QString *)> work) {
    if (!hasProject() || m_job.isRunning())
        return;
    if (!needAi())
        return;
    if (m_playing)
        togglePlay();
    const Scene scene = m_scene;
    const QString directory = m_directory;
    const QByteArray started = Project::serialize(m_scene);
    auto result = std::make_shared<std::pair<Scene, QString>>();
    setBusy(busy);
    m_job.disconnect(this);
    connect(&m_job, &QFutureWatcher<void>::finished, this, [this, result, started, done] {
        setBusy({});
        m_workAssets.clear();
        const auto &[edited, error] = *result;
        if (!error.isEmpty())
            return setError(error);
        if (Project::serialize(m_scene) != started)
            return setError("The scene changed meanwhile, so that wasn't kept; try again.");
        checkpoint();
        m_scene = edited;
        changed();
        rearmWatcher();
        setNotice(done);
    });
    m_job.setFuture(QtConcurrent::run([this, scene, directory, result, work] {
        m_workAssets.setDirectory(directory);
        Scene edited = scene;
        QString error;
        if (!work(edited, m_workAssets, &error)) {
            result->second = error.isEmpty() ? QString("That didn't work.") : error;
            return;
        }
        result->first = edited;
    }));
}

void Backend::makeWorld(double horizon) {
    const int layer = m_selected >= 0 && m_scene.layers.at(m_selected).particles.isEmpty() ? m_selected : 0;
    worldJob("Reading the picture in 3D… (a minute or two the first time)",
             "The picture is a world now. V and drag turns the camera; Shift+drag moves it. S gives a piece a shape.",
             [layer, horizon](Scene &scene, AssetCache &assets, QString *error) {
                 return World::build(scene, assets, layer, error, horizon);
             });
}

void Backend::shapeSelected() {
    if (!World::info(m_scene).valid)
        return setNotice("Make the world first (Shift+D).");
    const int layer = m_selected;
    if (layer < 0 || !World::layers(m_scene).contains(layer) || layer == m_scene.indexOf(World::info(m_scene).picture))
        return setNotice("Select a piece cut out of the world's picture to give it a shape.");
    worldJob("Giving it a 3D shape… (about 20 seconds)", "It has a shape: it can turn, and casts a shadow.",
             [layer](Scene &scene, AssetCache &assets, QString *error) { return World::shapeObject(scene, assets, layer, error); });
}

void Backend::setRealSize(double meters) {
    Scene scene = m_scene;
    QString error;
    if (!World::setSize(scene, m_assets, m_selected, meters, &error))
        return setError(error);
    checkpoint();
    m_scene = scene;
    changed();
    setNotice(QString("The world is to scale: %1 is %2 m tall.").arg(m_scene.layers.at(m_selected).name).arg(meters));
}

void Backend::driveSelected(double fromX, double fromAhead, double toX, double toAhead, double kmh, bool face) {
    Scene scene = m_scene;
    QString error;
    QList<double> landings;
    if (!World::drive(scene, m_assets, m_selected, QPointF(fromX, fromAhead), QPointF(toX, toAhead), kmh / 3.6, m_time,
                      &landings, &error, face))
        return setError(error);
    checkpoint();
    m_scene = scene;
    changed();
    QStringList times;
    for (const double at : landings)
        times << QString::number(at, 'f', 2);
    setNotice(QString("Driven at %1 km/h%2. Space plays it.").arg(kmh).arg(times.isEmpty() ? QString() : ", landing at " + times.join(", ") + " s"));
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

void Backend::saveMotion(const QString &name) {
    QList<KeyRef> refs = m_keySelection;
    if (refs.isEmpty() && targetLayer())
        for (double t : m_scene.keyTimes(target()))
            refs.append(KeyRef{target(), t});
    QString error;
    if (!Project::saveMotion(name, m_scene.copyKeys(refs), &error))
        return setError(error);
    setNotice(QString("Saved the motion \"%1\". It's in N, for any layer in any project.").arg(name.trimmed()));
}

void Backend::applyMotion(const QString &name) {
    if (!targetLayer())
        return setNotice("Select the layer to move first.");
    QString error;
    const QList<KeyClip> keys = Project::loadMotion(name, &error);
    if (keys.isEmpty())
        return setError(error);
    pasteClip(keys);
    setNotice(QString("Put on \"%1\" from the playhead.").arg(name));
}

QStringList Backend::savedMotions() const { return Project::savedMotions(); }

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
    // The AI's fill takes seconds whatever the size, and redrawing the source
    // redraws every piece of it, so any AI fill there goes in the background.
    bool aiThere = fill == 2;
    if (from >= 0)
        for (const Layer &layer : m_scene.layers) {
            const QJsonObject cut = layer.raw.value("cut").toObject();
            aiThere |= cut.value("from").toString() == m_scene.layers.at(from).name && cut.value("fillWith").toString() == "ai";
        }
    if (size.width() * size.height() <= 4e6 && !aiThere) {
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

// ---- The 3D view ------------------------------------------------------------

World::View Backend::view() const {
    if (m_freeView) {
        const QQuaternion turn = QQuaternion::fromAxisAndAngle(0, 1, 0, float(-m_orbitYaw)) *
                                 QQuaternion::fromAxisAndAngle(1, 0, 0, float(m_orbitPitch));
        return World::View{m_orbitTarget + turn.rotatedVector(QVector3D(0, 0, float(m_orbitDistance))), turn, 50};
    }
    // The shot's camera; the assets are only read through their caches.
    return World::cameraView(World::frame(m_scene, const_cast<AssetCache &>(m_assets), m_time));
}

QSizeF Backend::viewSize() const { return m_freeView ? m_viewportSize : QSizeF(m_scene.width, m_scene.height); }

void Backend::syncView() {
    m_world.setView(m_freeView, m_freeView ? view() : World::View{});
}

void Backend::setViewportSize(double width, double height) {
    if (width < 1 || height < 1 || QSizeF(width, height) == m_viewportSize)
        return;
    m_viewportSize = QSizeF(width, height);
    if (m_freeView)
        emit viewChanged();
}

void Backend::toggleFreeView() {
    if (!m_freeView && !World::info(m_scene).valid) {
        setNotice("The free view looks around a world: make one first (Shift+D).");
        return;
    }
    m_freeView = !m_freeView;
    if (m_freeView) {
        // It starts where the shot's camera is, looking where it looks, at
        // what's selected or ten meters ahead.
        const World::View shot = World::cameraView(World::frame(m_scene, m_assets, m_time));
        const QVector3D ahead = shot.rotation.rotatedVector(QVector3D(0, 0, -1));
        QList<QVector3D> corners;
        QVector3D pivot;
        m_orbitDistance = 10;
        if (m_selected >= 0 && World::box(m_scene, m_assets, m_selected, m_time, &corners, &pivot))
            m_orbitDistance = std::clamp(double((pivot - shot.position).length()), 1.0, 200.0);
        m_orbitTarget = shot.position + ahead * float(m_orbitDistance);
        m_orbitYaw = std::atan2(ahead.x(), -ahead.z()) * 180 / M_PI;
        m_orbitPitch = std::asin(std::clamp(double(ahead.y()), -1.0, 1.0)) * 180 / M_PI;
        setNotice("Free view: middle-drag or Alt+drag orbits, Shift pans, the wheel comes closer, F looks at the selected, ` goes back to the camera.");
    }
    syncView();
    emit viewChanged();
}

void Backend::orbitView(double dx, double dy) {
    if (!m_freeView)
        return;
    m_orbitYaw += dx * .3;
    m_orbitPitch = std::clamp(m_orbitPitch - dy * .3, -89.0, 89.0);
    syncView();
    emit viewChanged();
}

void Backend::panView(double dx, double dy) {
    if (!m_freeView)
        return;
    // The point looked at follows the hand, a view pixel's worth of meters
    // at its distance for each pixel.
    const World::View v = view();
    const double meters = 2 * m_orbitDistance * std::tan(v.fov * M_PI / 360) / std::max(1.0, m_viewportSize.height());
    m_orbitTarget += v.rotation.rotatedVector(QVector3D(float(-dx * meters), float(dy * meters), 0));
    syncView();
    emit viewChanged();
}

void Backend::zoomView(double factor) {
    if (!m_freeView || !(factor > 0))
        return;
    m_orbitDistance = std::clamp(m_orbitDistance / factor, .2, 5000.0);
    syncView();
    emit viewChanged();
}

void Backend::frameSelected() {
    if (!m_freeView)
        return;
    QList<QVector3D> corners;
    QVector3D pivot;
    if (m_cameraSelected) {
        pivot = World::frame(m_scene, m_assets, m_time).cameraPosition;
        m_orbitDistance = 4;
    } else if (m_selected >= 0 && World::box(m_scene, m_assets, m_selected, m_time, &corners, &pivot)) {
        float size = 0;
        for (const QVector3D &corner : corners)
            size = std::max(size, (corner - pivot).length());
        m_orbitDistance = std::clamp(double(size) * 2.5, .5, 300.0);
    } else {
        return;
    }
    m_orbitTarget = pivot;
    syncView();
    emit viewChanged();
}

void Backend::pickAt(double x, double y, bool add) {
    int found = -1;
    if (!World::info(m_scene).valid || !m_freeView) {
        // What's drawn over the world is in front of it, in the shot.
        found = emptyNear(x, y, 12);
        if (found < 0)
            found = layerAt(m_scene, m_assets, m_time, QPointF(x, y),
                            World::info(m_scene).valid ? World::layers(m_scene) : QList<int>());
    }
    if (found < 0 && World::info(m_scene).valid)
        for (const int index : World::lightLayers(m_scene)) {
            QPointF at;
            if (World::project(view(), viewSize(), World::lightPosition(m_scene, index, m_time), &at) && QLineF(at, QPointF(x, y)).length() < 14)
                found = index;
        }
    if (found < 0 && World::info(m_scene).valid)
        found = World::pick(m_scene, m_assets, m_time, view(), viewSize(), QPointF(x, y));
    if (add) {
        if (found >= 0)
            selectAlso(found);
        return;
    }
    select(found);
}

QList<int> Backend::handleTargets() const {
    QList<int> list;
    const int main = handleTarget();
    if (main == -1)
        return list;
    list << main;
    if (main != Scene::CameraIndex)
        for (const int i : m_also)
            if (World::layers(m_scene).contains(i) || World::lightLayers(m_scene).contains(i) || World::modelLayers(m_scene).contains(i))
                list << i;
    return list;
}

int Backend::handleTarget() const {
    if (!World::info(m_scene).valid)
        return -1;
    if (m_cameraSelected)
        return m_freeView ? Scene::CameraIndex : -1;
    return World::layers(m_scene).contains(m_selected) || World::lightLayers(m_scene).contains(m_selected) ||
                   World::modelLayers(m_scene).contains(m_selected)
               ? m_selected
               : -1;
}

Backend::Gizmo Backend::gizmo() const {
    Gizmo gizmo;
    if (!World::info(m_scene).valid || m_playing)
        return gizmo;
    AssetCache &assets = const_cast<AssetCache &>(m_assets);
    const World::View v = view();
    const QSizeF size = viewSize();
    const World::Frame now = World::frame(m_scene, assets, m_time);
    if (m_freeView) {
        // The shot's camera, as a little pyramid toward what it frames.
        const World::View shot = World::cameraView(now);
        const double tall = std::tan(shot.fov * M_PI / 360), wide = tall * m_scene.width / std::max(1, m_scene.height);
        const double reach = .6;
        QPointF eye;
        if (World::project(v, size, shot.position, &eye)) {
            gizmo.camera << eye;
            for (const QPointF &corner : {QPointF(-1, 1), QPointF(1, 1), QPointF(1, -1), QPointF(-1, -1)}) {
                QPointF at;
                const QVector3D local(float(corner.x() * wide * reach), float(corner.y() * tall * reach), float(-reach));
                if (!World::project(v, size, shot.position + shot.rotation.rotatedVector(local), &at)) {
                    gizmo.camera.clear();
                    break;
                }
                gizmo.camera << at;
            }
        }
    }
    if (m_freeView) {
        // Twenty meters of ground either way about what the view looks at,
        // each line in short steps so the part behind the view drops out.
        const int cx = int(std::round(m_orbitTarget.x())), cz = int(std::round(m_orbitTarget.z()));
        constexpr int reach = 20;
        for (int across = 0; across < 2; ++across)
            for (int i = -reach; i <= reach; ++i) {
                QPolygonF line;
                for (int k = -reach * 2; k <= reach * 2; ++k) {
                    const float along = k / 2.0f;
                    const QVector3D point = across ? QVector3D(float(cx + i), 0, float(cz) + along)
                                                   : QVector3D(float(cx) + along, 0, float(cz + i));
                    QPointF at;
                    if (World::project(v, size, point, &at)) {
                        line << at;
                    } else if (line.size() > 1) {
                        gizmo.grid << line;
                        line.clear();
                    } else {
                        line.clear();
                    }
                }
                if (line.size() > 1)
                    gizmo.grid << line;
            }
    }
    for (const World::Light &light : now.lights) {
        QPointF at;
        if (World::project(v, size, light.position, &at)) {
            const QVector3D c = light.colour.normalized();
            gizmo.lights << qMakePair(at, QColor::fromRgbF(c.x(), c.y(), c.z()).lighter(130));
        }
    }
    // Lights switched off still show, dimmer, so they can be found.
    for (const int index : World::lightLayers(m_scene)) {
        bool shown = false;
        for (const World::Light &light : now.lights)
            shown |= light.layer == index;
        QPointF at;
        if (!shown && World::project(v, size, World::lightPosition(m_scene, index, m_time), &at))
            gizmo.lights << qMakePair(at, QColor(150, 150, 150));
    }
    const int target = handleTarget();
    if (target == -1)
        return gizmo;
    QVector3D pivot;
    if (target == Scene::CameraIndex) {
        pivot = now.cameraPosition;
    } else if (World::lightLayers(m_scene).contains(target)) {
        pivot = World::lightPosition(m_scene, target, m_time);
    } else {
        QList<QVector3D> corners;
        if (!World::box(m_scene, assets, target, m_time, &corners, &pivot))
            return gizmo;
        for (const QVector3D &corner : corners) {
            QPointF at;
            if (!World::project(v, size, corner, &at)) {
                gizmo.corners.clear();
                break;
            }
            gizmo.corners << at;
        }
    }
    if (!World::project(v, size, pivot, &gizmo.pivot))
        return {};
    // Arrows a tenth of the view tall, however far the pivot is.
    const double distance = std::max(.1f, (pivot - v.position).length());
    const double focal = size.height() / 2 / std::tan(v.fov * M_PI / 360);
    const double meters = size.height() * .1 * distance / focal;
    for (const QVector3D &axis : {QVector3D(1, 0, 0), QVector3D(0, 1, 0), QVector3D(0, 0, -1)}) {
        QPointF end;
        if (!World::project(v, size, pivot + axis * float(meters), &end))
            end = gizmo.pivot;
        gizmo.arrows << end;
    }
    // Rings about y (yaw), x (pitch) and z (roll), as Blender's turn gizmo.
    const float r = float(meters * .8);
    for (int i = 0; i <= 48; ++i) {
        const float c = float(std::cos(2 * M_PI * i / 48)) * r, s = float(std::sin(2 * M_PI * i / 48)) * r;
        QPointF at;
        if (World::project(v, size, pivot + QVector3D(c, 0, s), &at))
            gizmo.ring << at;
        if (World::project(v, size, pivot + QVector3D(0, c, s), &at))
            gizmo.ringPitch << at;
        if (World::project(v, size, pivot + QVector3D(c, s, 0), &at))
            gizmo.ringRoll << at;
    }
    for (const int other : handleTargets().mid(1)) {
        QList<QVector3D> corners;
        QVector3D centre;
        QList<QPointF> points;
        if (World::box(m_scene, assets, other, m_time, &corners, &centre))
            for (const QVector3D &corner : corners) {
                QPointF at;
                if (!World::project(v, size, corner, &at)) {
                    points.clear();
                    break;
                }
                points << at;
            }
        if (points.size() == 8)
            gizmo.others << points;
    }
    return gizmo;
}

namespace {
double distanceToSegment(const QPointF &p, const QPointF &a, const QPointF &b) {
    const QPointF ab = b - a;
    const double length = QPointF::dotProduct(ab, ab);
    const double k = length > 0 ? std::clamp(QPointF::dotProduct(p - a, ab) / length, 0.0, 1.0) : 0;
    return QLineF(p, a + ab * k).length();
}
}

int Backend::handleAt(double x, double y, double reach) const {
    const Gizmo g = gizmo();
    if (g.arrows.size() != 3)
        return -1;
    const QPointF p(x, y);
    // The arrows' tips first: they're what's aimed at.
    int best = -1;
    double nearest = reach;
    for (int i = 0; i < 3; ++i) {
        const double d = distanceToSegment(p, g.pivot + (g.arrows.at(i) - g.pivot) * .25, g.arrows.at(i));
        if (d < nearest)
            nearest = d, best = i + 1;
    }
    if (best > 0)
        return best;
    int ring = -1;
    nearest = reach;
    for (const auto &[polygon, id] : {std::pair{&g.ring, 4}, std::pair{&g.ringPitch, 5}, std::pair{&g.ringRoll, 6}})
        for (int i = 1; i < polygon->size(); ++i) {
            const double d = distanceToSegment(p, polygon->at(i - 1), polygon->at(i));
            if (d < nearest)
                nearest = d, ring = id;
        }
    return ring;
}

void Backend::beginHandle(int handle, double x, double y) {
    const int target = handleTarget();
    if (target == -1 || handle < 0 || handle > 6)
        return;
    beginGesture();
    m_handle = handle;
    m_handleStart = QPointF(x, y);
    m_handleOrigins.clear();
    for (const int each : handleTargets())
        for (const QString &property : Scene::worldProperties() + QStringList{"rotation"})
            m_handleOrigins[each][property] = m_scene.value(*m_scene.at(each), property, m_time);
    QList<QVector3D> corners;
    if (target == Scene::CameraIndex)
        m_handlePivot = World::frame(m_scene, m_assets, m_time).cameraPosition;
    else if (World::lightLayers(m_scene).contains(target))
        m_handlePivot = World::lightPosition(m_scene, target, m_time);
    else
        World::box(m_scene, m_assets, target, m_time, &corners, &m_handlePivot);
    emit viewChanged();
}

void Backend::setHandleAxis(int axis) {
    if (m_handle < 0 || axis < 0 || axis > 6)
        return;
    // Pressing the same axis again frees it, as in Blender.
    m_handle = axis == m_handle ? 0 : axis;
    emit viewChanged();
}

void Backend::handleTo(double x, double y, bool snap) {
    const QList<int> targets = handleTargets();
    if (m_handle < 0 || targets.isEmpty())
        return;
    const World::View v = view();
    const QSizeF size = viewSize();
    const QPointF moved = QPointF(x, y) - m_handleStart;
    // Back where they started, so each move is from there.
    for (const int each : targets)
        for (auto it = m_handleOrigins[each].begin(); it != m_handleOrigins[each].end(); ++it)
            m_scene.setValue(each, it.key(), m_time, it.value());
    QPointF pivot;
    if (!World::project(v, size, m_handlePivot, &pivot)) {
        changed();
        return;
    }
    if (m_handle >= 4) {
        // Turning: by the angle swept about the pivot on screen, about the
        // ring's axis, which turns the other way seen from its far side.
        const QPointF a = m_handleStart - pivot, b = QPointF(x, y) - pivot;
        const double swept = std::atan2(a.x() * b.y() - a.y() * b.x(), a.x() * b.x() + a.y() * b.y()) * 180 / M_PI;
        const QVector3D axis = m_handle == 4 ? QVector3D(0, 1, 0) : m_handle == 5 ? QVector3D(1, 0, 0) : QVector3D(0, 0, 1);
        double about = QVector3D::dotProduct(axis, v.position - m_handlePivot) > 0 ? -swept : swept;
        if (snap)
            about = std::round(about / 15) * 15;
        for (const int each : targets) {
            const bool camera = each == Scene::CameraIndex;
            // Yaw and roll turn the other way about their axes than pitch.
            const QString property = m_handle == 4 ? "yaw" : m_handle == 5 ? "pitch" : camera ? "rotation" : "roll";
            m_scene.setValue(each, property, m_time, m_handleOrigins[each][property] + (m_handle == 5 ? about : -about));
        }
        changed();
        return;
    }
    QVector3D shift;
    if (m_handle == 0) {
        // Freely: across the view at the pivot's distance.
        const double distance = std::max(.1f, (m_handlePivot - v.position).length());
        const double meters = distance * 2 * std::tan(v.fov * M_PI / 360) / size.height();
        shift = v.rotation.rotatedVector(QVector3D(float(moved.x() * meters), float(-moved.y() * meters), 0));
    } else {
        // Along an axis: as far as the hand goes along it on screen.
        const QVector3D axis = m_handle == 1 ? QVector3D(1, 0, 0) : m_handle == 2 ? QVector3D(0, 1, 0) : QVector3D(0, 0, -1);
        QPointF end;
        if (!World::project(v, size, m_handlePivot + axis, &end)) {
            changed();
            return;
        }
        const QPointF perMeter = end - pivot;
        const double length = QPointF::dotProduct(perMeter, perMeter);
        // Pointing nearly at the view, an axis can't be followed well.
        if (length < 4) {
            changed();
            return;
        }
        shift = axis * float(QPointF::dotProduct(moved, perMeter) / length);
    }
    double dx = shift.x(), dy = shift.y(), dz = shift.z();
    if (snap) {
        dx = std::round(dx * 10) / 10;
        dy = std::round(dy * 10) / 10;
        dz = std::round(dz * 10) / 10;
    }
    for (const int each : targets) {
        m_scene.setValue(each, "worldX", m_time, m_handleOrigins[each]["worldX"] + dx);
        m_scene.setValue(each, "worldY", m_time, m_handleOrigins[each]["worldY"] + dy);
        m_scene.setValue(each, "worldZ", m_time, m_handleOrigins[each]["worldZ"] - dz);
    }
    changed();
}

void Backend::endHandle(bool keep) {
    if (m_handle < 0)
        return;
    m_handle = -1;
    if (!keep && m_inGesture) {
        // Put back as it was, with nothing to undo.
        m_scene = Scene::fromJson(m_gestureStart);
        m_inGesture = false;
        changed();
    } else {
        endGesture();
    }
    emit viewChanged();
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
    if (format == "glb") {
        // For Blender and other 3D tools: written off the window's thread,
        // as a world's meshes are big.
        if (m_job.isRunning())
            return;
        QDir().mkpath(renders.path());
        const Scene scene = m_scene;
        const QString directory = m_directory;
        auto failed = std::make_shared<QString>();
        setBusy("Writing the scene as glTF…");
        m_job.disconnect(this);
        connect(&m_job, &QFutureWatcher<void>::finished, this, [this, failed, output] {
            setBusy({});
            if (failed->startsWith("!"))
                return setError(failed->mid(1));
            setNotice("Exported " + QFileInfo(output).fileName() + ": open it in Blender (File ▸ Import ▸ glTF)." +
                      (failed->isEmpty() ? QString() : " Left out " + *failed + "."));
        });
        m_job.setFuture(QtConcurrent::run([scene, directory, output, failed] {
            AssetCache assets(directory);
            QString error;
            QStringList skipped;
            // A leading ! marks a failure; otherwise what was left out.
            if (!Gltf::write(scene, assets, output, &error, &skipped))
                *failed = "!" + error;
            else
                *failed = skipped.join("; ");
        }));
        return;
    }
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
    // The sound's waveform, worked out again when the sound or its window
    // onto it changes.
    const QJsonObject audio = m_scene.raw.value("audio").toObject();
    const QString source = audio.value("source").toString();
    const QString key = source.isEmpty() ? QString()
                                         : QString("%1|%2|%3").arg(source).arg(audio.value("offset").toDouble()).arg(m_scene.duration);
    if (key != m_peaksFor) {
        m_peaksFor = key;
        m_peaks.clear();
        if (!key.isEmpty()) {
            const QString path = QDir(m_directory).absoluteFilePath(source);
            const double offset = audio.value("offset").toDouble(), duration = m_scene.duration;
            m_peaksJob.setFuture(QtConcurrent::run([path, offset, duration] { return Project::audioPeaks(path, offset, duration, 600); }));
        }
        emit audioChanged();
        if (m_player && source.isEmpty())
            m_player->stop();
        // The sound or its offset changed: the player follows, at the playhead.
        if (!source.isEmpty())
            syncAudio(true);
    } else if (!key.isEmpty()) {
        emit audioChanged();
        syncAudio(false);  // The volume may have.
    }
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
        if (layer.particles.isEmpty() && !layer.source.isEmpty() && !QFileInfo::exists(QDir(m_directory).absoluteFilePath(layer.source)))
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
