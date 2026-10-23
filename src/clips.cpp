#include "clips.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QQmlEngine>
#include <QStandardPaths>
#include <cmath>

void Clips::use(QQmlEngine *engine, const QString &scenePath) {
    engine->setProperty("omotionScenePath", scenePath);
    if (auto *existing = engine->property("omotionClips").value<Clips *>())
        existing->m_scene = scenePath;
}

const Clips::Decoded *Clips::decode(const QString &clip, int fps) {
    const QFileInfo scene(m_scene);
    const QString path = QFileInfo(clip).isAbsolute() ? clip : scene.absoluteDir().filePath(clip);
    const QFileInfo info(path);
    if (!info.isFile() || fps <= 0)
        return nullptr;
    // Decoded again when the file or the frame rate changes.
    const QString key = QString::fromLatin1(
        QCryptographicHash::hash((info.absoluteFilePath() + QString::number(info.size()) +
                                  QString::number(info.lastModified().toMSecsSinceEpoch()) + "@" + QString::number(fps))
                                     .toUtf8(),
                                 QCryptographicHash::Sha1)
            .toHex()
            .left(16));
    if (auto it = m_decoded.constFind(key); it != m_decoded.constEnd())
        return &it.value();

    const QDir folder(scene.absoluteDir().filePath("renders/.cache/clips/" + key));
    Decoded decoded{folder.path(), 0};
    const QString done = folder.filePath("done");
    if (!QFileInfo::exists(done)) {
        const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
        if (ffmpeg.isEmpty() || !QDir().mkpath(folder.path()))
            return nullptr;
        // Every frame at the scene's rate, near-lossless, no wider than 1920.
        QProcess process;
        process.start(ffmpeg, {"-hide_banner", "-loglevel", "error", "-y", "-i", info.absoluteFilePath(), "-vf",
                               QString("fps=%1,scale='min(1920,iw)':-2").arg(fps), "-q:v", "2",
                               folder.filePath("%06d.jpg")});
        if (!process.waitForFinished(-1) || process.exitCode() != 0)
            return nullptr;
        QFile marker(done);
        if (!marker.open(QIODevice::WriteOnly))
            return nullptr;
    }
    decoded.frames = int(folder.entryList({"*.jpg"}, QDir::Files).size());
    if (decoded.frames == 0)
        return nullptr;
    return &m_decoded.insert(key, decoded).value();
}

QUrl Clips::frame(const QString &clip, double time, int fps) {
    const Decoded *decoded = decode(clip, fps);
    if (!decoded)
        return {};
    // Frame n shows from n / fps; ffmpeg numbers them from 1.
    const int n = qBound(0, int(std::floor(time * fps + 1e-6)), decoded->frames - 1);
    return QUrl::fromLocalFile(QDir(decoded->folder).filePath(QString("%1.jpg").arg(n + 1, 6, 10, QChar('0'))));
}

double Clips::length(const QString &clip, int fps) {
    const Decoded *decoded = decode(clip, fps);
    return decoded ? decoded->frames / double(fps) : 0;
}
