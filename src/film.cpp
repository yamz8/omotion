#include "film.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QProcess>
#include <QStandardPaths>
#include <cmath>

#include "project.h"

bool Film::load(const QString &path, QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(QString("Couldn't read %1.").arg(path));
    QJsonParseError parse;
    const QJsonObject json = QJsonDocument::fromJson(file.readAll(), &parse).object();
    if (parse.error != QJsonParseError::NoError)
        return fail(QString("%1 isn't JSON: %2.").arg(path, parse.errorString()));
    const QDir here = QFileInfo(path).absoluteDir();
    m_shots.clear();
    double at = 0;
    for (const QJsonValue &value : json.value("shots").toArray()) {
        const QJsonObject o = value.toObject();
        auto shot = std::make_shared<Shot>();
        shot->path = here.absoluteFilePath(o.value("project").toString());
        QString why;
        shot->scene = Project::load(shot->path, &why);
        if (!why.isEmpty())
            return fail(QString("Shot %1 (%2): %3").arg(m_shots.size() + 1).arg(o.value("project").toString(), why));
        shot->assets = std::make_unique<AssetCache>(shot->path);
        shot->assets->setKeepFull(false);
        shot->from = std::clamp(o.value("from").toDouble(0), 0.0, shot->scene.duration);
        shot->to = std::clamp(o.value("to").toDouble(shot->scene.duration), shot->from, shot->scene.duration);
        shot->transition = o.value("transition").toString("cut");
        if (!QStringList{"cut", "dissolve", "fade", "wipe"}.contains(shot->transition))
            return fail(QString("Shot %1's transition is %2; use cut, dissolve, fade or wipe.").arg(m_shots.size() + 1).arg(shot->transition));
        const double length = shot->to - shot->from;
        shot->length = m_shots.isEmpty() || shot->transition == "cut" ? 0 : std::clamp(o.value("length").toDouble(.5), 0.0, length);
        // A dissolve and a wipe overlap the shot before; a fade goes
        // through black between them, end to end.
        const double overlap = shot->transition == "dissolve" || shot->transition == "wipe" ? std::min(shot->length, m_shots.isEmpty() ? 0 : m_shots.last()->to - m_shots.last()->from) : 0;
        // No longer than the shot before can lend it, so it never mixes
        // over that shot held still at its end.
        if (shot->transition == "dissolve" || shot->transition == "wipe")
            shot->length = overlap;
        shot->start = at - overlap;
        at = shot->start + length;
        m_shots << shot;
    }
    if (m_shots.isEmpty())
        return fail(QString("%1 has no shots.").arg(path));
    m_fps = json.value("fps").toDouble(m_shots.first()->scene.fps);
    m_size = QSize(json.value("width").toInt(m_shots.first()->scene.width), json.value("height").toInt(m_shots.first()->scene.height));
    if (m_fps <= 0 || m_size.isEmpty())
        return fail("A film needs a frame rate and a size.");
    return true;
}

double Film::duration() const {
    return m_shots.isEmpty() ? 0 : m_shots.last()->start + m_shots.last()->to - m_shots.last()->from;
}

QImage Film::shotAt(Shot &shot, double t, const QSize &size) {
    // Fitted inside the film's frame, with black bars where shapes differ.
    const QSizeF fitted = QSizeF(shot.scene.width, shot.scene.height).scaled(QSizeF(size), Qt::KeepAspectRatio);
    const QSize inner(std::max(2, int(std::lround(fitted.width()))), std::max(2, int(std::lround(fitted.height()))));
    const QImage drawn = renderFrame(shot.scene, *shot.assets, std::clamp(shot.from + t, shot.from, shot.to), inner);
    if (inner == size)
        return drawn;
    QImage frame(size, QImage::Format_ARGB32_Premultiplied);
    frame.fill(Qt::black);
    QPainter painter(&frame);
    painter.drawImage(QPointF((size.width() - inner.width()) / 2.0, (size.height() - inner.height()) / 2.0), drawn);
    return frame;
}

QImage Film::frame(double t, const QSize &size) {
    QImage black(size, QImage::Format_ARGB32_Premultiplied);
    black.fill(Qt::black);
    // The latest shot that has started.
    int i = int(m_shots.size()) - 1;
    while (i > 0 && t < m_shots.at(i)->start)
        --i;
    Shot &shot = *m_shots.at(i);
    const double local = t - shot.start, length = shot.to - shot.from;
    QImage image = shotAt(shot, local, size);
    // Mid-way through a dissolve or a wipe from the shot before.
    if (i > 0 && shot.length > 0 && local < shot.length && (shot.transition == "dissolve" || shot.transition == "wipe")) {
        Shot &before = *m_shots.at(i - 1);
        QImage under = shotAt(before, t - before.start, size);
        const double into = local / shot.length;
        QPainter painter(&under);
        if (shot.transition == "wipe")
            painter.setClipRect(QRectF(0, 0, size.width() * into, size.height()));
        else
            painter.setOpacity(into);
        painter.drawImage(0, 0, image);
        return under;
    }
    // Fades through black: up from it at a fade's start, down to it before
    // a fade into the next shot.
    double light = 1;
    if (i > 0 && shot.transition == "fade" && shot.length > 0)
        light = std::min(light, local / (shot.length / 2));
    if (i + 1 < m_shots.size()) {
        const Shot &next = *m_shots.at(i + 1);
        if (next.transition == "fade" && next.length > 0)
            light = std::min(light, (length - local) / (next.length / 2));
    }
    light = std::clamp(light, 0.0, 1.0);
    if (light >= 1)
        return image;
    QPainter painter(&black);
    painter.setOpacity(light);
    painter.drawImage(0, 0, image);
    return black;
}

bool Film::addSound(const QString &video, QString *error) const {
    QStringList inputs, chains, labels;
    for (const auto &shot : m_shots) {
        const QJsonObject audio = shot->scene.raw.value("audio").toObject();
        const QString source = audio.value("source").toString();
        if (source.isEmpty())
            continue;
        const QString file = QDir(shot->path).absoluteFilePath(source);
        if (!QFileInfo::exists(file))
            continue;
        const double offset = std::max(0.0, audio.value("offset").toDouble());
        const int n = int(inputs.size()) / 2 + 1;
        inputs << "-i" << file;
        // Its stretch of the track, louder or softer, from where the shot
        // starts in the film.
        const qint64 delay = qint64(std::lround(std::max(0.0, shot->start) * 1000));
        chains << QString("[%1:a]atrim=start=%2:end=%3,asetpts=PTS-STARTPTS,volume=%4,adelay=%5:all=1[a%1]")
                      .arg(n)
                      .arg(offset + shot->from)
                      .arg(offset + shot->to)
                      .arg(std::clamp(audio.value("volume").toDouble(1), 0.0, 4.0))
                      .arg(delay);
        labels << QString("[a%1]").arg(n);
    }
    if (inputs.isEmpty())
        return true;
    const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
    if (ffmpeg.isEmpty()) {
        if (error)
            *error = "ffmpeg isn't installed.";
        return false;
    }
    const QFileInfo target(video);
    const QString partial = target.absoluteDir().filePath("." + target.completeBaseName() + ".sound." + target.suffix());
    const QString graph = chains.join(';') + ";" + labels.join("") + QString("amix=inputs=%1:normalize=0,apad,aresample=async=1[sound]").arg(labels.size());
    QStringList args{"-hide_banner", "-loglevel", "error", "-y", "-i", video};
    args << inputs << "-filter_complex" << graph << "-map" << "0:v" << "-map" << "[sound]" << "-c:v" << "copy"
         << "-c:a" << (target.suffix().toLower() == "webm" ? "libopus" : "aac") << "-t" << QString::number(duration()) << partial;
    QProcess process;
    process.start(ffmpeg, args);
    if (!process.waitForFinished(10 * 60 * 1000) || process.exitCode() != 0) {
        QFile::remove(partial);
        if (error)
            *error = "ffmpeg couldn't add the sound: " + QString::fromUtf8(process.readAllStandardError()).trimmed().left(300);
        return false;
    }
    QFile::remove(video);
    return QFile::rename(partial, video);
}
