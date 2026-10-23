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

// A sound or moment on the timeline: a synthesized sound to play there, or
// just a name, and the beat it must land on, if any.
struct SceneCue {
    double at = 0;
    QString name;
    QString sound; // "click", "tick", "hit", "whoosh", or empty.
    QString on;    // A beat's name, or empty to pair it with what changes nearest.
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
    // Seconds cut from the start of the track, and seconds of film before it
    // starts.
    double audioTrim() const { return m_audioTrim; }
    double audioDelay() const { return m_audioDelay; }
    // The film time at which the track plays the given second of itself, for
    // a second the trim keeps.
    double filmTime(double trackTime) const { return trackTime - m_audioTrim + m_audioDelay; }
    QList<SceneCue> cues() const { return m_cues; }

    // The seed every scene this process loads gets, as its "seed" property
    // when it declares one: render --seed 3 is the third repeatable variation.
    static void setSeed(int seed);
    static int seed();

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

    // Problems with the declarations and with this format's frames. thorough
    // asks two more copies for every frame, one in order and one shuffled,
    // instead of only the beats in opposite orders.
    QList<Problem> check(bool thorough = false);

    // Encodes every frame through ffmpeg into output, written beside it first
    // and renamed into place, so a failed render never leaves a broken file.
    // Each beat is encoded on its own and kept in renders/.cache, so a render
    // after a patch encodes again only the beats whose frames changed; fresh
    // encodes every beat.
    bool render(const QString &output, QString *error,
                const std::function<void(int frame, int frames)> &progress = {}, bool fresh = false);
    // Frames the last render took from the cache.
    int reusedFrames() const { return m_reusedFrames; }
    // The state of every visible item at t, hashed: frames with the same key
    // look the same. Empty when something on screen paints in a way its
    // properties don't show, such as a Canvas.
    QByteArray frameKey(double t);

    static QString defaultOutput(const QString &scenePath, const QString &format);
    static QString partialPath(const QString &output);
    QString cachePath(const QString &output) const;

private:
    bool readDeclarations(QString *error);

    QString m_path;
    Format m_format;
    std::unique_ptr<QQmlEngine> m_engine;
    std::unique_ptr<QQuickWindow> m_window;
    QQuickItem *m_root = nullptr;
    double m_scale = 1;
    int m_reusedFrames = 0;
    double m_duration = 0;
    int m_fps = 30;
    QList<Beat> m_beats;
    QStringList m_formats;
    QString m_audio;
    double m_audioTrim = 0;
    double m_audioDelay = 0;
    QList<SceneCue> m_cues;
    QStringList m_warnings;
    QList<Problem> m_declarationProblems;
};
