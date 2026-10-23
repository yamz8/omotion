#include "cli.h"
#include "deliver.h"
#include "measure.h"
#include "reference.h"
#include "review.h"
#include "sound.h"
#include "scene.h"
#include "studio.h"

#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QRegularExpression>
#include <QUrl>
#include <QSaveFile>
#include <cmath>
#include <cstdio>

namespace {
void print(FILE *stream, const QString &text) { fprintf(stream, "%s\n", qPrintable(text)); }
void print(const QJsonObject &object, bool compact = false) {
    fprintf(stdout, "%s%s", QJsonDocument(object).toJson(compact ? QJsonDocument::Compact : QJsonDocument::Indented)
                                .constData(), compact ? "\n" : "");
    fflush(stdout);
}
int fail(const QString &message) {
    print(stderr, message);
    return 1;
}
QString seconds(double t) { return QString::number(t, 'f', 2) + "s"; }
// A path ending in a slash, naming a folder that exists, or without an
// extension is a folder for the file to go in.
bool namesDirectory(const QString &path) {
    return path.endsWith('/') || QFileInfo(path).isDir() || QFileInfo(path).suffix().isEmpty();
}
// Written beside and renamed into place, so a window watching the folder
// never loads half a PNG.
bool savePng(const QImage &image, const QString &path) {
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && image.save(&file, "PNG") && file.commit();
}
// A file name given for several formats keeps its name, with the format added:
// proof.png becomes proof-16x9.png, so one format never overwrites another,
// and no other file is touched.
QString withFormat(const QString &path, const QString &slug) {
    const QFileInfo info(path);
    const QString suffix = info.suffix().isEmpty() ? QString() : "." + info.suffix();
    return info.dir().filePath(info.completeBaseName() + "-" + slug + suffix);
}

QJsonObject toJson(const Problem &problem) {
    return {{"severity", problem.error ? "error" : "warning"},
            {"format", problem.format.isEmpty() ? QJsonValue() : QJsonValue(problem.format)},
            {"at", problem.at < 0 ? QJsonValue() : QJsonValue(problem.at)},
            {"message", problem.message}};
}

struct Command {
    QCommandLineParser parser;
    Command(const QString &name, const QString &description, bool takesScene = true) {
        parser.setApplicationDescription(description);
        parser.addHelpOption();
        parser.addPositionalArgument(name, description, name);
        if (takesScene)
            parser.addPositionalArgument("scene", "Scene file (.qml)", "<scene>");
    }
    void json() { parser.addOption({"json", "Print the result as JSON"}); }
    // A variation of the scene, for scenes that declare "property int seed".
    void seed() { parser.addOption({"seed", "Set the scene's seed property, for a repeatable variation", "number"}); }
    bool applySeed() const {
        if (!parser.isSet("seed"))
            return true;
        bool ok = false;
        const int value = parser.value("seed").toInt(&ok);
        if (!ok) {
            print(stderr, "Give the seed as a whole number, such as --seed 3.");
            return false;
        }
        Scene::setSeed(value);
        return true;
    }
    void format(const QString &fallback) {
        parser.addOption({"format", Formats::names().join(", ") + ", or all (default: " + fallback + ")", "format"});
    }
    QString argument(int index) const { return parser.positionalArguments().value(index); }
    QString seedSuffix() const { return parser.isSet("seed") ? "-seed" + parser.value("seed") : QString(); }
    QString scene() const { return argument(1); }

    // The formats asked for, checked against the scene's own list when "all".
    bool formats(bool allByDefault, QStringList *formats) const {
        if (!applySeed())
            return false;
        if (scene().isEmpty()) {
            print(stderr, "Name a scene file.");
            return false;
        }
        Scene probe;
        QString error;
        if (!probe.load(scene(), "16:9", &error)) {
            print(stderr, error);
            return false;
        }
        const QString wanted = parser.value("format");
        if (wanted.isEmpty())
            *formats = allByDefault ? probe.formats() : QStringList{probe.formats().first()};
        else if (wanted == "all")
            *formats = probe.formats();
        else {
            formats->clear();
            for (const auto &name : wanted.split(',', Qt::SkipEmptyParts)) {
                Format format;
                if (!Formats::find(name, &format)) {
                    print(stderr, QString("Unknown format %1; use %2, or all.").arg(name, Formats::names().join(", ")));
                    return false;
                }
                *formats << format.name;
            }
        }
        return true;
    }
};

int create(const QStringList &arguments) {
    Command command("new", "Start a film: a folder with the brief, style guide, shot list, assets, gates, a "
                           "starter scene, and the agent skill. Name a .qml file instead for a lone scene.", false);
    command.parser.addPositionalArgument("folder", "New folder for the film, or a .qml file", "<folder>");
    command.json();
    command.parser.process(arguments);
    const QString path = command.argument(1);
    if (path.isEmpty())
        return fail("Name the folder for the film, such as ~/Videos/launch.");

    if (QFileInfo(path).suffix().compare("qml", Qt::CaseInsensitive) == 0) {
        if (QFileInfo::exists(path))
            return fail(path + " already exists.");
        QFile starter(":/starter.qml");
        if (!starter.open(QIODevice::ReadOnly))
            return 1;
        if (!QDir().mkpath(QFileInfo(path).absolutePath()))
            return fail("Could not create " + QFileInfo(path).absolutePath());
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(starter.readAll()) < 0 || !file.commit())
            return fail(path + ": " + file.errorString());
        if (command.parser.isSet("json"))
            print({{"scene", QFileInfo(path).absoluteFilePath()}});
        else
            print(stdout, "Created " + path);
        return 0;
    }

    QString error;
    QStringList created;
    if (!Studio::create(path, &error, &created))
        return fail(error);
    const QString root = QFileInfo(path).absoluteFilePath();
    if (command.parser.isSet("json")) {
        print({{"studio", root}, {"scene", QDir(root).filePath("scene.qml")},
               {"files", QJsonArray::fromStringList(created)}});
        return 0;
    }
    print(stdout, "Created the film " + path + ":");
    for (const auto &file : created)
        print(stdout, "  " + file);
    print(stdout, "\nNext, open your coding agent in " + path + " and ask it for the film you want.\n"
                  "To watch it as it's made: omotion open " + QDir(path).filePath("scene.qml"));
    return 0;
}

int check(const QStringList &arguments) {
    Command command("check", "Report every problem with a scene: its declarations, clocks that break seeking, "
                             "and text outside the frame in each format.");
    command.parser.addOption({"thorough", "Also ask for every frame, in order and shuffled, not just the beats"});
    command.format("all of the scene's formats");
    command.seed();
    command.json();
    command.parser.process(arguments);
    QStringList formats;
    if (!command.formats(true, &formats))
        return 1;

    QList<Problem> problems;
    QJsonObject info;
    for (int i = 0; i < formats.size(); ++i) {
        Scene scene;
        QString error;
        if (!scene.load(command.scene(), formats[i], &error))
            return fail(error);
        if (i == 0) {
            QJsonArray beats;
            for (const auto &beat : scene.beats())
                beats << QJsonObject{{"at", beat.at}, {"name", beat.name}, {"purpose", beat.purpose},
                                     {"enter", beat.enter}, {"exit", beat.exit}};
            info = {{"scene", scene.path()}, {"duration", scene.duration()}, {"fps", scene.fps()},
                    {"frames", scene.frameCount()}, {"beats", beats},
                    {"formats", QJsonArray::fromStringList(scene.formats())},
                    {"audio", scene.audio().isEmpty() ? QJsonValue() : QJsonValue(scene.audio())}};
        }
        // The declarations are the same in every format, so report them once.
        for (const auto &problem : scene.check(command.parser.isSet("thorough")))
            if (i == 0 || !problem.format.isEmpty())
                problems << problem;
    }

    int errors = 0;
    for (const auto &problem : problems)
        errors += problem.error;
    if (command.parser.isSet("json")) {
        QJsonArray list;
        for (const auto &problem : problems)
            list << toJson(problem);
        info.insert("ok", errors == 0);
        info.insert("problems", list);
        print(info);
        return errors ? 1 : 0;
    }
    for (const auto &problem : problems)
        print(problem.error ? stderr : stdout,
              QString("%1: %2%3%4: %5")
                  .arg(command.scene(), problem.error ? "error" : "warning",
                       problem.format.isEmpty() ? QString() : " [" + problem.format + "]",
                       problem.at < 0 ? QString() : " at " + seconds(problem.at), problem.message));
    print(stdout, QString("%1, %2 fps, %3 beats, %4: %5 errors, %6 warnings")
                      .arg(seconds(info["duration"].toDouble()))
                      .arg(info["fps"].toInt())
                      .arg(info["beats"].toArray().size())
                      .arg(formats.join(" "))
                      .arg(errors)
                      .arg(problems.size() - errors));
    return errors ? 1 : 0;
}

int frame(const QStringList &arguments) {
    Command command("frame", "Render the frame at one moment to a PNG.");
    command.parser.addOption({"at", "Time in seconds", "seconds"});
    command.parser.addOption({{"o", "output"}, "PNG file, or a folder for it (default: <scene>-<format>-<seconds>.png)", "path"});
    command.parser.addOption({"width", "Scale the image to this width", "pixels"});
    command.parser.addOption({"mark", "Ring a point of the frame, in its pixels, such as 1100,446", "x,y"});
    command.format("the scene's first format");
    command.seed();
    command.json();
    command.parser.process(arguments);
    QStringList formats;
    if (!command.formats(false, &formats))
        return 1;
    if (formats.size() != 1)
        return fail("A frame is in one format; name one with --format.");
    bool ok = false;
    const double at = command.parser.value("at").toDouble(&ok);
    if (!ok)
        return fail("Say which moment with --at <seconds>.");

    Scene scene;
    QString error;
    if (!scene.load(command.scene(), formats.first(), &error))
        return fail(error);
    if (at < 0 || at > scene.duration())
        return fail(QString("%1 is outside the scene's %2.").arg(seconds(at), seconds(scene.duration())));
    QImage image = scene.frame(at);
    if (command.parser.isSet("mark")) {
        const QStringList point = command.parser.value("mark").split(',');
        bool x = false, y = false;
        const QPointF center(point.value(0).toDouble(&x), point.value(1).toDouble(&y));
        if (point.size() != 2 || !x || !y)
            return fail("Give the point to mark as x,y in the frame's pixels, such as 1100,446.");
        // A ring wide enough to surround what it points at rather than cover it,
        // in a pink films rarely use, with a dark edge to read on light frames.
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        const double radius = qMin(image.width(), image.height()) * 0.075;
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(0, 0, 0, 170), radius * 0.16));
        painter.drawEllipse(center, radius, radius);
        painter.setPen(QPen(QColor("#FF2D78"), radius * 0.08));
        painter.drawEllipse(center, radius, radius);
    }
    if (command.parser.isSet("width")) {
        const int width = command.parser.value("width").toInt();
        if (width < 16 || width > 7680)
            return fail("Width must be between 16 and 7680.");
        image = image.scaledToWidth(width, Qt::SmoothTransformation);
    }
    const QString name = QString("%1-%2-%3.png").arg(QFileInfo(scene.path()).completeBaseName(),
                                                     scene.format().slug(), QString::number(at, 'f', 2));
    QString output = command.parser.value("output");
    if (output.isEmpty())
        output = name;
    else if (namesDirectory(output))
        output = QDir(output).filePath(name);
    QDir().mkpath(QFileInfo(output).absolutePath());
    if (!savePng(image, output))
        return fail("Could not save " + output);
    if (command.parser.isSet("json"))
        print({{"image", QFileInfo(output).absoluteFilePath()}, {"at", at}, {"format", scene.format().name},
               {"frame", int(std::lround(at * scene.fps()))}, {"beat", scene.beatNameAt(at)},
               {"width", image.width()}, {"height", image.height()}});
    else
        print(stdout, "Rendered " + output);
    return 0;
}

int sheet(const QStringList &arguments) {
    Command command("sheet", "Render a contact sheet: one frame from the middle of every beat, or the moments "
                             "named with --times, labelled with beat, time, and frame.");
    command.parser.addOption({"times", "Comma-separated seconds to show instead of the beats", "list"});
    command.parser.addOption({{"o", "output"}, "PNG file (with the format added when there are several), or a folder (default: <scene>-<format>-sheet.png beside the scene)", "path"});
    command.parser.addOption({"columns", "Tiles per row", "count"});
    command.parser.addOption({"strips", "Strips of the film's fastest changes to add below the beats, five frames "
                                        "each (default: 3; 0 for none)", "count"});
    command.format("the scene's first format");
    command.seed();
    command.json();
    command.parser.process(arguments);
    QStringList formats;
    if (!command.formats(false, &formats))
        return 1;

    QJsonArray sheets;
    for (const auto &name : formats) {
        Scene scene;
        QString error;
        if (!scene.load(command.scene(), name, &error))
            return fail(error);
        QList<double> times = scene.sheetTimes();
        if (command.parser.isSet("times")) {
            times.clear();
            for (const auto &value : command.parser.value("times").split(',', Qt::SkipEmptyParts)) {
                bool ok = false;
                const double t = value.toDouble(&ok);
                if (!ok || t < 0 || t > scene.duration())
                    return fail(QString("%1 is not a time within the scene's %2.").arg(value, seconds(scene.duration())));
                times << t;
            }
        }
        if (times.isEmpty())
            return fail("No moments to show.");
        // The fastest changes, found by comparing every frame rendered small,
        // are where a half-drawn or doubled frame hides between the beats.
        QList<Scene::Strip> strips;
        QJsonArray stripList;
        const int stripCount = command.parser.isSet("strips") ? command.parser.value("strips").toInt() : 3;
        if (!command.parser.isSet("times") && stripCount > 0) {
            const QList<Measure::Moment> moments = Measure::moments(command.scene(), name, stripCount, &error);
            if (!error.isEmpty())
                return fail(error);
            for (const auto &moment : moments) {
                const double t = moment.at;
                const QString kind = moment.stray ? "A frame unlike its neighbors" : "Fast change";
                const int at = int(std::lround(t * scene.fps()));
                Scene::Strip strip;
                for (int f = qMax(0, at - 2); f <= qMin(scene.frameCount() - 1, at + 2); ++f)
                    strip.times << f / double(scene.fps());
                strip.heading = QString("%1 at %2 (frame %3)%4")
                                    .arg(kind, seconds(t)).arg(at)
                                    .arg(scene.beatNameAt(t).isEmpty() ? QString() : ", in " + scene.beatNameAt(t));
                strips << strip;
                QJsonArray frames;
                for (double s : strip.times)
                    frames << s;
                stripList << QJsonObject{{"at", t}, {"frame", at}, {"kind", moment.stray ? "flash" : "change"},
                                         {"times", frames}};
            }
        }
        const QString file = QString("%1-%2-sheet.png").arg(QFileInfo(scene.path()).completeBaseName(),
                                                            scene.format().slug());
        QString output = command.parser.value("output");
        if (output.isEmpty())
            output = QFileInfo(scene.path()).absoluteDir().filePath(file);
        else if (namesDirectory(output))
            output = QDir(output).filePath(file);
        else if (formats.size() > 1)
            output = withFormat(output, scene.format().slug());
        QDir().mkpath(QFileInfo(output).absolutePath());
        if (!savePng(scene.contactSheet(times, command.parser.value("columns").toInt(), strips), output))
            return fail("Could not save " + output);
        QJsonArray tiles;
        for (double t : times)
            tiles << QJsonObject{{"at", t}, {"frame", int(std::lround(t * scene.fps()))}, {"beat", scene.beatNameAt(t)}};
        sheets << QJsonObject{{"image", QFileInfo(output).absoluteFilePath()}, {"format", name}, {"tiles", tiles},
                              {"fast_changes", stripList}};
        if (!command.parser.isSet("json"))
            print(stdout, "Rendered " + output);
    }
    if (command.parser.isSet("json"))
        print({{"sheets", sheets}});
    return 0;
}

int render(const QStringList &arguments) {
    Command command("render", "Encode the scene to MP4 through ffmpeg, once per format, from the source rather "
                              "than a crop.");
    command.parser.addOption({{"o", "output"}, "MP4 file (with the format added when there are several), or a folder "
                                               "(default: renders/<scene>-<format>.mp4 beside the scene)", "path"});
    command.parser.addOption({"progress", "Print a JSON line per frame"});
    command.parser.addOption({"fresh", "Encode every beat again, rather than only the beats whose frames changed"});
    command.format("all of the scene's formats");
    command.seed();
    command.json();
    command.parser.process(arguments);
    QStringList formats;
    if (!command.formats(true, &formats))
        return 1;
    const bool progress = command.parser.isSet("progress");
    const QString output = command.parser.value("output");

    QJsonArray outputs;
    for (int i = 0; i < formats.size(); ++i) {
        Scene scene;
        QString error;
        if (!scene.load(command.scene(), formats[i], &error))
            return fail(error);
        QString target = Scene::defaultOutput(scene.path(), formats[i]);
        // A variation never replaces the film.
        if (!command.seedSuffix().isEmpty())
            target = QFileInfo(target).absoluteDir().filePath(QFileInfo(target).completeBaseName() +
                                                              command.seedSuffix() + ".mp4");
        if (!output.isEmpty())
            target = namesDirectory(output) ? QDir(output).filePath(QFileInfo(target).fileName())
                     : formats.size() == 1 ? output
                                           : withFormat(output, scene.format().slug());
        const QString format = formats[i];
        const int count = formats.size();
        auto report = [&](int frame, int frames) {
            if (progress)
                print({{"format", format}, {"index", i}, {"count", count}, {"frame", frame}, {"frames", frames}}, true);
        };
        if (!scene.render(target, &error, report, command.parser.isSet("fresh")))
            return fail(error);
        outputs << QJsonObject{{"format", format}, {"video", QFileInfo(target).absoluteFilePath()},
                               {"frames", scene.frameCount()}, {"duration", scene.frameCount() / double(scene.fps())},
                               {"reused_frames", scene.reusedFrames()}};
        if (!command.parser.isSet("json") && !progress)
            print(stdout, scene.reusedFrames() == 0
                              ? "Rendered " + target
                              : QString("Rendered %1, encoding %2 of %3 frames again")
                                    .arg(target).arg(scene.frameCount() - scene.reusedFrames()).arg(scene.frameCount()));
    }
    if (command.parser.isSet("json") || progress)
        print({{"outputs", outputs}}, progress);
    return 0;
}

int status(const QStringList &arguments) {
    Command command("status", "Say which gate the film has reached, what each gate's evidence still lacks, and "
                              "which notes are open, from the film's files.", false);
    command.parser.addPositionalArgument("folder", "The film's folder (default: the one you're in)", "[folder]");
    command.json();
    command.parser.process(arguments);
    const QString from = command.argument(1).isEmpty() ? QDir::currentPath() : command.argument(1);
    const QString root = Studio::rootOf(from);
    if (root.isEmpty())
        return fail(from + " isn't inside a film made by omotion. Start one with: omotion new <folder>");

    // Gate 6 needs a render per format, which only the scene knows.
    QStringList formats;
    QString sceneError;
    Scene scene;
    if (scene.load(QDir(root).filePath("scene.qml"), "16:9", &sceneError))
        formats = scene.formats();

    const QList<Studio::Gate> gates = Studio::readGates(root);
    QJsonArray gateList;
    QStringList problems;
    const Studio::Gate *current = nullptr;
    for (const auto &gate : gates) {
        QStringList missing = Studio::missingEvidence(root, gate.number, formats);
        if (gate.number == 6 && !sceneError.isEmpty())
            missing.prepend("scene.qml doesn't load: " + sceneError);
        if (!gate.passed && !current)
            current = &gate;
        // A passed gate whose evidence is gone can't be trusted until it's back.
        if (gate.passed)
            for (const auto &item : missing)
                problems << QString("Gate %1 is passed, but %2.").arg(gate.number).arg(item);
        gateList << QJsonObject{{"number", gate.number}, {"title", gate.title}, {"passed", gate.passed},
                                {"status", gate.status}, {"waiting_for_person", gate.waitingForPerson()},
                                {"missing", QJsonArray::fromStringList(missing)}};
    }
    QStringList open;
    QJsonArray notes;
    for (const auto &note : Studio::readNotes(root))
        if (!note.done) {
            open << note.id;
            notes << QJsonObject{{"id", note.id}, {"at", note.at}, {"format", note.format}, {"note", note.text}};
        }
    const QString skill = Studio::skillProblem(root);
    const QStringList assets = Studio::missingAssets(root);

    QString where;
    if (gates.isEmpty())
        where = "gates.md lists no gates";
    else if (!current)
        where = "every gate is passed: the film is approved";
    else if (current->waitingForPerson())
        where = QString("at gate %1, %2, waiting for the person's approval").arg(current->number).arg(current->title);
    else
        where = QString("at gate %1, %2").arg(current->number).arg(current->title);

    if (command.parser.isSet("json")) {
        print({{"studio", root}, {"gate", current ? QJsonValue(current->number) : QJsonValue()},
               {"summary", where}, {"gates", gateList}, {"open_notes", notes},
               {"missing_assets", QJsonArray::fromStringList(assets)},
               {"skill", skill.isEmpty() ? QJsonValue() : QJsonValue(skill)},
               {"ok", problems.isEmpty()}, {"problems", QJsonArray::fromStringList(problems)}});
        return problems.isEmpty() ? 0 : 1;
    }
    print(stdout, root + ": " + where + ".");
    for (const auto &value : gateList) {
        const QJsonObject gate = value.toObject();
        print(stdout, QString("  [%1] %2 %3%4").arg(gate["passed"].toBool() ? "x" : " ")
                          .arg(gate["number"].toInt()).arg(gate["title"].toString(),
                               gate["status"].toString().isEmpty() ? QString() : ": " + gate["status"].toString()));
        for (const auto &item : gate["missing"].toArray())
            print(stdout, "        missing: " + item.toString());
    }
    print(stdout, open.isEmpty() ? QString("No open notes.")
                                 : QString("Open notes: %1 (in notes.md).").arg(open.join(", ")));
    if (!assets.isEmpty())
        print(stdout, "Assets marked missing in assets/assets.md: " + assets.join("; ") + ".");
    if (!skill.isEmpty())
        print(stdout, skill);
    for (const auto &problem : problems)
        print(stderr, "error: " + problem);
    return problems.isEmpty() ? 0 : 1;
}

int review(const QStringList &arguments) {
    Command command("review", "Write the next review, reviews/NNN.md: contact sheets with strips of the fastest "
                              "changes, everything check measures, scores, and the three largest defects with "
                              "cropped evidence, leaving the judgments to complete.");
    command.format("all of the scene's formats");
    command.seed();
    command.json();
    command.parser.process(arguments);
    QStringList formats;
    if (!command.formats(true, &formats))
        return 1;
    QJsonObject result;
    QString error;
    if (!Review::write(command.scene(), formats, &result, &error))
        return fail(error);
    if (command.parser.isSet("json")) {
        print(result);
        return 0;
    }
    print(stdout, "Wrote " + result["review"].toString());
    print(stdout, result["verdict"].toString());
    for (const auto &defect : result["defects"].toArray())
        print(stdout, QString("  %1 at %2: %3").arg(defect.toObject()["format"].toString(),
                                                     seconds(defect.toObject()["at"].toDouble()),
                                                     defect.toObject()["message"].toString()));
    return 0;
}

int reference(const QStringList &arguments) {
    Command command("reference", "Measure a reference film or images: the palette, the time between scene changes, "
                                 "and how much moves. With --write, put the numbers in the style guide's REFERENCE "
                                 "ANALYSIS, for the agent to complete.", false);
    command.parser.addPositionalArgument("inputs", "A video, or one or more images", "<video or images...>");
    command.parser.addOption({"write", "The style guide to write the measured lines into", "style-guide.md"});
    command.json();
    command.parser.process(arguments);
    const QStringList inputs = command.parser.positionalArguments().mid(1);
    Reference::Analysis analysis;
    QString error;
    if (!Reference::analyze(inputs, &analysis, &error))
        return fail(error);
    if (command.parser.isSet("write") && !Reference::writeInto(command.parser.value("write"), analysis, &error))
        return fail(error);
    if (command.parser.isSet("json")) {
        QJsonArray palette, cuts;
        for (const auto &swatch : analysis.palette)
            palette << QJsonObject{{"color", swatch.color.name()}, {"share", std::round(swatch.share * 1000) / 1000}};
        for (double cut : analysis.cuts)
            cuts << cut;
        print({{"palette", palette}, {"cuts", cuts}, {"shot_length", analysis.shotLength},
               {"energy", analysis.energy}, {"duration", analysis.duration}, {"frames", analysis.frames}});
        return 0;
    }
    for (const auto &line : Reference::guideLines(analysis))
        print(stdout, line);
    if (command.parser.isSet("write"))
        print(stdout, "Wrote these into " + command.parser.value("write") +
                          ". Complete the rest of REFERENCE ANALYSIS, and list what not to copy.");
    return 0;
}

int deliver(const QStringList &arguments) {
    Command command("deliver", "Hand the film over: render every format, then write the videos, posters, contact "
                               "sheets, a storyboard PDF, the source with its assets, the asset list, and a note on what "
                               "still needs a person's review, into deliverables/.");
    command.parser.addOption({{"o", "output"}, "Folder for the deliverables (default: deliverables/ in the film's folder)",
                              "folder"});
    command.format("all of the scene's formats");
    command.seed();
    command.json();
    command.parser.process(arguments);
    QStringList formats;
    if (!command.formats(true, &formats))
        return 1;
    const bool json = command.parser.isSet("json");
    QJsonObject result;
    QString error;
    if (!Deliver::write(command.scene(), formats, command.parser.value("output"), &result, &error,
                        [&](const QString &what) {
                            if (!json)
                                print(stdout, what + "...");
                        }))
        return fail(error);
    if (json) {
        print(result);
        return 0;
    }
    print(stdout, "Delivered " + result["folder"].toString() + ": " +
                      QString("%1 videos, posters, sheets, storyboard.pdf, source/, and REVIEW.md")
                          .arg(result["videos"].toArray().size()));
    if (result["open"].toInt() > 0)
        print(stdout, QString("REVIEW.md lists %1 things still open.").arg(result["open"].toInt()));
    return 0;
}

int capture(const QStringList &arguments) {
    Command command("capture", "Capture a web page as a PNG with Chromium, for a film's assets: in a film's folder it "
                               "goes in assets/screens/ and assets.md lists it with its address, for the person to "
                               "approve. Pages behind a login need the person's own screenshot.", false);
    command.parser.addPositionalArgument("url", "The page's address", "<url>");
    command.parser.addOption({{"o", "output"}, "PNG file (default: assets/screens/<host>.png in a film's folder)", "path"});
    command.parser.addOption({"size", "Window size, such as 1440x900 (default) or 390x844 for a phone", "WxH"});
    command.json();
    command.parser.process(arguments);
    const QString url = command.argument(1);
    if (url.isEmpty())
        return fail("Name the page, such as https://example.com.");
    QSize size(1440, 900);
    if (command.parser.isSet("size")) {
        const QStringList parts = command.parser.value("size").split('x');
        size = QSize(parts.value(0).toInt(), parts.value(1).toInt());
        if (size.width() < 200 || size.height() < 200 || size.width() > 7680 || size.height() > 7680)
            return fail("Give the size as WxH, such as 1440x900.");
    }
    const QString root = Studio::rootOf(QDir::currentPath());
    QString output = command.parser.value("output");
    if (output.isEmpty()) {
        QString host = QUrl(url).host().remove(QRegularExpression("^www\\.")).replace('.', '-');
        if (host.isEmpty())
            host = QFileInfo(QUrl(url).path()).completeBaseName();
        const QString folder = root.isEmpty() ? QDir::currentPath() : QDir(root).filePath("assets/screens");
        output = QDir(folder).filePath((host.isEmpty() ? "page" : host) + ".png");
        for (int n = 2; QFileInfo::exists(output); ++n)
            output = QDir(folder).filePath(QString("%1-%2.png").arg(host.isEmpty() ? "page" : host).arg(n));
    }
    // Listed only when it lands among the studio's assets.
    const bool listed = !root.isEmpty() &&
                        QFileInfo(output).absoluteFilePath().startsWith(QDir(root).filePath("assets") + "/");
    QString error;
    if (!Studio::capture(url, output, size, listed ? root : QString(), &error))
        return fail(error);
    if (command.parser.isSet("json"))
        print({{"image", QFileInfo(output).absoluteFilePath()}, {"listed", listed}});
    else
        print(stdout, "Captured " + output + (listed ? ", listed in assets/assets.md for the person to approve" : ""));
    return 0;
}

int beats(const QStringList &arguments) {
    Command command("beats", "Measure a track's tempo and beats, and write them beside it as <track>.beats.json, "
                             "where check finds them to hold the film's cuts to the music.", false);
    command.parser.addPositionalArgument("track", "The audio file", "<track>");
    command.json();
    command.parser.process(arguments);
    const QString track = command.argument(1);
    if (track.isEmpty() || !QFileInfo(track).isFile())
        return fail("Name the audio file, such as assets/track.wav.");
    QString error;
    const int rate = 22050;
    const QList<float> samples = Sound::decode(track, rate, &error);
    if (!error.isEmpty())
        return fail(error);
    const Sound::Grid grid = Sound::beats(samples, rate);
    if (grid.beats.isEmpty())
        return fail(track + " has no steady beat omotion can find; cut to the picture instead.");
    QJsonArray times;
    for (double t : grid.beats)
        times << std::round(t * 1000) / 1000;
    const QJsonObject result{{"track", QFileInfo(track).absoluteFilePath()},
                             {"tempo", std::round(grid.tempo * 10) / 10},
                             {"duration", samples.size() / double(rate)},
                             {"beats", times}};
    const QString path = QFileInfo(track).absoluteFilePath() + ".beats.json";
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(result).toJson()) < 0 || !file.commit())
        return fail(path + ": " + file.errorString());
    if (command.parser.isSet("json")) {
        print(result);
        return 0;
    }
    QStringList first;
    for (int i = 0; i < qMin<int>(8, grid.beats.size()); ++i)
        first << QString::number(grid.beats[i], 'f', 2);
    print(stdout, QString("%1 BPM, %2 beats: %3%4\nWrote %5")
                      .arg(grid.tempo, 0, 'f', 1)
                      .arg(grid.beats.size())
                      .arg(first.join(", "), grid.beats.size() > 8 ? ", ..." : "")
                      .arg(path));
    return 0;
}

// The skill lives in each film's folder, so it always matches the omotion that
// wrote it and travels with the film; nothing is installed in the home folder.
int skill(const QStringList &arguments) {
    Command command("skill", "Print the agent skill, or write it into a film's folder with: skill install.", false);
    command.parser.addPositionalArgument("install", "Write .claude/skills/omotion/SKILL.md (and AGENTS.md if missing) "
                                                    "into the film's folder", "[install]");
    command.parser.addPositionalArgument("folder", "The film's folder (default: the one you're in)", "[folder]");
    command.parser.process(arguments);
    if (command.argument(1).isEmpty()) {
        fputs(Studio::stampedSkill().constData(), stdout);
        return 0;
    }
    if (command.argument(1) != "install")
        return fail("Use omotion skill to print the skill, or omotion skill install in a film's folder.");
    const QString from = command.argument(2).isEmpty() ? QDir::currentPath() : command.argument(2);
    const QString root = Studio::rootOf(from);
    if (root.isEmpty())
        return fail(from + " isn't inside a film made by omotion. Start one with: omotion new <folder>");
    QString error;
    QStringList written;
    if (!Studio::installSkill(root, &error, &written))
        return fail(error);
    for (const auto &file : written)
        print(stdout, "Wrote " + QDir(root).filePath(file));
    return 0;
}

int help(const QStringList &arguments) {
    const QString topic = arguments.value(2);
    if (topic == "format") {
        QFile guide(":/format.md");
        if (!guide.open(QIODevice::ReadOnly))
            return 1;
        fputs(guide.readAll().constData(), stdout);
        return 0;
    }
    if (isCliCommand(topic) && topic != "help")
        return runCli({arguments[0], topic, "--help"});
    print(stdout, "Usage: omotion <command> [options]\n\n" + cliSummary());
    return 0;
}
} // namespace

QString cliSummary() {
    return "Window:\n"
           "  open [scene]              Preview a scene, on the last one by default\n\n"
           "Commands that need no display (omotion help <command> for options):\n"
           "  new <folder>              Start a film: brief, shot list, gates, scene, agent skill\n"
           "  status [folder]           Which gate the film is at, and what each gate lacks\n"
           "  check <scene>             Report every problem, in every format\n"
           "  frame <scene> --at <s>    Render one frame to PNG\n"
           "  sheet <scene>             Render a contact sheet of the beats to PNG\n"
           "  review <scene>            Write the next review: sheets, measures, scores, defects\n"
           "  render <scene>            Encode MP4s, one per format\n"
           "  deliver <scene>           Every deliverable: videos, posters, sheets, storyboard, source\n"
           "  beats <track>             Measure a track's tempo and beats for cutting to it\n"
           "  reference <video|images>  Measure a reference's palette, pacing, and motion\n"
           "  capture <url>             Capture a page into the film's assets\n"
           "  help format               How to write a scene\n"
           "  skill [install]           Print the agent skill, or write it into this film's folder";
}

bool isCliCommand(const QString &word) {
    return QStringList{"new", "status", "check", "frame", "sheet", "review", "render", "beats", "reference", "capture", "deliver", "skill", "help"}.contains(word);
}

int runCli(const QStringList &arguments) {
    const QString command = arguments.value(1);
    return command == "new"      ? create(arguments)
           : command == "status" ? status(arguments)
           : command == "check"  ? check(arguments)
           : command == "frame"  ? frame(arguments)
           : command == "sheet"  ? sheet(arguments)
           : command == "review" ? review(arguments)
           : command == "render" ? render(arguments)
           : command == "beats"  ? beats(arguments)
           : command == "reference" ? reference(arguments)
           : command == "capture" ? capture(arguments)
           : command == "deliver" ? deliver(arguments)
           : command == "skill"  ? skill(arguments)
                                 : help(arguments);
}
