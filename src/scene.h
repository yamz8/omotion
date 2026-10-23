#pragma once

#include <QImage>
#include <QList>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>

class QQmlEngine;
class QQuickItem;
class QQuickWindow;

// The compositions a scene can be rendered in. Each one is laid out by the
// scene itself rather than cropped from another.
struct Format {
    QString name; // "16:9"
    QSize size;   // 1920x1080
    QString slug() const { return QString(name).replace(':', 'x'); }
};

namespace Formats {
QList<Format> all();
QStringList names();
// Accepts "16:9" and "16x9".
bool find(const QString &name, Format *format = nullptr);
}

// A moment the viewer should notice, and its contract with the shot list:
// why it exists, the state it starts from, and the state it must reach.
struct Beat {
    double at = 0;
    QString name;
    QString purpose;
    QString enter;
    QString exit;
};

struct Problem {
    bool error = true;
    QString message;
    QString format; // Empty when the problem is the scene's as a whole.
    double at = -1; // Seconds, or -1 when it isn't about one moment.
    QRectF area;    // Where in the frame, in its pixels, when it's about one place.
};

// The Omotion QML module (the Motion helpers), registered once per process.
void registerSceneTypes();

// One scene file, created in its own window at one format's size. Frames are
// asked for by time, in any order, which is the whole contract: a scene is a
// function of t.
class Scene {
public:
    Scene();
    ~Scene();

    // scale renders the same layout smaller, for scans that compare frames
    // rather than read them: the scene still lays out at the format's size.
    bool load(const QString &path, const QString &format, QString *error, double scale = 1);

    QString path() const { return m_path; }
    Format format() const { return m_format; }
    double duration() const { return m_duration; }
    int fps() const { return m_fps; }
    int frameCount() const;
    QList<Beat> beats() const { return m_beats; }
    // The formats the scene says it is laid out for; 16:9 when it doesn't say.
    QStringList formats() const { return m_formats; }
    // Absolute path of the soundtrack, or empty.
    QString audio() const { return m_audio; }

    QImage frame(double t);
    // Moves the scene to t and lays it out, without rendering, so its items
    // can be measured.
    void seek(double t);
    QQuickItem *root() const { return m_root; }

    // The moments a contact sheet shows: the middle of every beat, or eight
    // even steps through a scene without beats.
    QList<double> sheetTimes() const;
    QString beatNameAt(double t) const;
    // strips are extra rows of consecutive frames, such as the film's fastest
    // changes, each under its own heading.
    struct Strip {
        QString heading;
        QList<double> times;
    };
    QImage contactSheet(const QList<double> &times, int columns = 0, const QList<Strip> &strips = {});

    // Problems with the declarations and with this format's frames.
    QList<Problem> check();

    // Encodes every frame through ffmpeg into output, written beside it first
    // and renamed into place, so a failed render never leaves a broken file.
    bool render(const QString &output, QString *error,
                const std::function<void(int frame, int frames)> &progress = {});

    static QString defaultOutput(const QString &scenePath, const QString &format);
    static QString partialPath(const QString &output);

private:
    bool readDeclarations(QString *error);

    QString m_path;
    Format m_format;
    std::unique_ptr<QQmlEngine> m_engine;
    std::unique_ptr<QQuickWindow> m_window;
    QQuickItem *m_root = nullptr;
    double m_scale = 1;
    double m_duration = 0;
    int m_fps = 30;
    QList<Beat> m_beats;
    QStringList m_formats;
    QString m_audio;
    QStringList m_warnings;
    QList<Problem> m_declarationProblems;
};
