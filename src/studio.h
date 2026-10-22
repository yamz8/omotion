#pragma once

#include <QString>
#include <QStringList>

// The omotion release; the agent skill a project carries is stamped with it.
inline constexpr char omotionVersion[] = "0.1.0";

// A studio is one film's folder: the brief, style guide, shot list, assets,
// gates, a scene, and the agent skill that knows how to work in it, so the
// folder alone carries everything a later session needs.
namespace Studio {
// Creates the studio in an empty or new folder; never writes into a folder
// that already has files.
bool create(const QString &folder, QString *error, QStringList *created = nullptr);

// The studio a path is in: the nearest folder at or above it holding gates.md.
QString rootOf(const QString &path);

// Writes the bundled skill into the studio, and AGENTS.md when it has none.
bool installSkill(const QString &root, QString *error, QStringList *written = nullptr);

// The bundled skill as it is written into a project, stamped with the release
// and a hash of its text.
QByteArray stampedSkill();

// Empty when the studio's skill is the one this omotion would write, or the
// person took it over by removing its stamp; otherwise why it's out of date.
QString skillProblem(const QString &root);
}
