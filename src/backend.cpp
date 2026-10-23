#include "backend.h"

#include "filepicker.h"
#include "scene.h"
#include "sound.h"
#include "studio.h"
#include "style.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QGuiApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlEngine>
#include <QStandardPaths>
#include <QUrlQuery>

Backend::Backend(FilePicker *picker, const QString &renderer, QObject *parent)
    : QObject(parent), m_picker(picker),
      m_renderer(renderer.isEmpty() ? QCoreApplication::applicationFilePath() : renderer) {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(120);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_debounce, &QTimer::timeout, this, &Backend::filesChanged);
    m_studioDebounce.setSingleShot(true);
    m_studioDebounce.setInterval(150);
    connect(&m_studioWatcher, &QFileSystemWatcher::fileChanged, this, [this] { m_studioDebounce.start(); });
    connect(&m_studioWatcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_studioDebounce.start(); });
    connect(&m_studioDebounce, &QTimer::timeout, this, &Backend::reloadStudio);

    if (m_picker) {
        connect(m_picker, &FilePicker::openSelected, this, [this](const QUrl &url) { load(url.toString()); });
        connect(m_picker, &FilePicker::newSelected, this, [this](const QUrl &url) {
            QString folder = url.toLocalFile();
            if (folder.endsWith(".qml", Qt::CaseInsensitive))
                folder.chop(4);
            if (createFilm(folder))
                load(QDir(folder).filePath("scene.qml"));
        });
        connect(m_picker, &FilePicker::failed, this, [this](const QString &message) { setMessage(message, true); });
    }

    connect(&m_export, &QProcess::readyReadStandardOutput, this, &Backend::readExportOutput);
    connect(&m_export, &QProcess::finished, this, &Backend::exportFinished);
    connect(&m_export, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            setMessage(tr("Could not start the export: %1").arg(m_export.errorString()), true);
            emit exportChanged();
        }
    });
}

Backend::~Backend() {
    if (exporting()) {
        m_export.kill();
        m_export.waitForFinished(2000);
        removePartials();
    }
}

QString Backend::sceneName() const { return QFileInfo(m_path).fileName(); }

// A new URL for every revision, so the engine compiles the edited file instead
// of handing back the one it cached.
QUrl Backend::sceneUrl() const {
    if (m_path.isEmpty())
        return {};
    QUrl url = QUrl::fromLocalFile(m_path);
    url.setQuery(QString("revision=%1").arg(m_revision));
    return url;
}

QStringList Backend::formatNames() const { return Formats::names(); }

QSize Backend::formatSize(const QString &name) const {
    Format format;
    return Formats::find(name, &format) ? format.size : QSize(1920, 1080);
}

QUrl Backend::resolve(const QString &relative) const {
    if (relative.isEmpty() || m_path.isEmpty())
        return {};
    if (QFileInfo(relative).isAbsolute())
        return QUrl::fromLocalFile(relative);
    return QUrl::fromLocalFile(QFileInfo(m_path).absoluteDir().filePath(relative));
}

void Backend::setEngine(QQmlEngine *engine) {
    m_engine = engine;
    if (m_engine && !m_path.isEmpty())
        StyleTokens::use(m_engine, StyleTokens::guideFor(m_path));
}

QVariantList Backend::waveform(const QString &track, double trim, double delay, double duration, int count) {
    const QString path = resolve(track).toLocalFile();
    if (path.isEmpty() || !QFileInfo(path).isFile() || duration <= 0 || count <= 0)
        return {};
    // 8 kHz is plenty for the shape of a track.
    constexpr int rate = 8000;
    const QString key = path + "@" + QString::number(QFileInfo(path).lastModified().toMSecsSinceEpoch());
    if (!m_tracks.contains(key)) {
        QString error;
        m_tracks.insert(key, Sound::decode(path, rate, &error));
    }
    const QList<float> &samples = m_tracks[key];
    QList<float> peaks(count, 0.0f);
    float loudest = 0;
    for (int i = 0; i < count; ++i) {
        // The slice of film, as seconds of the track.
        const double from = i * duration / count - delay + trim, to = (i + 1) * duration / count - delay + trim;
        const qsizetype first = qMax<qsizetype>(0, qsizetype(from * rate));
        const qsizetype last = qMin<qsizetype>(samples.size(), qsizetype(to * rate));
        for (qsizetype j = first; j < last; ++j)
            peaks[i] = qMax(peaks[i], std::abs(samples[j]));
        loudest = qMax(loudest, peaks[i]);
    }
    QVariantList result;
    for (float peak : peaks)
        result << (loudest > 0 ? peak / loudest : 0.0);
    return result;
}

QUrl Backend::cueTrack(const QVariantList &cues, double duration) {
    QList<Sound::Cue> sounds;
    QByteArray signature = QByteArray::number(duration);
    for (const auto &value : cues) {
        const QVariantMap cue = value.toMap();
        const QString sound = cue.value("sound").toString();
        if (!Sound::names().contains(sound))
            continue;
        sounds << Sound::Cue{cue.value("at").toDouble(), sound};
        signature += "|" + QByteArray::number(sounds.last().at) + sound.toUtf8();
    }
    if (sounds.isEmpty() || duration <= 0 || !m_cueFolder.isValid())
        return {};
    // Named by what's in it, so a changed cue is a new file the player loads.
    const QString path = m_cueFolder.filePath(
        QString::fromLatin1(QCryptographicHash::hash(signature, QCryptographicHash::Sha1).toHex().left(12)) + ".wav");
    QString error;
    if (!QFileInfo::exists(path) && !Sound::writeCues(sounds, duration, path, &error))
        return {};
    return QUrl::fromLocalFile(path);
}

bool Backend::load(const QString &pathOrUrl) {
    const QUrl url(pathOrUrl);
    const QString path = url.isLocalFile() ? url.toLocalFile() : pathOrUrl;
    const QFileInfo info(path);
    if (!info.isFile()) {
        setMessage(tr("%1 does not exist.").arg(path), true);
        return false;
    }
    if (info.suffix().compare("qml", Qt::CaseInsensitive) != 0) {
        setMessage(tr("%1 is not a scene; scenes are .qml files.").arg(info.fileName()), true);
        return false;
    }
    m_path = info.absoluteFilePath();
    m_root = Studio::rootOf(m_path);
    if (m_engine)
        StyleTokens::use(m_engine, StyleTokens::guideFor(m_path));
    reloadStudio();
    ++m_revision;
    watch();
    m_signature = signature();
    m_warning.clear();
    emit warningChanged();
    clearMessage();
    emit sceneChanged();
    return true;
}

void Backend::openScene() {
    if (m_picker)
        m_picker->openScene(QUrl::fromLocalFile(m_path.isEmpty() ? QDir::homePath() : QFileInfo(m_path).absolutePath()));
}

// New films go beside the open one's folder, or in Videos.
void Backend::newFilm() {
    if (!m_picker)
        return;
    const QString studio = Studio::rootOf(m_path);
    QString parent = !studio.isEmpty() ? QFileInfo(studio).absolutePath()
                     : !m_path.isEmpty() ? QFileInfo(m_path).absolutePath()
                     : QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    if (!QFileInfo(parent).isDir())
        parent = QDir::homePath();
    m_picker->newFilm(QUrl::fromLocalFile(QDir(parent).filePath("new-film")));
}

bool Backend::createFilm(const QString &folder) {
    // The portal asks before replacing a file, but a folder with someone's
    // work in it is never filled with templates.
    const QDir directory(folder);
    if (QFileInfo::exists(folder) &&
        (!QFileInfo(folder).isDir() || !directory.isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden))) {
        setMessage(tr("%1 already has files in it; pick a new folder for the film.").arg(QFileInfo(folder).fileName()),
                   true);
        return false;
    }
    QString error;
    if (!Studio::create(folder, &error)) {
        setMessage(tr("Could not create the film: %1").arg(error), true);
        return false;
    }
    return true;
}

void Backend::reload() {
    if (m_path.isEmpty())
        return;
    ++m_revision;
    m_signature = signature();
    m_warning.clear();
    emit warningChanged();
    emit sceneChanged();
}

void Backend::trimCache() {
    if (m_engine)
        m_engine->trimComponentCache();
}

void Backend::watch() {
    const auto paths = m_watcher.files() + m_watcher.directories();
    if (!paths.isEmpty())
        m_watcher.removePaths(paths);
    if (m_path.isEmpty())
        return;
    // Editors and agents often write a new file and rename it over the old
    // one, which drops the file from the watch; the directory sees that too.
    m_watcher.addPath(QFileInfo(m_path).absolutePath());
    if (QFileInfo::exists(m_path))
        m_watcher.addPath(m_path);
}

// The scene and the QML and JavaScript beside it, which it may import.
QString Backend::signature() const {
    QString text;
    const QDir directory = QFileInfo(m_path).absoluteDir();
    for (const auto &info : directory.entryInfoList({"*.qml", "*.js", "*.mjs", "qmldir"}, QDir::Files, QDir::Name))
        text += QString("%1:%2:%3;").arg(info.fileName()).arg(info.lastModified().toMSecsSinceEpoch()).arg(info.size());
    return text;
}

void Backend::filesChanged() {
    watch();
    if (m_path.isEmpty() || !QFileInfo::exists(m_path))
        return;
    const QString current = signature();
    if (current == m_signature)
        return;
    m_signature = current;
    ++m_revision;
    m_warning.clear();
    emit warningChanged();
    emit sceneChanged();
}

void Backend::addWarning(const QString &warning) {
    if (warning == m_warning)
        return;
    m_warning = warning;
    emit warningChanged();
}

void Backend::setMessage(const QString &message, bool error, const QStringList &outputs) {
    m_message = message;
    m_messageIsError = error;
    m_outputs = outputs;
    emit messageChanged();
}

void Backend::clearMessage() {
    if (m_message.isEmpty() && m_outputs.isEmpty())
        return;
    setMessage({}, false);
}

void Backend::exportVideo(const QStringList &formats) {
    if (exporting() || m_path.isEmpty() || formats.isEmpty())
        return;
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty()) {
        setMessage(tr("Exporting needs ffmpeg. Install it with: omarchy-pkg-add ffmpeg"), true);
        return;
    }
    m_exportFormats = formats;
    m_exportBuffer.clear();
    m_exportOutputs.clear();
    m_exportFormat = formats.first();
    m_progress = 0;
    m_canceled = false;
    clearMessage();
    m_export.setProcessChannelMode(QProcess::SeparateChannels);
    m_export.start(m_renderer, {"render", m_path, "--format", formats.join(','), "--progress"});
    emit exportChanged();
}

void Backend::cancelExport() {
    if (!exporting())
        return;
    m_canceled = true;
    m_export.kill();
}

void Backend::readExportOutput() {
    m_exportBuffer += m_export.readAllStandardOutput();
    int newline;
    bool changed = false;
    while ((newline = m_exportBuffer.indexOf('\n')) >= 0) {
        const QJsonObject line = QJsonDocument::fromJson(m_exportBuffer.left(newline)).object();
        m_exportBuffer.remove(0, newline + 1);
        if (line.contains("outputs")) {
            for (const auto &output : line["outputs"].toArray())
                m_exportOutputs << output.toObject()["video"].toString();
        } else if (line.contains("frame")) {
            const int count = qMax(1, line["count"].toInt());
            const double frames = qMax(1, line["frames"].toInt());
            m_progress = (line["index"].toInt() + line["frame"].toInt() / frames) / count;
            m_exportFormat = line["format"].toString();
            changed = true;
        }
    }
    if (changed)
        emit exportChanged();
}

void Backend::exportFinished(int code, QProcess::ExitStatus status) {
    readExportOutput();
    if (m_canceled) {
        removePartials();
        setMessage(tr("Export canceled."), false);
    } else if (status != QProcess::NormalExit || code != 0) {
        removePartials();
        const QStringList lines = QString::fromUtf8(m_export.readAllStandardError()).trimmed().split('\n');
        setMessage(tr("Export failed: %1").arg(lines.last().isEmpty() ? tr("the renderer stopped.") : lines.last()),
                   true);
    } else {
        QStringList names;
        for (const auto &output : m_exportOutputs)
            names << QFileInfo(output).fileName();
        setMessage(tr("Saved %1").arg(names.join(", ")), false, m_exportOutputs);
    }
    m_progress = 0;
    emit exportChanged();
}

// A killed render leaves its unfinished file beside where the export would go.
void Backend::removePartials() {
    for (const auto &format : m_exportFormats)
        QFile::remove(Scene::partialPath(Scene::defaultOutput(m_path, format)));
}

void Backend::showOutputs() {
    const QString folder = m_outputs.isEmpty() ? QFileInfo(Scene::defaultOutput(m_path, "16:9")).absolutePath()
                                               : QFileInfo(m_outputs.first()).absolutePath();
    QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
}

QString Backend::studioName() const { return QFileInfo(m_root).fileName(); }

void Backend::watchStudio() {
    const auto paths = m_studioWatcher.files() + m_studioWatcher.directories();
    if (!paths.isEmpty())
        m_studioWatcher.removePaths(paths);
    if (m_root.isEmpty())
        return;
    // The agent saves by writing beside a file and renaming over it, which only
    // the folder sees; so the folders are watched as well as the files.
    for (const auto &name : {"", "assets", "reviews", "reviews/notes", "renders", "gates.md", "notes.md",
                             "activity.md", "assets/assets.md"}) {
        const QString path = QDir(m_root).filePath(name);
        if (QFileInfo::exists(path))
            m_studioWatcher.addPath(path);
    }
}

namespace {
QString kindOf(const QString &suffix) {
    static const QStringList images{"png", "jpg", "jpeg", "webp", "svg", "gif"};
    static const QStringList fonts{"ttf", "otf"};
    static const QStringList audio{"wav", "mp3", "flac", "ogg", "m4a", "aac"};
    static const QStringList video{"mp4", "mov", "webm", "mkv"};
    const QString s = suffix.toLower();
    return images.contains(s) ? "image" : fonts.contains(s) ? "font" : audio.contains(s) ? "audio"
           : video.contains(s) ? "video" : s == "md" ? "text" : "other";
}
}

void Backend::reloadStudio() {
    watchStudio();
    QVariantList gates, assets, evidence, notes;
    QStringList missing, activity;
    if (!m_root.isEmpty()) {
        const QDir root(m_root);
        for (const auto &gate : Studio::readGates(m_root))
            gates << QVariantMap{{"number", gate.number}, {"title", gate.title}, {"passed", gate.passed},
                                 {"status", gate.status}, {"waiting", gate.waitingForPerson()},
                                 // Gates 1, 2, and 6 are the person's to pass; the rest are the agent's.
                                 {"person", gate.number == 1 || gate.number == 2 || gate.number == 6}};
        for (const auto &info : QDir(root.filePath("assets")).entryInfoList(QDir::Files, QDir::Name))
            if (info.fileName() != "assets.md")
                assets << QVariantMap{{"name", info.fileName()}, {"url", QUrl::fromLocalFile(info.absoluteFilePath())},
                                      {"kind", kindOf(info.suffix())}};
        missing = Studio::missingAssets(m_root);

        QFileInfoList files = QDir(root.filePath("reviews")).entryInfoList({"*.png", "*.md", "*.jpg"}, QDir::Files);
        files += QDir(root.filePath("renders")).entryInfoList({"*.mp4", "*.png", "*.webm", "*.mov"}, QDir::Files);
        std::sort(files.begin(), files.end(), [](const QFileInfo &a, const QFileInfo &b) {
            return a.lastModified() > b.lastModified();
        });
        for (const auto &info : files.mid(0, 40))
            evidence << QVariantMap{{"name", info.fileName()}, {"path", root.relativeFilePath(info.absoluteFilePath())},
                                    {"url", QUrl::fromLocalFile(info.absoluteFilePath())},
                                    {"kind", kindOf(info.suffix())}, {"modified", info.lastModified()}};

        for (const auto &note : Studio::readNotes(m_root)) {
            const QFileInfo proof(root.filePath(note.evidence));
            notes << QVariantMap{{"id", note.id}, {"done", note.done}, {"at", note.at}, {"format", note.format},
                                 {"beat", note.beat}, {"target", note.target}, {"text", note.text},
                                 {"answers", note.answers}, {"evidence", note.evidence},
                                 {"evidenceUrl", !note.evidence.isEmpty() && proof.isFile()
                                                     ? QUrl::fromLocalFile(proof.absoluteFilePath()) : QUrl()},
                                 // Changes when the frame is rewritten, so the card shows the new one.
                                 {"evidenceStamp", proof.isFile() ? proof.lastModified().toMSecsSinceEpoch() : 0}};
        }
        activity = Studio::readActivity(m_root);
    }
    auto update = [](auto &member, const auto &value, auto signal) {
        if (member != value) {
            member = value;
            signal();
        }
    };
    update(m_gates, gates, [this] { emit gatesChanged(); });
    if (m_assets != assets || m_missing != missing) {
        m_assets = assets;
        m_missing = missing;
        emit assetsChanged();
    }
    update(m_evidence, evidence, [this] { emit evidenceChanged(); });
    update(m_notes, notes, [this] { emit notesChanged(); });
    update(m_activity, activity, [this] { emit activityChanged(); });
    emit studioChanged();
}

QString Backend::addNote(double at, const QString &format, const QString &beat, int x, int y, const QString &target,
                         const QString &text) {
    if (m_root.isEmpty() || text.trimmed().isEmpty())
        return {};
    Studio::Note note;
    note.at = at;
    note.format = format;
    note.beat = beat;
    note.point = {x, y};
    note.target = target;
    note.text = text;
    QString error;
    const QString id = Studio::addNote(m_root, note, &error);
    if (id.isEmpty()) {
        setMessage(tr("Could not save the note: %1").arg(error), true);
        return {};
    }
    // The evidence is the frame as it renders, made the same way an export is,
    // at exactly the time notes.md records. A failure is said, never hidden,
    // since the note would otherwise point at a frame that doesn't exist.
    QDir(m_root).mkpath("reviews/notes");
    auto *render = new QProcess(this);
    render->setProcessChannelMode(QProcess::SeparateChannels);
    auto failed = [this, id, render](const QString &why) {
        setMessage(tr("Note %1 is saved, but its evidence frame couldn't be rendered: %2").arg(id, why), true);
    };
    connect(render, &QProcess::errorOccurred, this, [render, failed](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            failed(render->errorString());
            render->deleteLater();
        }
    });
    connect(render, &QProcess::finished, this, [this, render, failed](int code, QProcess::ExitStatus status) {
        if (status != QProcess::NormalExit || code != 0) {
            const QStringList lines = QString::fromUtf8(render->readAllStandardError()).trimmed().split('\n');
            failed(lines.last().isEmpty() ? tr("the renderer stopped.") : lines.last());
        }
        reloadStudio();
        render->deleteLater();
    });
    render->start(m_renderer, {"frame", m_path, "--at", Studio::noteTime(at), "--format", format, "--mark",
                               QString("%1,%2").arg(x).arg(y), "-o",
                               QDir(m_root).filePath("reviews/notes/" + id + ".png")});
    reloadStudio();
    setMessage(tr("Note %1 saved to notes.md. Tell your agent to check your notes.").arg(id), false);
    return id;
}

void Backend::copyNote(const QString &id) {
    for (const auto &note : Studio::readNotes(m_root))
        if (note.id == id) {
            QGuiApplication::clipboard()->setText(Studio::noteReference(note));
            setMessage(tr("Copied note %1. Paste it into your agent's terminal.").arg(id), false);
            return;
        }
}

void Backend::copyOpenNotes() {
    QStringList open;
    for (const auto &note : Studio::readNotes(m_root))
        if (!note.done)
            open << note.id;
    if (open.isEmpty())
        return;
    // English on purpose: it's addressed to the agent, whose skill is in English.
    QGuiApplication::clipboard()->setText(
        QString("Check my notes: notes.md has %1 open (%2). Fix each with evidence, or reply under it.")
            .arg(open.size()).arg(open.join(", ")));
    setMessage(tr("Copied a request to check %n open notes. Paste it into your agent's terminal.", "", open.size()),
               false);
}

void Backend::openStudioPath(const QString &path) {
    if (m_root.isEmpty())
        return;
    const QString full = QFileInfo(path).isAbsolute() ? path : QDir(m_root).filePath(path);
    if (QFileInfo::exists(full))
        QDesktopServices::openUrl(QUrl::fromLocalFile(full));
}
