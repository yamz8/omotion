#pragma once

// Renders every frame of a scene and pipes them into ffmpeg, or writes them
// out as numbered PNGs. Works on a copy of the scene, so editing during an
// export doesn't change what comes out.

#include <QObject>
#include <QProcess>
#include <QString>

#include "scene.h"

class Exporter : public QObject {
    Q_OBJECT

public:
    explicit Exporter(QObject *parent = nullptr) : QObject(parent) {}

    static const QStringList &formats();
    // "mp4", "webm", "gif", or "png" for a folder of frames; from the output's
    // extension, with no extension meaning frames.
    static QString formatFor(const QString &output);

    bool busy() const { return m_busy; }
    // Returns false (and emits nothing) when it can't start, with the reason
    // in error().
    // scale shrinks (or grows) the frames from the canvas size, for smaller files.
    bool start(const Scene &scene, const QString &directory, const QString &output, double scale = 1);
    void cancel();
    QString error() const { return m_error; }

    // Runs an export to the end on a local event loop; for the command line.
    static bool run(const Scene &scene, const QString &directory, const QString &output, QString *error,
                    double scale = 1);

signals:
    void progress(double fraction);
    void finished(bool ok, const QString &message);

private:
    void feed();
    void writeFrames();
    void done(bool ok, const QString &message);

    Scene m_scene;
    AssetCache m_assets;
    QProcess *m_process = nullptr;
    QString m_output;
    QString m_partial;
    QString m_format;
    QString m_error;
    QSize m_size;
    int m_next = 0;
    int m_count = 0;
    bool m_busy = false;
    bool m_cancelled = false;
};
