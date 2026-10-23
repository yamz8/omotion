#include "scene.h"
#include "studio.h"
#include "sound.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaProperty>
#include <QProcess>
#include <QQuickItem>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>

// Rendering encodes each beat on its own and joins them, so a patch to one
// beat re-renders only that beat: a frame whose items are all as they were
// looks as it did, and the cache keeps each beat's encoding until its frames
// change.

namespace {
QString renderer;

// Encoder settings belong to the cache key: a change to them is a new film.
const QStringList videoSettings{"-c:v", "libx264", "-preset", "medium", "-crf", "18", "-pix_fmt", "yuv420p"};

void addValue(QCryptographicHash &hash, const QVariant &value) {
    switch (value.typeId()) {
    case QMetaType::Double:
    case QMetaType::Float:
        hash.addData(QByteArray::number(value.toDouble(), 'g', 10));
        break;
    case QMetaType::Int:
    case QMetaType::Bool:
    case QMetaType::QString:
    case QMetaType::QUrl:
        hash.addData(value.toString().toUtf8());
        break;
    default: {
        QString text;
        QDebug(&text) << value;
        hash.addData(text.toUtf8());
    }
    }
    hash.addData("|");
}

// The properties a type draws with: its native ones. Properties a scene's
// QML declares (data arrays, timings) change pixels only through these, so
// they'd only make every frame look changed.
const QMetaObject *native(const QObject *object) {
    const QMetaObject *meta = object->metaObject();
    while (meta && QByteArray(meta->className()).contains("_QML"))
        meta = meta->superClass();
    return meta;
}

int nativeProperties(const QObject *object) {
    const QMetaObject *meta = native(object);
    return meta ? meta->propertyCount() : 0;
}

void addProperties(QCryptographicHash &hash, QObject *object, int from, int depth) {
    const QMetaObject *meta = object->metaObject();
    for (int i = from; i < nativeProperties(object); ++i) {
        const QMetaProperty property = meta->property(i);
        if (!property.isReadable())
            continue;
        const QVariant value = property.read(object);
        if (value.metaType().flags() & QMetaType::PointerToQObject) {
            // Grouped properties such as border and gradient are part of how
            // the item looks; anchors only place it, which is already hashed.
            QObject *group = value.value<QObject *>();
            if (depth < 3 && group && group->parent() == object && !qobject_cast<QQuickItem *>(group) &&
                qstrcmp(property.name(), "anchors") != 0) {
                hash.addData(property.name());
                addProperties(hash, group, QObject::staticMetaObject.propertyCount(), depth + 1);
            }
            continue;
        }
        if (QByteArray(value.typeName()).startsWith("QQmlListProperty"))
            continue;
        hash.addData(property.name());
        addValue(hash, value);
    }
    // Objects a type draws from that aren't items, such as a Shape's paths.
    if (depth < 3)
        for (QObject *child : object->children())
            if (!qobject_cast<QQuickItem *>(child)) {
                hash.addData(native(child) ? native(child)->className() : "");
                addProperties(hash, child, QObject::staticMetaObject.propertyCount(), depth + 1);
            }
}

// Items that draw nothing themselves: they place their children, whose
// positions are hashed, and hold models, which change nothing until shown.
bool placesOnly(const QQuickItem *item) {
    static const QList<const char *> types{"QQuickRepeater", "QQuickLoader", "QQuickBasePositioner",
                                           "QQuickFlickable", "QQuickItemView", "QQuickPathView"};
    for (const char *type : types)
        if (item->inherits(type))
            return true;
    return nativeProperties(item) == QQuickItem::staticMetaObject.propertyCount();
}

// Items whose pixels come from more than their properties.
bool unhashable(const QQuickItem *item) {
    return item->inherits("QQuickCanvasItem") || item->inherits("QQuickVideoOutput") ||
           item->inherits("QQuickAnimatedImage");
}

bool addItem(QCryptographicHash &hash, QQuickItem *item, double opacity) {
    if (!item->isVisible())
        return true;
    opacity *= item->opacity();
    if (opacity <= 0)
        return true;
    if (unhashable(item))
        return false;
    // The native type: a QML type's name counts the engines a process made.
    hash.addData(native(item) ? native(item)->className() : "");
    for (const QPointF &corner : {QPointF(0, 0), QPointF(item->width(), 0), QPointF(0, item->height())}) {
        const QPointF at = item->mapToScene(corner);
        hash.addData(QByteArray::number(at.x(), 'f', 3) + "," + QByteArray::number(at.y(), 'f', 3) + ";");
    }
    hash.addData(QByteArray::number(opacity, 'f', 5) + (item->clip() ? "c" : "") + QByteArray::number(item->z()));
    if (!placesOnly(item))
        addProperties(hash, item, QQuickItem::staticMetaObject.propertyCount(), 0);
    for (QQuickItem *child : item->childItems())
        if (!addItem(hash, child, opacity))
            return false;
    return true;
}

// Everything a frame shows that its items' properties don't: the files it
// loads (scenes and scripts show their effect in the items), the format,
// and how it's encoded.
QByteArray inputsKey(const QString &scenePath, const QString &outputFolder, const Format &format, int fps,
                     const QString &audio) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArray(omotionVersion) + format.name.toUtf8() + QByteArray::number(fps) +
                 videoSettings.join(' ').toUtf8() + audio.toUtf8());
    const QDir folder = QFileInfo(scenePath).absoluteDir();
    QStringList files;
    QDirIterator it(folder.path(), QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString absolute = it.next();
        const QString path = folder.relativeFilePath(absolute);
        const QString top = path.section('/', 0, 0);
        // What omotion writes is never what a frame is made from.
        if (top == "renders" || top == "reviews" || top == "deliverables" || top.startsWith('.') ||
            absolute.startsWith(outputFolder + "/"))
            continue;
        // A scene in a folder this big isn't a film's folder; don't cache it.
        if (files.size() > 5000)
            return {};
        const QString suffix = it.fileInfo().suffix().toLower();
        if (suffix == "qml" || suffix == "js" || suffix == "mjs" || suffix == "md")
            continue;
        files << path;
    }
    files.sort();
    for (const auto &path : files) {
        const QFileInfo info(folder.filePath(path));
        hash.addData(path.toUtf8() + QByteArray::number(info.size()) +
                     QByteArray::number(info.lastModified().toMSecsSinceEpoch()));
    }
    return hash.result().toHex().left(16);
}

bool run(QProcess &process, QString *error) {
    process.waitForFinished(-1);
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString message = QString::fromUtf8(process.readAllStandardError()).trimmed();
        *error = "ffmpeg failed" + (message.isEmpty() ? QString(".") : ": " + message);
        return false;
    }
    return true;
}
}

void Scene::setRenderer(const QString &path) { renderer = path; }

QByteArray Scene::frameKey(double t) {
    seek(t);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!addItem(hash, m_root, 1))
        return {};
    return hash.result().toHex().left(24);
}

bool Scene::encodeRange(int first, int count, const QString &output, QString *error,
                        const std::function<void()> &frameDone) {
    const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
    if (ffmpeg.isEmpty()) {
        *error = "ffmpeg is not installed; install it with: omarchy-pkg-add ffmpeg";
        return false;
    }
    QStringList arguments{"-hide_banner", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "bgra",
                          "-s", QString("%1x%2").arg(m_format.size.width()).arg(m_format.size.height()),
                          "-r", QString::number(m_fps), "-i", "-"};
    arguments << videoSettings << output;
    QProcess encoder;
    encoder.setProcessChannelMode(QProcess::SeparateChannels);
    encoder.start(ffmpeg, arguments);
    if (!encoder.waitForStarted()) {
        *error = "Could not start ffmpeg: " + encoder.errorString();
        return false;
    }
    for (int f = first; f < first + count; ++f) {
        // Format_RGB32 is BGRA in memory on little-endian machines, which is
        // what ffmpeg is told to expect.
        const QImage image = frame(f / double(m_fps));
        if (image.size() != m_format.size) {
            encoder.kill();
            encoder.waitForFinished();
            QFile::remove(output);
            *error = QString("A frame came back at %1x%2 instead of %3x%4.")
                         .arg(image.width()).arg(image.height())
                         .arg(m_format.size.width()).arg(m_format.size.height());
            return false;
        }
        for (int y = 0; y < image.height(); ++y)
            encoder.write(reinterpret_cast<const char *>(image.constScanLine(y)), image.width() * 4);
        while (encoder.bytesToWrite() > 0 && encoder.state() == QProcess::Running)
            encoder.waitForBytesWritten(1000);
        if (encoder.state() != QProcess::Running)
            break;
        if (frameDone)
            frameDone();
    }
    encoder.closeWriteChannel();
    if (!run(encoder, error)) {
        QFile::remove(output);
        return false;
    }
    return true;
}

QString Scene::cachePath(const QString &output) const {
    const QFileInfo info(output);
    return info.absoluteDir().filePath(".cache/" + info.completeBaseName());
}

bool Scene::render(const QString &output, QString *error, const std::function<void(int, int)> &progress,
                   bool fresh) {
    m_reusedFrames = 0;
    const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
    if (ffmpeg.isEmpty()) {
        *error = "ffmpeg is not installed; install it with: omarchy-pkg-add ffmpeg";
        return false;
    }
    const QString target = QFileInfo(output).absoluteFilePath();
    const QDir cache(cachePath(target));
    if (!QDir().mkpath(cache.path())) {
        *error = "Could not create " + cache.path();
        return false;
    }
    const QString partial = partialPath(target);
    QFile::remove(partial);

    // The film in segments, one per beat, each with a key from its frames.
    struct Segment {
        int first = 0;
        int count = 0;
        QByteArray key;
        QString file;
        bool reused = false;
    };
    const int frames = frameCount();
    QList<int> starts{0};
    for (const auto &beat : m_beats) {
        const int at = int(std::lround(beat.at * m_fps));
        if (at > starts.last() && at < frames)
            starts << at;
    }
    if (m_beats.isEmpty())
        for (int at = 2 * m_fps; at < frames; at += 2 * m_fps)
            starts << at;
    const QByteArray inputs = inputsKey(m_path, QFileInfo(target).absolutePath(), m_format, m_fps, m_audio);
    if (inputs.isEmpty())
        fresh = true;
    QList<Segment> segments;
    for (int i = 0; i < starts.size(); ++i) {
        Segment segment;
        segment.first = starts[i];
        segment.count = (i + 1 < starts.size() ? starts[i + 1] : frames) - starts[i];
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(inputs + QByteArray::number(segment.first) + "+" + QByteArray::number(segment.count));
        for (int f = segment.first; f < segment.first + segment.count; ++f) {
            const QByteArray key = frameKey(f / double(m_fps));
            if (key.isEmpty()) {
                hash.reset();
                break;
            }
            hash.addData(key);
        }
        const QByteArray key = hash.result().toHex().left(16);
        segment.key = key == QCryptographicHash::hash({}, QCryptographicHash::Sha256).toHex().left(16) ? QByteArray()
                                                                                                         : key;
        segment.file = cache.filePath(QString("%1-%2.mp4").arg(segment.first, 6, 10, QChar('0'))
                                          .arg(segment.key.isEmpty() ? QString("fresh") : QString(segment.key)));
        segment.reused = !fresh && !segment.key.isEmpty() && QFileInfo(segment.file).size() > 0;
        segments << segment;
    }

    int done = 0;
    QList<const Segment *> pending;
    for (const auto &segment : segments) {
        if (segment.reused) {
            done += segment.count;
            m_reusedFrames += segment.count;
        } else {
            pending << &segment;
        }
    }
    if (progress)
        progress(done, frames);

    // Beats to encode, each in a process of its own when there are several,
    // since one process renders on one core.
    const int jobs = qMin<int>(pending.size(), m_jobs > 0 ? m_jobs : qMax(1, QThread::idealThreadCount() / 2));
    if (jobs > 1 && !renderer.isEmpty()) {
        QList<QPair<QProcess *, const Segment *>> running;
        auto stop = [&] {
            for (auto &[process, segment] : running) {
                process->kill();
                process->waitForFinished();
                QFile::remove(segment->file + ".part.mp4");
                delete process;
            }
            running.clear();
        };
        int next = 0;
        while (next < pending.size() || !running.isEmpty()) {
            while (next < pending.size() && running.size() < jobs) {
                const Segment *segment = pending[next++];
                auto *process = new QProcess;
                QStringList arguments{"_segment", m_path, "--format", m_format.name, "--first",
                                      QString::number(segment->first), "--count", QString::number(segment->count),
                                      "--output", segment->file + ".part.mp4", "--seed", QString::number(Scene::seed())};
                process->start(renderer, arguments);
                running << qMakePair(process, segment);
            }
            for (int i = running.size() - 1; i >= 0; --i) {
                QProcess *process = running[i].first;
                process->waitForReadyRead(20);
                // A line per frame encoded.
                while (process->canReadLine()) {
                    process->readLine();
                    if (progress)
                        progress(++done, frames);
                }
                if (process->state() != QProcess::NotRunning)
                    continue;
                const Segment *segment = running[i].second;
                const bool ok = process->exitStatus() == QProcess::NormalExit && process->exitCode() == 0;
                const QString message = QString::fromUtf8(process->readAllStandardError()).trimmed();
                running.removeAt(i);
                delete process;
                if (!ok) {
                    stop();
                    *error = message.isEmpty() ? "A beat failed to encode." : message;
                    return false;
                }
                QFile::remove(segment->file);
                if (!QFile::rename(segment->file + ".part.mp4", segment->file)) {
                    stop();
                    *error = "Could not keep " + segment->file;
                    return false;
                }
            }
        }
    } else {
        for (const Segment *segment : pending) {
            const QString part = segment->file + ".part.mp4";
            if (!encodeRange(segment->first, segment->count, part, error, [&] {
                    if (progress)
                        progress(++done, frames);
                }))
                return false;
            QFile::remove(segment->file);
            if (!QFile::rename(part, segment->file)) {
                *error = "Could not keep " + segment->file;
                return false;
            }
        }
    }

    // Join the segments without encoding them again, with the soundtrack.
    QSaveFile list(cache.filePath("segments.txt"));
    if (!list.open(QIODevice::WriteOnly)) {
        *error = "Could not write " + list.fileName();
        return false;
    }
    for (const auto &segment : segments)
        list.write("file '" + QFileInfo(segment.file).fileName().toUtf8() + "'\n");
    if (!list.commit()) {
        *error = "Could not write " + list.fileName();
        return false;
    }
    QStringList arguments{"-hide_banner", "-loglevel", "error", "-y", "-f", "concat", "-safe", "0",
                          "-i", cache.filePath("segments.txt")};
    // The sound: the track, trimmed and delayed, with the cues' synthesized
    // sounds mixed in, brought to the loudness feeds play at (-14 LUFS).
    QStringList mix;
    int input = 1;
    if (!m_audio.isEmpty()) {
        arguments << "-i" << m_audio;
        mix << QString("[%1:a]atrim=start=%2,asetpts=PTS-STARTPTS,adelay=%3:all=1[track]")
                   .arg(input++).arg(m_audioTrim, 0, 'f', 6).arg(qRound(m_audioDelay * 1000));
    }
    QList<Sound::Cue> sounds;
    for (const auto &cue : m_cues)
        if (!cue.sound.isEmpty())
            sounds << Sound::Cue{cue.at, cue.sound};
    if (!sounds.isEmpty()) {
        const QString cueTrack = cache.filePath("cues.wav");
        if (!Sound::writeCues(sounds, frames / double(m_fps), cueTrack, error))
            return false;
        arguments << "-i" << cueTrack;
        mix << QString("[%1:a]anull[cues]").arg(input++);
    }
    arguments << "-map" << "0:v" << "-c:v" << "copy";
    if (!mix.isEmpty()) {
        const QString both = !m_audio.isEmpty() && !sounds.isEmpty()
                                 ? "[track][cues]amix=inputs=2:normalize=0:duration=longest"
                                 : m_audio.isEmpty() ? "[cues]anull" : "[track]anull";
        arguments << "-filter_complex"
                  << mix.join(';') + ";" + both + ",loudnorm=I=-14:TP=-1.5:LRA=11,aresample=48000[sound]"
                  << "-map" << "[sound]" << "-c:a" << "aac" << "-b:a" << "192k";
    }
    arguments << "-t" << QString::number(frames / double(m_fps), 'f', 6) << "-movflags" << "+faststart" << partial;
    QProcess join;
    join.start(ffmpeg, arguments);
    if (!join.waitForStarted()) {
        *error = "Could not start ffmpeg: " + join.errorString();
        return false;
    }
    if (!run(join, error)) {
        QFile::remove(partial);
        return false;
    }
    if (QFileInfo::exists(target) && !QFile::remove(target)) {
        QFile::remove(partial);
        *error = "Could not replace " + target;
        return false;
    }
    if (!QFile::rename(partial, target)) {
        QFile::remove(partial);
        *error = "Could not move the render into " + target;
        return false;
    }

    // Segments the film no longer uses.
    QSet<QString> kept;
    for (const auto &segment : segments)
        kept << QFileInfo(segment.file).fileName();
    for (const auto &name : cache.entryList({"*.mp4"}, QDir::Files))
        if (!kept.contains(name))
            QFile::remove(cache.filePath(name));
    return true;
}
