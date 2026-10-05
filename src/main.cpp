// omotion — animates images: cut a picture into layers, keyframe them, and
// export the result. The whole animation is a scene.json anyone can edit.

#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QTextStream>
#include <QUrl>
#include <cstring>

#include "ai.h"
#include "backend.h"
#include "exporter.h"
#include "portalfilepicker.h"
#include "project.h"
#include "sceneview.h"
#include "theme.h"

namespace {

const char *Usage = R"(Usage:
  omotion [image | folder]               Open the editor
  omotion render <project> <output> [--scale <factor>]
                                         Render to .mp4, .webm, .gif, or a folder of PNG frames,
                                         at the canvas size or scaled (0.5 for half)
  omotion frame <project> <seconds> <output.png>
                                         Render one frame as a PNG
  omotion cut <project> <layer> <x,y> <x,y> <x,y>... [--copy] [--no-fill] [--name <name>] [--soft <px>] [--at <seconds>]
  omotion cut <project> <layer> --pick <x,y> [--pick <x,y>]... [--tolerance <0-160>, picked when left out] [same options]
                                         Cut the part of a layer inside a polygon (canvas
                                         pixels) out into a new layer, as the C tool does;
                                         the hole it leaves is filled unless --no-fill.
                                         --pick cuts the area of similar colour around a
                                         point instead, as the W tool does
  omotion cut <project> <layer> --ai-pick <x,y>... [--ai-exclude <x,y>]... [--choice <n>] [same options]
                                         Cut what the AI sees under the points, less what's
                                         under the excluded ones; --choice 1, 2... takes its
                                         other guesses (part, whole)
  omotion split <project> [--layer <name>] [--layers <2-8>] [--soft <px>] [--animate [<px>]]
                                         Split a layer (the bottom one unless named) into
                                         layers by depth, with the AI; --animate adds a
                                         looping parallax camera move
  omotion preset <project> <layer> <sway|float|pulse|bounce|spin|fade> [--amount <n>] [--at <seconds>]
                                         Give a layer a ready-made motion, looping with the scene
  omotion clean <project> [--dry-run]    Move images scene.json doesn't use to the trash
  omotion models [--download]            Whether the AI is ready; --download fetches its
                                         models (about 145 MB, checked against their hashes)

A project is a folder holding scene.json. Opening an image makes
"<image>.omotion" beside it; opening a folder of images makes a scene.json
with one layer per image.
)";

int fail(const QString &message) {
    QTextStream(stderr) << "omotion: " << message << "\n";
    return 1;
}

// The command-line renderer, for scripts and agents: no window, same output.
int command(const QStringList &args) {
    QString error;
    const QString directory = Project::open(args.at(2), &error);
    if (directory.isEmpty())
        return fail(error);
    const Scene scene = Project::load(directory, &error);
    if (!error.isEmpty())
        return fail(error);

    AssetCache assets(directory);
    for (const Layer &layer : scene.layers)
        if (assets.image(layer.source).isNull())
            return fail(QString("Can't read %1 (layer \"%2\").").arg(layer.source, layer.name));

    if (args.at(1) == "cut") {
        if (args.size() < 4)
            return fail("cut takes a project, a layer name, and at least three x,y points.");
        Scene edited = scene;
        const int layer = edited.indexOf(args.at(3));
        if (layer < 0)
            return fail(QString("There's no layer named \"%1\".").arg(args.at(3)));
        QPolygonF polygon;
        bool punch = true;
        QString name;
        double t = 0;
        double feather = 0;
        bool fill = true;
        int tolerance = -1;
        QList<QPointF> picks, aiTake, aiLeave;
        int choice = 0;
        auto point = [&](int i, QList<QPointF> &into) {
            const QStringList xy = args.at(i).split(',');
            bool okX = false, okY = false;
            if (xy.size() == 2)
                into.append(QPointF(xy.at(0).toDouble(&okX), xy.at(1).toDouble(&okY)));
            return okX && okY;
        };
        for (int i = 4; i < args.size(); ++i) {
            const QString arg = args.at(i);
            if ((arg == "--ai-pick" || arg == "--ai-exclude") && i + 1 < args.size()) {
                if (!point(++i, arg == "--ai-pick" ? aiTake : aiLeave))
                    return fail(QString("\"%1\" isn't a point like 120,340.").arg(args.at(i)));
            } else if (arg == "--choice" && i + 1 < args.size()) {
                choice = args.at(++i).toInt();
            } else if (arg == "--copy") {
                punch = false;
            } else if (arg == "--no-fill") {
                fill = false;
            } else if (arg == "--tolerance" && i + 1 < args.size()) {
                tolerance = args.at(++i).toInt();
            } else if (arg == "--pick" && i + 1 < args.size()) {
                const QStringList xy = args.at(++i).split(',');
                if (xy.size() != 2)
                    return fail(QString("\"%1\" isn't a point like 120,340.").arg(args.at(i)));
                picks.append(QPointF(xy.at(0).toDouble(), xy.at(1).toDouble()));
            } else if (arg == "--name" && i + 1 < args.size()) {
                name = args.at(++i);
            } else if (arg == "--soft" && i + 1 < args.size()) {
                feather = args.at(++i).toDouble();
            } else if (arg == "--at" && i + 1 < args.size()) {
                t = args.at(++i).toDouble();
            } else {
                const QStringList xy = arg.split(',');
                bool okX = false, okY = false;
                if (xy.size() == 2)
                    polygon.append(QPointF(xy.at(0).toDouble(&okX), xy.at(1).toDouble(&okY)));
                if (!okX || !okY)
                    return fail(QString("\"%1\" isn't a point like 120,340.").arg(arg));
            }
        }
        if (!aiTake.isEmpty()) {
            if (!polygon.isEmpty() || !picks.isEmpty())
                return fail("Give one of points to cut around, --pick, or --ai-pick.");
            int choices = 0;
            polygon = Project::aiOutline(edited, assets, layer, aiTake, aiLeave, t, choice, &choices, &error);
            if (polygon.isEmpty())
                return fail(error.isEmpty() ? QString("The AI found nothing of \"%1\" there.").arg(args.at(3)) : error);
        }
        if (!picks.isEmpty()) {
            if (!polygon.isEmpty())
                return fail("Give either points to cut around or --pick, not both.");
            int regions = 0;
            polygon = Project::smartOutline(edited, assets, layer, picks, t, tolerance, &regions);
            if (polygon.isEmpty())
                return fail(QString("Nothing of \"%1\" is under those points.").arg(args.at(3)));
            if (regions > 1)
                QTextStream(stderr) << "omotion: only areas that touch make one shape; cutting the biggest.\n";
        }
        const int piece = Project::cutOut(edited, assets, layer, polygon, t, punch, &error, name, feather, fill);
        if (piece < 0)
            return fail(error);
        if (!Project::save(directory, edited, &error))
            return fail(error);
        QTextStream(stdout) << edited.layers.at(piece).name << "\n";
        return 0;
    }

    if (args.at(1) == "preset") {
        if (args.size() < 5)
            return fail("preset takes a project, a layer, and a preset.");
        Scene edited = scene;
        const int layer = edited.indexOf(args.at(3));
        if (layer < 0)
            return fail(QString("There's no layer named \"%1\".").arg(args.at(3)));
        double amount = 1, t = 0;
        for (int i = 5; i + 1 < args.size(); i += 2) {
            if (args.at(i) == "--amount")
                amount = args.at(i + 1).toDouble();
            else if (args.at(i) == "--at")
                t = args.at(i + 1).toDouble();
            else
                return fail(QString("preset doesn't know \"%1\".").arg(args.at(i)));
        }
        if (!Project::applyPreset(edited, assets, layer, args.at(4), t, amount, &error) ||
            !Project::save(directory, edited, &error))
            return fail(error);
        return 0;
    }

    if (args.at(1) == "clean") {
        const QList<QJsonObject> scenes{scene.toJson()};
        if (args.contains("--dry-run")) {
            for (const QString &file : Project::unusedAssets(directory, scenes))
                QTextStream(stdout) << file << "\n";
            return 0;
        }
        int count = 0;
        const qint64 freed = Project::cleanAssets(directory, scenes, &count, &error);
        if (freed < 0)
            return fail(error);
        QTextStream(stdout) << QString("Moved %1 unused image%2 (%3 MB) to the trash.\n")
                                   .arg(count).arg(count == 1 ? "" : "s").arg(freed / 1e6, 0, 'f', 1);
        return error.isEmpty() ? 0 : 1;
    }

    if (args.at(1) == "split") {
        Scene edited = scene;
        int layer = edited.layers.isEmpty() ? -1 : 0;
        int count = 4;
        double feather = 2;
        double strength = -1;
        for (int i = 3; i < args.size(); ++i) {
            const QString arg = args.at(i);
            if (arg == "--layer" && i + 1 < args.size()) {
                layer = edited.indexOf(args.at(++i));
                if (layer < 0)
                    return fail(QString("There's no layer named \"%1\".").arg(args.at(i)));
            } else if (arg == "--layers" && i + 1 < args.size()) {
                count = args.at(++i).toInt();
            } else if (arg == "--soft" && i + 1 < args.size()) {
                feather = args.at(++i).toDouble();
            } else if (arg == "--animate") {
                strength = 36;
                bool ok = false;
                if (i + 1 < args.size()) {
                    const double given = args.at(i + 1).toDouble(&ok);
                    if (ok) {
                        strength = given;
                        ++i;
                    }
                }
            } else {
                return fail(QString("split doesn't know \"%1\".").arg(arg));
            }
        }
        if (layer < 0)
            return fail("There's no layer to split.");
        const QImage depth = Project::depthOf(edited, assets, layer, &error);
        if (depth.isNull())
            return fail(error);
        const QList<int> created = Project::splitByDepth(edited, assets, layer, depth, count, feather, &error);
        if (created.isEmpty())
            return fail(error);
        if (strength >= 0 && !Project::animateParallax(edited, assets, layer, strength, &error))
            return fail(error);
        if (!Project::save(directory, edited, &error))
            return fail(error);
        for (const int i : created)
            QTextStream(stdout) << edited.layers.at(i).name << "\n";
        return 0;
    }

    if (args.at(1) == "frame") {
        if (args.size() != 5)
            return fail("frame takes a project, a time in seconds, and an output .png.");
        bool ok = false;
        const double t = args.at(3).toDouble(&ok);
        if (!ok)
            return fail(QString("\"%1\" isn't a time in seconds.").arg(args.at(3)));
        if (!renderFrame(scene, assets, t).save(args.at(4)))
            return fail(QString("Couldn't write %1.").arg(args.at(4)));
        QTextStream(stdout) << args.at(4) << "\n";
        return 0;
    }

    double scale = 1;
    if (args.size() == 6 && args.at(4) == "--scale")
        scale = args.at(5).toDouble();
    else if (args.size() != 4)
        return fail("render takes a project, an output, and maybe --scale <factor>.");
    if (!Exporter::run(scene, directory, args.at(3), &error, scale))
        return fail(error);
    QTextStream(stdout) << args.at(3) << "\n";
    return 0;
}

}

int main(int argc, char *argv[]) {
    const bool headless = argc > 1 && (!std::strcmp(argv[1], "render") || !std::strcmp(argv[1], "frame") ||
                                       !std::strcmp(argv[1], "cut") || !std::strcmp(argv[1], "split") || !std::strcmp(argv[1], "clean") ||
                                       !std::strcmp(argv[1], "preset") ||
                                       !std::strcmp(argv[1], "models"));
    if (headless)
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QGuiApplication app(argc, argv);
    app.setApplicationName("omotion");
    app.setApplicationVersion("0.1.0");

    const QStringList args = app.arguments();
    if (args.size() > 1 && (args.at(1) == "--help" || args.at(1) == "-h")) {
        QTextStream(stdout) << Usage;
        return 0;
    }
    if (args.size() > 1 && args.at(1) == "--version") {
        QTextStream(stdout) << "omotion " << app.applicationVersion() << "\n";
        return 0;
    }
    if (headless && args.at(1) == "models") {
        QTextStream out(stdout);
        if (args.size() > 2 && args.at(2) == "--download") {
            const QList<Ai::Model> missing = Ai::missingModels();
            if (!missing.isEmpty())
                out << "Downloading the AI models into " << Ai::modelDirectory() << "\n";
            out.flush();
            QString error;
            if (!ModelDownload::run(&error, true))
                return fail(error);
        }
        const QString problem = Ai::instance().problem();
        out << (problem.isEmpty() ? QString("The AI is ready (models in %1).").arg(Ai::modelDirectory()) : problem) << "\n";
        return problem.isEmpty() ? 0 : 1;
    }
    if (headless) {
        if (args.size() < (args.at(1) == "split" || args.at(1) == "clean" ? 3 : 4))
            return fail(QString("missing arguments.\n\n") + Usage);
        return command(args);
    }

    // Matches the window to omotion.desktop, so the compositor (Wayland app_id)
    // and the launcher pick up the installed icon.
    app.setDesktopFileName("omotion");
    app.setWindowIcon(QIcon::fromTheme("omotion"));

    QQuickStyle::setStyle("Material");
    qmlRegisterType<SceneView>("Omotion", 1, 0, "SceneView");

    Theme theme;
    PortalFilePicker picker;
    Backend backend(&picker);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    if (args.size() > 1)
        backend.open(args.at(1));

    QObject::connect(&app, &QGuiApplication::aboutToQuit, &backend, &Backend::flush);
    return app.exec();
}
