#include "exporter.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonObject>
#include <QRegularExpression>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTimer>
#include <cmath>
#include <cstdio>

namespace {
// Enough frames in flight to keep ffmpeg busy without holding a whole
// video's worth of raw pixels in memory.
constexpr qint64 MaxBuffered = 32 * 1024 * 1024;
}

const QStringList &Exporter::formats() {
    static const QStringList list{"mp4", "webm", "gif", "png"};
    return list;
}

QString Exporter::formatFor(const QString &output) {
    const QString suffix = QFileInfo(output).suffix().toLower();
    if (suffix.isEmpty() || output.endsWith('/'))
        return "png";
    return formats().contains(suffix) && suffix != "png" ? suffix : QString();
}

bool Exporter::start(const Scene &scene, const QString &directory, const QString &output, double scale) {
    m_error.clear();
    if (m_busy) {
        m_error = "An export is already running.";
        return false;
    }
    m_format = formatFor(output);
    if (m_format.isEmpty()) {
        m_error = QString("Export to .mp4, .webm or .gif, or a folder for PNG frames; not %1.").arg(output);
        return false;
    }

    m_scene = scene;
    m_assets.setDirectory(directory);
    // Frames draw big pictures small; their smaller copies are all it takes.
    m_assets.setKeepFull(false);
    m_output = QFileInfo(output).absoluteFilePath();
    m_count = scene.frameCount();
    m_next = 0;
    m_cancelled = false;
    // H.264 wants even dimensions; the others don't care, so match it everywhere.
    if (!(scale > 0 && scale <= 4)) {
        m_error = "Scale exports between 0 and 4 times the canvas.";
        return false;
    }
    const int width = std::max(2, int(std::lround(scene.width * scale))),
              height = std::max(2, int(std::lround(scene.height * scale)));
    m_size = QSize(width - width % 2, height - height % 2);

    if (m_format == "png") {
        if (!QDir().mkpath(m_output)) {
            m_error = QString("Couldn't create %1.").arg(m_output);
            return false;
        }
        // Frames left from a longer earlier render would play on past the end.
        const QDir frames(m_output);
        static const QRegularExpression numbered("^\\d{4,}\\.png$");
        for (const QString &name : frames.entryList(QDir::Files))
            if (numbered.match(name).hasMatch())
                QFile::remove(frames.filePath(name));
        m_busy = true;
        QTimer::singleShot(0, this, &Exporter::writeFrames);
        return true;
    }

    const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
    if (ffmpeg.isEmpty()) {
        m_error = "ffmpeg isn't installed, so there's nothing to encode with. Install it with: omarchy-pkg-add ffmpeg";
        return false;
    }
    QDir().mkpath(QFileInfo(m_output).absolutePath());
    // Encode beside the target and rename at the end, so a failed or
    // cancelled export never leaves a broken file under the real name.
    const QFileInfo target(m_output);
    m_partial = target.absoluteDir().filePath("." + target.completeBaseName() + ".partial." + m_format);

    QStringList args{"-hide_banner", "-loglevel", "error", "-y",
                     "-f", "rawvideo", "-pix_fmt", "bgra",
                     "-s", QString("%1x%2").arg(m_size.width()).arg(m_size.height()),
                     "-framerate", QString::number(scene.fps), "-i", "-"};
    // The scene's sound, for video that can carry it: from its offset, as
    // long as the scene.
    const QJsonObject audio = scene.raw.value("audio").toObject();
    const QString sound = audio.value("source").toString().isEmpty()
                              ? QString()
                              : QDir(directory).absoluteFilePath(audio.value("source").toString());
    const bool withSound = m_format != "gif" && !sound.isEmpty() && QFileInfo::exists(sound);
    if (withSound)
        args << "-ss" << QString::number(std::max(0.0, audio.value("offset").toDouble())) << "-t"
             << QString::number(scene.duration) << "-i" << sound;
    if (m_format == "mp4")
        args << "-c:v" << "libx264" << "-pix_fmt" << "yuv420p" << "-crf" << "18" << "-preset" << "medium"
             << "-movflags" << "+faststart";
    else if (m_format == "webm")
        args << "-c:v" << "libvpx-vp9" << "-pix_fmt" << "yuva420p" << "-crf" << "30" << "-b:v" << "0";
    else
        // GIFs go to chats and READMEs, where size counts: at most 15 fps.
        args << "-vf"
             << QString("%1split[a][b];[a]palettegen=reserve_transparent=1:stats_mode=diff[p];[b][p]paletteuse=dither=sierra2_4a")
                    .arg(scene.fps > 15 ? "fps=15," : "")
             << "-loop" << "0";
    if (withSound) {
        args << "-map" << "0:v" << "-map" << "1:a" << "-c:a" << (m_format == "mp4" ? "aac" : "libopus") << "-b:a"
             << "160k" << "-filter:a" << QString("volume=%1").arg(std::clamp(audio.value("volume").toDouble(1), 0.0, 4.0))
             << "-shortest";
    }
    args << m_partial;

    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_process, &QProcess::started, this, &Exporter::feed);
    connect(m_process, &QProcess::bytesWritten, this, &Exporter::feed);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            done(false, "ffmpeg failed to start: " + m_process->errorString());
    });
    connect(m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if (!m_busy)
            return;
        if (m_cancelled) {
            QFile::remove(m_partial);
            done(false, "Export cancelled.");
        } else if (status != QProcess::NormalExit || code != 0) {
            QFile::remove(m_partial);
            const QString log = QString::fromUtf8(m_process->readAllStandardError()).trimmed();
            done(false, "ffmpeg failed: " + (log.isEmpty() ? QString("exit code %1").arg(code) : log));
        } else if (std::rename(QFile::encodeName(m_partial).constData(), QFile::encodeName(m_output).constData()) != 0) {
            done(false, QString("Couldn't move the export into place at %1.").arg(m_output));
        } else {
            done(true, m_output);
        }
    });
    m_busy = true;
    m_process->start(ffmpeg, args);
    return true;
}

void Exporter::feed() {
    if (!m_busy || !m_process || m_cancelled || m_process->state() != QProcess::Running)
        return;
    // One frame a turn: the window keeps drawing between them, and ffmpeg
    // asks for the next as it takes in this one.
    if (m_next < m_count && m_process->bytesToWrite() < MaxBuffered) {
        const QImage frame = renderFrame(m_scene, m_assets, m_scene.frameTime(m_next), m_size)
                                 .convertToFormat(QImage::Format_ARGB32);
        // Rows can be padded, so write them one by one at their real width.
        for (int y = 0; y < frame.height(); ++y)
            m_process->write(reinterpret_cast<const char *>(frame.constScanLine(y)), frame.width() * 4);
        ++m_next;
        emit progress(double(m_next) / m_count);
    }
    if (m_next >= m_count && m_process->bytesToWrite() == 0)
        m_process->closeWriteChannel();
}

void Exporter::writeFrames() {
    if (!m_busy)
        return;
    if (m_cancelled) {
        done(false, "Export cancelled.");
        return;
    }
    // A few frames per turn of the event loop keeps the window responsive.
    const int stop = std::min(m_count, m_next + 4);
    for (; m_next < stop; ++m_next) {
        const QImage frame = renderFrame(m_scene, m_assets, m_scene.frameTime(m_next), m_size);
        const QString path = QDir(m_output).filePath(QString("%1.png").arg(m_next + 1, 4, 10, QChar('0')));
        if (!frame.save(path, "PNG")) {
            done(false, QString("Couldn't write %1.").arg(path));
            return;
        }
    }
    emit progress(double(m_next) / m_count);
    if (m_next >= m_count)
        done(true, m_output);
    else
        QTimer::singleShot(0, this, &Exporter::writeFrames);
}

void Exporter::cancel() {
    if (!m_busy)
        return;
    m_cancelled = true;
    if (m_process)
        m_process->kill();
}

void Exporter::done(bool ok, const QString &message) {
    if (!m_busy)
        return;
    m_busy = false;
    if (m_process) {
        m_process->deleteLater();
        m_process = nullptr;
    }
    emit finished(ok, message);
}

bool Exporter::run(const Scene &scene, const QString &directory, const QString &output, QString *error, double scale) {
    Exporter exporter;
    if (!exporter.start(scene, directory, output, scale)) {
        if (error)
            *error = exporter.error();
        return false;
    }
    bool result = false;
    QEventLoop loop;
    QObject::connect(&exporter, &Exporter::finished, &loop, [&](bool ok, const QString &message) {
        result = ok;
        if (!ok && error)
            *error = message;
        loop.quit();
    });
    loop.exec();
    return result;
}
