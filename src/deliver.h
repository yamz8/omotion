#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <functional>

// Everything a finished film hands over, from one command: the videos and a
// poster per format, contact sheets, a storyboard PDF, the source with its
// assets, the asset list, and a note on what still needs a person's review.
namespace Deliver {
// Writes them into folder (by default deliverables/ in the film's studio),
// rendering each format first; progress says what it's doing.
bool write(const QString &scenePath, const QStringList &formats, const QString &folder, QJsonObject *result,
           QString *error, const std::function<void(const QString &)> &progress = {});
}
