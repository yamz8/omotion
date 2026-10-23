#pragma once

#include <QHash>
#include <QObject>
#include <QUrl>

class QQmlEngine;

// Video in a scene, frame-accurately: each clip is decoded once, at the
// scene's frame rate, into numbered frames under renders/.cache/clips, so a
// frame is chosen by its number rather than found by seeking, and any moment
// can be asked for in any order. Used through the Clip item:
//
//   Clip { video: "assets/footage.mp4"; time: scene.t - 2; width: 960; height: 540 }
class Clips : public QObject {
    Q_OBJECT

public:
    explicit Clips(const QString &scenePath, QObject *parent = nullptr) : QObject(parent), m_scene(scenePath) {}

    // The frame of the clip showing at time seconds into it, at fps; the first
    // before it starts and the last after it ends. Empty when it can't be read.
    Q_INVOKABLE QUrl frame(const QString &clip, double time, int fps);
    // How long the clip runs, in seconds.
    Q_INVOKABLE double length(const QString &clip, int fps);

    // Lets scenes in this engine resolve clips beside scenePath.
    static void use(QQmlEngine *engine, const QString &scenePath);

private:
    struct Decoded {
        QString folder;
        int frames = 0;
    };
    const Decoded *decode(const QString &clip, int fps);

    QString m_scene;
    QHash<QString, Decoded> m_decoded;
};
