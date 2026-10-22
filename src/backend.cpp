#include "backend.h"

#include "filepicker.h"
#include "scene.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlEngine>
#include <QSaveFile>
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

    if (m_picker) {
        connect(m_picker, &FilePicker::openSelected, this, [this](const QUrl &url) { load(url.toString()); });
        connect(m_picker, &FilePicker::newSelected, this, [this](const QUrl &url) {
            if (createScene(url.toLocalFile()))
                load(url.toLocalFile());
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

void Backend::newScene() {
    if (!m_picker)
        return;
    const QString folder = m_path.isEmpty() ? QDir::homePath() : QFileInfo(m_path).absolutePath();
    m_picker->newScene(QUrl::fromLocalFile(QDir(folder).filePath("scene.qml")));
}

bool Backend::createScene(const QString &path) {
    // The portal asks before replacing a file, but a scene someone already
    // wrote is never overwritten with the starter.
    if (QFileInfo::exists(path)) {
        setMessage(tr("%1 already exists; pick a new name.").arg(QFileInfo(path).fileName()), true);
        return false;
    }
    QFile starter(":/starter.qml");
    QSaveFile file(path);
    if (!starter.open(QIODevice::ReadOnly) || !file.open(QIODevice::WriteOnly) ||
        file.write(starter.readAll()) < 0 || !file.commit()) {
        setMessage(tr("Could not write %1: %2").arg(path, file.errorString()), true);
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
