#include "cli.h"
#include "scene.h"
#include "studio.h"

#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
// A path ending in a slash, or naming a folder that exists, is where the file goes.
bool namesDirectory(const QString &path) { return path.endsWith('/') || QFileInfo(path).isDir(); }

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
    void format(const QString &fallback) {
        parser.addOption({"format", "16:9, 9:16, 1:1, or all (default: " + fallback + ")", "format"});
    }
    QString argument(int index) const { return parser.positionalArguments().value(index); }
    QString scene() const { return argument(1); }

    // The formats asked for, checked against the scene's own list when "all".
    bool formats(bool allByDefault, QStringList *formats) const {
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
    command.format("all of the scene's formats");
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
                beats << QJsonObject{{"at", beat.at}, {"name", beat.name}};
            info = {{"scene", scene.path()}, {"duration", scene.duration()}, {"fps", scene.fps()},
                    {"frames", scene.frameCount()}, {"beats", beats},
                    {"formats", QJsonArray::fromStringList(scene.formats())},
                    {"audio", scene.audio().isEmpty() ? QJsonValue() : QJsonValue(scene.audio())}};
        }
        // The declarations are the same in every format, so report them once.
        for (const auto &problem : scene.check())
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
    command.format("the scene's first format");
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
    if (!image.save(output, "PNG"))
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
    command.parser.addOption({{"o", "output"}, "PNG file, or a folder for one per format (default: <scene>-<format>-sheet.png beside the scene)", "path"});
    command.parser.addOption({"columns", "Tiles per row", "count"});
    command.format("the scene's first format");
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
        const QString file = QString("%1-%2-sheet.png").arg(QFileInfo(scene.path()).completeBaseName(),
                                                            scene.format().slug());
        QString output = command.parser.value("output");
        if (output.isEmpty())
            output = QFileInfo(scene.path()).absoluteDir().filePath(file);
        else if (namesDirectory(output))
            output = QDir(output).filePath(file);
        else if (formats.size() > 1)
            output = QFileInfo(output).absoluteDir().filePath(file);
        QDir().mkpath(QFileInfo(output).absolutePath());
        if (!scene.contactSheet(times, command.parser.value("columns").toInt()).save(output, "PNG"))
            return fail("Could not save " + output);
        QJsonArray tiles;
        for (double t : times)
            tiles << QJsonObject{{"at", t}, {"frame", int(std::lround(t * scene.fps()))}, {"beat", scene.beatNameAt(t)}};
        sheets << QJsonObject{{"image", QFileInfo(output).absoluteFilePath()}, {"format", name}, {"tiles", tiles}};
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
    command.parser.addOption({{"o", "output"}, "MP4 file for one format, or a directory for several "
                                               "(default: renders/<scene>-<format>.mp4 beside the scene)", "path"});
    command.parser.addOption({"progress", "Print a JSON line per frame"});
    command.format("all of the scene's formats");
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
        if (!output.isEmpty())
            target = formats.size() == 1 && !namesDirectory(output)
                         ? output : QDir(output).filePath(QFileInfo(target).fileName());
        const QString format = formats[i];
        const int count = formats.size();
        auto report = [&](int frame, int frames) {
            if (progress)
                print({{"format", format}, {"index", i}, {"count", count}, {"frame", frame}, {"frames", frames}}, true);
        };
        if (!scene.render(target, &error, report))
            return fail(error);
        outputs << QJsonObject{{"format", format}, {"video", QFileInfo(target).absoluteFilePath()},
                               {"frames", scene.frameCount()}, {"duration", scene.frameCount() / double(scene.fps())}};
        if (!command.parser.isSet("json") && !progress)
            print(stdout, "Rendered " + target);
    }
    if (command.parser.isSet("json") || progress)
        print({{"outputs", outputs}}, progress);
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
           "  check <scene>             Report every problem, in every format\n"
           "  frame <scene> --at <s>    Render one frame to PNG\n"
           "  sheet <scene>             Render a contact sheet of the beats to PNG\n"
           "  render <scene>            Encode MP4s, one per format\n"
           "  help format               How to write a scene\n"
           "  skill [install]           Print the agent skill, or write it into this film's folder";
}

bool isCliCommand(const QString &word) {
    return QStringList{"new", "check", "frame", "sheet", "render", "skill", "help"}.contains(word);
}

int runCli(const QStringList &arguments) {
    const QString command = arguments.value(1);
    return command == "new"      ? create(arguments)
           : command == "check"  ? check(arguments)
           : command == "frame"  ? frame(arguments)
           : command == "sheet"  ? sheet(arguments)
           : command == "render" ? render(arguments)
           : command == "skill"  ? skill(arguments)
                                 : help(arguments);
}
