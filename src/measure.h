#pragma once

#include "scene.h"

#include <QImage>
#include <QList>
#include <QString>

// The review checklist, measured: what a person would otherwise judge by
// looking at stills, read from the scene's items at every frame (which needs
// no rendering) or from frames rendered small.
namespace Measure {
// The smallest readable text in a feed: 10 px when the film plays 390 px wide.
inline constexpr double phoneWidth = 390;
inline constexpr double readablePixels = 10;

// The frame minus what each platform covers with its own controls.
QRectF safeArea(const Format &format);

// Every text at every frame: too small at phone width, not held long enough
// to read, outside the safe area, or running into other text or into the edge
// of something drawn.
QList<Problem> text(Scene &scene);

// Colors and fonts the scene uses that style-guide.md doesn't name, each with
// the beat it first appears in. Empty when the guide is missing or a template.
QList<Problem> style(Scene &scene, const QString &styleGuide);

// Frame 0, often the thumbnail, showing nothing but one color, and nothing
// changing in the first two seconds.
QList<Problem> hook(Scene &scene);

// How much each frame differs from the one before, from 0 (identical) to 1,
// rendered small; the first frame's change is 0.
QList<double> changes(Scene &small, QList<double> *flashes = nullptr);
// flashes, when asked for, holds for each frame how much more it differs
// from its neighbors than they differ from each other: a frame that doesn't
// belong between them, such as a doubled or half-drawn one.

// The fastest changes in a film, as times where a strip should look: at most
// count of them, each the peak of its own moment, at least gap seconds apart.
QList<double> fastestChanges(const QList<double> &changes, int fps, int count = 3, double gap = 0.5);
}
