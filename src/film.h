#pragma once

// A film: shots in a row, each an omotion project (or a stretch of one),
// joined by cuts, dissolves, fades through black or wipes, as Blender's
// sequencer does. Kept as a small JSON file beside the projects:
//
//   {"fps": 30, "shots": [
//       {"project": "arrival.omotion", "from": 0, "to": 3},
//       {"project": "jump.omotion", "transition": "dissolve", "length": 0.5}]}
//
// The film's size is its "width" and "height", or the first shot's; shots
// of another shape are fitted inside it, with bars.

#include <QImage>
#include <QList>
#include <QString>
#include <memory>

#include "scene.h"

class Film {
public:
    // Reads a film file; false, with why, if it can't be used.
    bool load(const QString &path, QString *error);

    double fps() const { return m_fps; }
    QSize size() const { return m_size; }
    double duration() const;
    int frameCount() const { return int(std::lround(duration() * m_fps)); }
    // The film's frame at a time, at a size.
    QImage frame(double t, const QSize &size);

private:
    struct Shot {
        QString path;
        Scene scene;
        std::unique_ptr<AssetCache> assets;
        double from = 0, to = 0;
        QString transition = "cut";
        double length = 0;  // Of the transition into it.
        double start = 0;   // Where it starts in the film.
    };
    QImage shotAt(Shot &shot, double t, const QSize &size);
    QList<std::shared_ptr<Shot>> m_shots;
    double m_fps = 30;
    QSize m_size;
};
