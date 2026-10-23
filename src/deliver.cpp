#include "deliver.h"
#include "measure.h"
#include "scene.h"
#include "studio.h"

#include <QDate>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QSaveFile>
#include <cmath>

namespace {
QString seconds(double t) { return QString::number(t, 'f', 2) + "s"; }

bool copy(const QString &from, const QString &to, QString *error) {
    QDir().mkpath(QFileInfo(to).absolutePath());
    QFile::remove(to);
    if (!QFile::copy(from, to)) {
        *error = "Could not copy " + from + " to " + to;
        return false;
    }
    return true;
}

bool save(const QString &path, const QByteArray &text, QString *error) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(text) < 0 || !file.commit()) {
        *error = path + ": " + file.errorString();
        return false;
    }
    return true;
}

// One page per beat: its frame, its time, why it exists, its states, and
// every word it puts on screen.
bool storyboard(Scene &scene, const QString &root, const QString &path, QString *error) {
    QPdfWriter pdf(path);
    pdf.setPageSize(QPageSize(QPageSize::A4));
    pdf.setPageOrientation(QPageLayout::Landscape);
    pdf.setResolution(150);
    pdf.setTitle(QFileInfo(root.isEmpty() ? scene.path() : root).fileName() + " storyboard");
    QPainter painter;
    if (!painter.begin(&pdf)) {
        *error = "Could not write " + path;
        return false;
    }
    const QList<Studio::ShotListBeat> shots = root.isEmpty() ? QList<Studio::ShotListBeat>() : Studio::readShotList(root);
    const QList<Beat> beats = scene.beats();
    const QList<double> times = scene.sheetTimes();
    const QRect page = painter.viewport();
    QFont title = painter.font();
    title.setPixelSize(34);
    title.setBold(true);
    QFont body = title;
    body.setBold(false);
    body.setPixelSize(22);
    for (int i = 0; i < times.size(); ++i) {
        if (i > 0)
            pdf.newPage();
        const QImage frame = scene.frame(times[i]);
        const int width = page.width() * 0.58;
        const QRect picture(0, 0, width, width * frame.height() / frame.width());
        painter.drawImage(picture, frame);
        painter.setPen(QColor("#999999"));
        painter.drawRect(picture);

        const Beat beat = i < beats.size() ? beats[i] : Beat{times[i], QString(), {}, {}, {}};
        const double end = i + 1 < beats.size() ? beats[i + 1].at : scene.duration();
        QStringList lines;
        if (i < beats.size())
            lines << QString("%1 to %2").arg(seconds(beat.at), seconds(end));
        if (!beat.purpose.isEmpty())
            lines << "Why: " + beat.purpose;
        if (!beat.enter.isEmpty())
            lines << "Enters: " + beat.enter;
        if (!beat.exit.isEmpty())
            lines << "Exits: " + beat.exit;
        if (i < shots.size() && !shots[i].onScreen.isEmpty())
            lines << "On screen: " + shots[i].onScreen;
        const QRect text(picture.right() + 40, 0, page.width() - picture.width() - 40, page.height());
        painter.setPen(Qt::black);
        painter.setFont(title);
        painter.drawText(text, Qt::AlignTop | Qt::AlignLeft | Qt::TextWordWrap,
                         QString("%1  %2").arg(i + 1).arg(beat.name.isEmpty() ? seconds(times[i]) : beat.name));
        painter.setFont(body);
        painter.drawText(text.adjusted(0, 60, 0, 0), Qt::AlignTop | Qt::AlignLeft | Qt::TextWordWrap,
                         lines.join("\n\n"));
        painter.setPen(QColor("#777777"));
        painter.drawText(QRect(0, picture.bottom() + 20, picture.width(), 40), Qt::AlignLeft,
                         QString("%1   %2   frame %3").arg(scene.format().name, seconds(times[i]))
                             .arg(int(std::lround(times[i] * scene.fps()))));
    }
    painter.end();
    return true;
}
}

bool Deliver::write(const QString &scenePath, const QStringList &formats, const QString &folderPath,
                    QJsonObject *result, QString *error, const std::function<void(const QString &)> &progress) {
    auto say = [&](const QString &what) {
        if (progress)
            progress(what);
    };
    const QFileInfo info(scenePath);
    const QString root = Studio::rootOf(scenePath);
    const QDir base(root.isEmpty() ? info.absolutePath() : root);
    const QDir out(folderPath.isEmpty() ? base.filePath("deliverables") : folderPath);
    if (!QDir().mkpath(out.path())) {
        *error = "Could not create " + out.path();
        return false;
    }
    const QString name = root.isEmpty() ? info.completeBaseName() : QFileInfo(root).fileName();
    QJsonArray videos, posters, sheets;
    QStringList review;
    QList<Problem> problems;

    for (int i = 0; i < formats.size(); ++i) {
        Scene scene;
        if (!scene.load(scenePath, formats[i], error))
            return false;
        const QString slug = scene.format().slug();
        // Rendered where render keeps its cache, so only what changed is encoded.
        say("Rendering " + formats[i]);
        const QString rendered = Scene::defaultOutput(scene.path(), formats[i]);
        if (!scene.render(rendered, error))
            return false;
        const QString video = out.filePath(QString("videos/%1-%2.mp4").arg(name, slug));
        if (!copy(rendered, video, error))
            return false;
        videos << out.relativeFilePath(video);

        say("Posters and sheets for " + formats[i]);
        const QString poster = out.filePath(QString("posters/%1-%2.png").arg(name, slug));
        QDir().mkpath(QFileInfo(poster).absolutePath());
        if (!scene.frame((scene.frameCount() - 1) / double(scene.fps())).save(poster)) {
            *error = "Could not save " + poster;
            return false;
        }
        posters << out.relativeFilePath(poster);
        QList<Scene::Strip> strips;
        for (const auto &moment : Measure::moments(scenePath, formats[i], 3, error)) {
            const int at = int(std::lround(moment.at * scene.fps()));
            Scene::Strip strip;
            for (int f = qMax(0, at - 2); f <= qMin(scene.frameCount() - 1, at + 2); ++f)
                strip.times << f / double(scene.fps());
            strip.heading = QString("%1 at %2").arg(moment.stray ? "A frame unlike its neighbors" : "Fast change",
                                                    seconds(moment.at));
            strips << strip;
        }
        const QString sheet = out.filePath(QString("sheets/%1-%2-sheet.png").arg(name, slug));
        QDir().mkpath(QFileInfo(sheet).absolutePath());
        if (!scene.contactSheet(scene.sheetTimes(), 0, strips).save(sheet)) {
            *error = "Could not save " + sheet;
            return false;
        }
        sheets << out.relativeFilePath(sheet);
        if (i == 0) {
            say("Storyboard");
            if (!storyboard(scene, root, out.filePath("storyboard.pdf"), error))
                return false;
        }
        say("Checking " + formats[i]);
        for (const auto &problem : scene.check())
            if (i == 0 || !problem.format.isEmpty())
                problems << problem;
    }

    // The source: the scene, everything beside it that the film is made of,
    // and its assets, so the film can be rendered again elsewhere.
    say("Source");
    QStringList source;
    QDirIterator it(base.path(), QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = base.relativeFilePath(it.next());
        const QString top = path.section('/', 0, 0);
        if (top == "renders" || top == "reviews" || top == "deliverables" || top.startsWith('.') ||
            it.fileInfo().absoluteFilePath().startsWith(out.path() + "/"))
            continue;
        if (!copy(base.filePath(path), out.filePath("source/" + path), error))
            return false;
        source << path;
    }
    if (QFileInfo::exists(base.filePath("assets/assets.md")) &&
        !copy(base.filePath("assets/assets.md"), out.filePath("assets.md"), error))
        return false;

    // What still needs a person: approvals, open notes, what check found,
    // and the judgments no check makes.
    if (!root.isEmpty()) {
        for (const auto &gate : Studio::readGates(root))
            if (!gate.passed)
                review << QString("- Gate %1, %2: %3").arg(gate.number).arg(gate.title,
                                                                           gate.status.isEmpty() ? "not passed" : gate.status);
        for (const auto &note : Studio::readNotes(root))
            if (!note.done)
                review << QString("- Note %1 at %2 (%3) is open: %4").arg(note.id, seconds(note.at), note.format,
                                                                       note.text.simplified());
        for (const auto &asset : Studio::missingAssets(root))
            review << "- Asset still missing: " + asset;
    }
    for (const auto &p : problems)
        review << QString("- check %1%2%3: %4")
                      .arg(p.error ? "error" : "warning", p.format.isEmpty() ? QString() : " [" + p.format + "]",
                           p.at < 0 ? QString() : " at " + seconds(p.at), p.message);
    QString note = QString("# What still needs human review\n\n%1, delivered %2 in %3.\n\n")
                       .arg(name, QDate::currentDate().toString(Qt::ISODate), formats.join(", "));
    note += review.isEmpty() ? QString("omotion found nothing open: every gate passed, every note answered, and check "
                                       "clean.\n\n")
                             : review.join('\n') + "\n\n";
    note += "Judged by a person, always: whether the hook is a real one; whether the last frame is strong enough to be "
            "the poster; words inside pictures, at phone width; every claim on screen against the brief's facts; and "
            "the music's license.\n";
    if (!save(out.filePath("REVIEW.md"), note.toUtf8(), error))
        return false;

    int errors = 0;
    for (const auto &p : problems)
        errors += p.error;
    *result = {{"folder", out.path()}, {"videos", videos}, {"posters", posters}, {"sheets", sheets},
               {"storyboard", "storyboard.pdf"}, {"source", QJsonArray::fromStringList(source)},
               {"review", "REVIEW.md"}, {"open", review.size()}, {"errors", errors}};
    return true;
}
