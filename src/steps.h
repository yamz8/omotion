#pragma once

// A layer's AI steps: how it came to be (cut from another, the hole it left
// filled), and what the world made of it (the world itself, a 3D shape).
// Each says what it was made from, so when that changes the step is out of
// date; any step can be done again, and what's out of date redone in order.

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

#include "scene.h"

namespace Steps {

struct Step {
    QString name;    // "cut", "fill", "world" or "shape".
    QString title;   // What it is, in words.
    QString detail;  // What it was made from, and how.
    bool done = true;    // False for a step that could be made but isn't.
    bool stale = false;  // Made from something that has changed since.
    QString why;         // Why it's out of date.
    // What can be done to it: "reshape" (the cut's outline), "ai",
    // "smooth" or "none" (how the hole is filled), "make" or "again".
    QStringList actions;
    // Whether an action is slow work for the AI, best off the window's thread.
    static bool slow(const QString &step, const QString &action);
};

QList<Step> of(const Scene &scene, AssetCache &assets, int index);

// Does a step's action on a layer.
bool run(Scene &scene, AssetCache &assets, int index, const QString &step, const QString &action, QString *error);

// What's out of date across the scene, in the order to redo it: the world
// before the shapes fitted in it.
QList<QPair<int, QString>> stale(const Scene &scene, AssetCache &assets);
// Redoes all of it; done lists what was, in words.
bool redoStale(Scene &scene, AssetCache &assets, QString *error, QStringList *done = nullptr);

}
