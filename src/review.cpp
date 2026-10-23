#include "review.h"
#include "measure.h"
#include "scene.h"
#include "studio.h"

#include <QDate>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QPainter>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <algorithm>
#include <cmath>

namespace {
QString seconds(double t) { return QString::number(t, 'f', 2) + "s"; }

bool save(const QImage &image, const QString &path) {
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && image.save(&file, "PNG") && file.commit();
}

// The kinds of problem a review scores, read from what check says.
bool aboutReading(const Problem &p) {
    return p.message.contains("px when the film plays") || p.message.contains("fully on screen for");
}
bool aboutPlacement(const Problem &p) {
    return p.message.contains("safe area") || p.message.contains("runs into") || p.message.contains("outside the");
}
bool aboutStyle(const Problem &p) {
    return p.message.contains("style-guide.md") || p.message.contains("state differs from shotlist.md");
}
bool aboutTime(const Problem &p) {
    return p.message.contains("rendered before it") || p.message.contains("out of order") ||
           p.message.contains("wall clock");
}
bool aboutHook(const Problem &p) {
    return p.message.startsWith("Frame 0 shows") || p.message.startsWith("Nothing visibly changes");
}

// Most serious first: errors, then what the viewer can't read, then where
// things sit, then the rest; earlier in the film first.
int rank(const Problem &p) {
    return (p.error ? 0 : 10) + (aboutReading(p) ? 0 : aboutPlacement(p) ? 1 : aboutHook(p) ? 2 : 3);
}

// The frame at the defect's moment, cropped around where it is with room to
// see what's beside it, and the place outlined.
QImage evidence(Scene &scene, const Problem &problem) {
    QImage frame = scene.frame(qMax(0.0, problem.at));
    if (problem.area.isEmpty())
        return frame.scaledToWidth(qMin(960, frame.width()), Qt::SmoothTransformation);
    const QRectF bounds(QPointF(0, 0), QSizeF(frame.size()));
    const QRectF area = problem.area & bounds;
    {
        QPainter painter(&frame);
        painter.setRenderHint(QPainter::Antialiasing);
        const double pen = qMax(4.0, frame.width() / 320.0);
        painter.setPen(QPen(QColor(0, 0, 0, 170), pen * 2));
        painter.drawRect(area.adjusted(-pen * 2, -pen * 2, pen * 2, pen * 2));
        painter.setPen(QPen(QColor("#FF2D78"), pen));
        painter.drawRect(area.adjusted(-pen * 2, -pen * 2, pen * 2, pen * 2));
    }
    QRectF crop = area.adjusted(-area.width() * 0.4 - 80, -area.height() * 0.6 - 80, area.width() * 0.4 + 80,
                                area.height() * 0.6 + 80);
    // At least a third of the frame, so the defect is seen in its place.
    const QSizeF least(frame.width() / 3.0, frame.height() / 3.0);
    if (crop.width() < least.width())
        crop.adjust(-(least.width() - crop.width()) / 2, 0, (least.width() - crop.width()) / 2, 0);
    if (crop.height() < least.height())
        crop.adjust(0, -(least.height() - crop.height()) / 2, 0, (least.height() - crop.height()) / 2);
    crop.translate(qMax(0.0, -crop.left()), qMax(0.0, -crop.top()));
    crop.translate(qMin(0.0, bounds.right() - crop.right()), qMin(0.0, bounds.bottom() - crop.bottom()));
    return frame.copy((crop & bounds).toAlignedRect());
}

int score(int start, double lost) { return qBound(1, int(std::lround(start - lost)), 5); }
}

bool Review::write(const QString &scenePath, const QStringList &formats, QJsonObject *result, QString *error) {
    const QFileInfo info(scenePath);
    const QString studio = Studio::rootOf(scenePath);
    const QDir base(studio.isEmpty() ? info.absolutePath() : studio);
    if (!base.mkpath("reviews")) {
        *error = "Could not create " + base.filePath("reviews");
        return false;
    }
    const QDir reviews(base.filePath("reviews"));

    // Numbered after the last review, which is never written over.
    int last = 0;
    static const QRegularExpression numbered(R"(^(\d{3})\.md$)");
    for (const auto &name : reviews.entryList({"*.md"}, QDir::Files))
        if (const auto match = numbered.match(name); match.hasMatch())
            last = qMax(last, match.captured(1).toInt());
    const QString number = QString("%1").arg(last + 1, 3, 10, QChar('0'));
    if (!reviews.mkpath(number)) {
        *error = "Could not create " + reviews.filePath(number);
        return false;
    }
    const QDir folder(reviews.filePath(number));
    auto relative = [&](const QString &path) { return base.relativeFilePath(path); };

    QList<Problem> problems;
    QStringList sheets, posters, moments;
    bool silent = true;
    for (int i = 0; i < formats.size(); ++i) {
        Scene scene;
        if (!scene.load(scenePath, formats[i], error))
            return false;
        silent = scene.audio().isEmpty();
        for (const auto &problem : scene.check())
            if (i == 0 || !problem.format.isEmpty())
                problems << problem;

        QList<Scene::Strip> strips;
        for (const auto &moment : Measure::moments(scenePath, formats[i], 3, error)) {
            const int at = int(std::lround(moment.at * scene.fps()));
            Scene::Strip strip;
            for (int f = qMax(0, at - 2); f <= qMin(scene.frameCount() - 1, at + 2); ++f)
                strip.times << f / double(scene.fps());
            strip.heading = QString("%1 at %2 (frame %3)")
                                .arg(moment.stray ? "A frame unlike its neighbors" : "Fast change", seconds(moment.at))
                                .arg(at);
            strips << strip;
            moments << QString("%1 %2 at %3").arg(formats[i], moment.stray ? "stray frame" : "fast change",
                                                  seconds(moment.at));
        }
        const QString slug = scene.format().slug();
        const QString sheet = folder.filePath(QString("%1-%2-sheet.png").arg(info.completeBaseName(), slug));
        if (!save(scene.contactSheet(scene.sheetTimes(), 0, strips), sheet)) {
            *error = "Could not save " + sheet;
            return false;
        }
        sheets << relative(sheet);
        // The last frame, as the poster it should be strong enough to be.
        const QString poster = folder.filePath(QString("poster-%1.png").arg(slug));
        if (!save(scene.frame((scene.frameCount() - 1) / double(scene.fps())), poster)) {
            *error = "Could not save " + poster;
            return false;
        }
        posters << relative(poster);
    }

    // The three largest defects, each with its own cropped frame.
    QList<Problem> timed;
    for (const auto &problem : problems)
        if (problem.at >= 0)
            timed << problem;
    std::stable_sort(timed.begin(), timed.end(), [](const Problem &a, const Problem &b) {
        return rank(a) != rank(b) ? rank(a) < rank(b) : a.at < b.at;
    });
    QJsonArray defects;
    QStringList defectLines;
    for (int i = 0; i < qMin<int>(3, timed.size()); ++i) {
        const Problem &problem = timed[i];
        const QString format = problem.format.isEmpty() ? formats.first() : problem.format;
        Scene scene;
        if (!scene.load(scenePath, format, error))
            return false;
        Format found;
        Formats::find(format, &found);
        const QString image = folder.filePath(QString("defect-%1-%2.png").arg(i + 1).arg(found.slug()));
        if (!save(evidence(scene, problem), image)) {
            *error = "Could not save " + image;
            return false;
        }
        defects << QJsonObject{{"at", problem.at}, {"format", format}, {"severity", problem.error ? "error" : "warning"},
                               {"message", problem.message}, {"evidence", relative(image)}};
        defectLines << QString("%1. **%2, %3:** %4\n   Evidence: %5\n   Local fix: ✎\n")
                           .arg(i + 1).arg(seconds(problem.at), format, problem.message, relative(image));
    }

    // Scores from what's measured, as a floor: the agent lowers any that its
    // own judgment of the frames says is worse.
    int errors = 0, reading = 0, placement = 0, style = 0, time = 0;
    // The same empty opening in every format is one weak hook.
    QSet<QString> hooks;
    for (const auto &p : problems)
        if (aboutHook(p))
            hooks << p.message;
    const int hook = hooks.size();
    for (const auto &p : problems) {
        errors += p.error;
        reading += aboutReading(p);
        placement += aboutPlacement(p);
        style += aboutStyle(p);
        time += aboutTime(p);
    }
    int strays = 0;
    for (const auto &m : moments)
        strays += m.contains("stray frame");
    const QJsonObject scores{{"hook", score(5, hook * 1.5)},
                             {"readability", score(5, reading + placement * 0.5)},
                             {"continuity", score(5, style)},
                             {"motion", score(5, strays + time * 2)},
                             {"sound_sync", silent ? QJsonValue("n/a") : QJsonValue("not measured")}};
    const bool sentBack = errors > 0;
    const QString verdict = sentBack
                                ? QString("Sent back: check reports %1 error%2 to fix before the person sees the film.")
                                      .arg(errors).arg(errors == 1 ? "" : "s")
                                : QString("Passes what omotion measures. The agent's judgment and the person's "
                                          "decide the rest.");

    QStringList found;
    for (const auto &p : problems)
        found << QString("- %1%2%3: %4")
                     .arg(p.error ? "error" : "warning", p.format.isEmpty() ? QString() : " [" + p.format + "]",
                          p.at < 0 ? QString() : " at " + seconds(p.at), p.message);

    QString text;
    text += QString("# Review %1\n\n").arg(number);
    text += QString("%1, %2, %3. Written by omotion review; complete each ✎.\n\n")
                .arg(QDate::currentDate().toString(Qt::ISODate), relative(info.absoluteFilePath()), formats.join(", "));
    text += "**Verdict:** " + verdict + "\n\n";
    text += "## Scores (1-5)\n\nMeasured, as a floor; lower any your judgment of the frames says is worse.\n\n";
    text += "| | Measured | ✎ Judged |\n| --- | --- | --- |\n";
    text += QString("| Hook | %1 | |\n| Readability | %2 | |\n| Continuity | %3 | |\n| Motion | %4 | |\n"
                    "| Sound sync | %5 | |\n\n")
                .arg(scores["hook"].toInt()).arg(scores["readability"].toInt()).arg(scores["continuity"].toInt())
                .arg(scores["motion"].toInt())
                .arg(silent ? "n/a (silent)" : "not measured yet");
    text += "## The three largest defects\n\n";
    text += defectLines.isEmpty() ? QString("None measured. ✎ The three largest you see in the sheets, each with its "
                                            "time, format, evidence, and local fix.\n\n")
                                  : defectLines.join("\n") + "\n";
    text += "## Everything check found\n\n" + (found.isEmpty() ? QString("Nothing.") : found.join("\n")) + "\n\n";
    text += "## Evidence\n\n";
    text += "- Contact sheets, with strips of the fastest changes: " + sheets.join(", ") + "\n";
    if (!moments.isEmpty())
        text += "- Strips: " + moments.join("; ") + "\n";
    text += "- Last frames, as posters: " + posters.join(", ") + "\n\n";
    text += "## ✎ Still to judge\n\n"
            "- Each beat reaching its exit state in shotlist.md.\n"
            "- Real product assets, and no claim inside them the brief doesn't make.\n"
            "- Words inside pictures, at phone width.\n"
            "- Whether the hook is a real one, and the last frame strong enough to be the poster.\n"
            "- After each move, the viewer knows where to look.\n";

    const QString path = reviews.filePath(number + ".md");
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(text.toUtf8()) < 0 || !file.commit()) {
        *error = path + ": " + file.errorString();
        return false;
    }
    QJsonArray problemList;
    for (const auto &p : problems)
        problemList << QJsonObject{{"severity", p.error ? "error" : "warning"},
                                   {"format", p.format.isEmpty() ? QJsonValue() : QJsonValue(p.format)},
                                   {"at", p.at < 0 ? QJsonValue() : QJsonValue(p.at)},
                                   {"message", p.message}};
    *result = {{"review", QFileInfo(path).absoluteFilePath()}, {"number", number}, {"sent_back", sentBack},
               {"verdict", verdict}, {"scores", scores}, {"defects", defects}, {"problems", problemList},
               {"sheets", QJsonArray::fromStringList(sheets)}, {"posters", QJsonArray::fromStringList(posters)}};
    return true;
}
