#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

// A review round, as the article's critique asks for it: contact sheets and
// strips, everything omotion measures, scores, and the three largest defects,
// each with its time, a cropped frame as evidence, and room for the local fix.
// What can't be measured is left marked for the agent and the person to judge.
namespace Review {
// Writes reviews/NNN.md, numbered after the last, and its evidence in
// reviews/NNN/, beside the scene or in its studio. The result says where, the
// scores and defects, and whether the film is sent back: any error from check
// sends it back before a person needs to see it.
bool write(const QString &scenePath, const QStringList &formats, QJsonObject *result, QString *error);
}
