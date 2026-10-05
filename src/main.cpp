// omotion — animates images: cut a picture into layers, keyframe them, and
// export the result. The whole animation is a scene.json anyone can edit.

#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
  omotion cut <project> <layer> <x,y> <x,y> <x,y>... [--copy] [--fill ai|smooth|none] [--name <name>] [--soft <px>] [--at <seconds>]
  omotion cut <project> <layer> --pick <x,y> [--pick <x,y>]... [--tolerance <0-160>, picked when left out] [same options]
                                         Cut the part of a layer inside a polygon (canvas
                                         pixels) out into a new layer, as the C tool does;
                                         the hole it leaves is filled, by the AI where it's
                                         set up, unless --fill smooth or --fill none.
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
  omotion pivot <project> <layer> <x,y> [--at <seconds>]
                                         Move where a layer turns, without moving it, as P does
  omotion link <project> <layer|camera> <property> <link | --none>
                                         Drive a property from another, on top of its own
                                         value: "car.x * 0.8", "speed of car.x * 0.05",
                                         "acceleration of car.x * -0.002, smooth 0.2",
                                         "time * 90"; extras: lag, smooth, spring, damping,
                                         min, max, abs
  omotion info <project> [--at <seconds>]
                                         The scene as JSON, with each layer's place on the canvas
  omotion particles <project> <snow|rain|dust|fireflies|leaves|embers> [--<setting> <value>]...
                                         Add a particle layer; settings: count, size, speed,
                                         direction, spread, sway, twinkle, color, seed
  omotion audio <project> <file|--none> [--offset <seconds>] [--volume <0-4>]
                                         The scene's sound, for playing along and video exports
  omotion motion list | save <project> <layer|camera> <name> | apply <project> <layer|camera> <name> [--at <seconds>]
                                         Saved motions: a layer's keyframes, kept to reuse
  omotion clean <project> [--dry-run]    Move images scene.json doesn't use to the trash
  omotion models [--download]            Whether the AI is ready; --download fetches its
                                         models (about 350 MB, checked against their hashes)

A project is a folder holding scene.json. Opening an image makes
"<image>.omotion" beside it; opening a folder of images makes a scene.json
with one layer per image.
)";

int fail(const QString &message) {
    QTextStream(stderr) << "omotion: " << message << "\n";
    return 1;
}

// The number after a flag, or false (with a message) when it's missing or isn't one.
bool numberAfter(const QStringList &args, int &i, double *value) {
    bool ok = false;
    if (i + 1 < args.size())
        *value = args.at(i + 1).toDouble(&ok);
    if (!ok) {
        fail(QString("%1 needs a number after it.").arg(args.at(i)));
        return false;
    }
    ++i;
    return true;
}

// The command-line renderer, for scripts and agents: no window, same output.
int command(const QStringList &args) {
    QString error;
    // "motion save|apply <project> ...": the project comes a word later.
    const QString directory = Project::open(args.at(args.at(1) == "motion" ? 3 : 2), &error);
    if (directory.isEmpty())
        return fail(error);
    const Scene scene = Project::load(directory, &error);
    if (!error.isEmpty())
        return fail(error);

    AssetCache assets(directory);
    for (const Layer &layer : scene.layers)
        if (layer.particles.isEmpty() && assets.image(layer.source).isNull())
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
        bool aiFill = Ai::instance().canInpaint();
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
            } else if (arg == "--fill" && i + 1 < args.size()) {
                const QString how = args.at(++i);
                if (how != "ai" && how != "smooth" && how != "none")
                    return fail("--fill is ai, smooth or none.");
                fill = how != "none";
                aiFill = how == "ai";
                if (aiFill && !Ai::instance().canInpaint())
                    return fail("The AI's fill isn't set up here; omotion models --download fetches it.");
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
        const int piece = Project::cutOut(edited, assets, layer, polygon, t, punch, &error, name, feather, fill, aiFill);
        if (piece < 0)
            return fail(error);
        if (!Project::save(directory, edited, &error))
            return fail(error);
        QTextStream(stdout) << edited.layers.at(piece).name << "\n";
        return 0;
    }

    if (args.at(1) == "info") {
        double t = 0;
        for (int i = 3; i < args.size(); ++i) {
            if (args.at(i) != "--at")
                return fail(QString("info doesn't know \"%1\".").arg(args.at(i)));
            if (!numberAfter(args, i, &t))
                return 1;
        }
        // What an agent needs to work on the picture: where each layer is.
        QJsonArray layers;
        for (int i = 0; i < scene.layers.size(); ++i) {
            const Layer &layer = scene.layers.at(i);
            const QSizeF size = assets.size(layer.source);
            const QTransform toCanvas = layerTransform(scene, assets, i, t);
            const QRectF bounds = toCanvas.mapRect(QRectF(QPointF(0, 0), size));
            const QPointF pivot = toCanvas.map(QPointF(layer.anchor.x() * size.width(), layer.anchor.y() * size.height()));
            auto round = [](double v) { return std::round(v * 10) / 10; };
            QJsonObject links;
            for (auto it = layer.links.begin(); it != layer.links.end(); ++it)
                links.insert(it.key(), Scene::linkText(it.value()));
            if (!layer.particles.isEmpty()) {
                QJsonObject entry{{"name", layer.name}, {"visible", layer.visible}, {"particles", layer.particles},
                                  {"blend", layer.blend}, {"follow", followOf(scene, i)}};
                if (!links.isEmpty())
                    entry["links"] = links;
                layers.append(entry);
                continue;
            }
            QJsonObject entry{{"name", layer.name},
                              {"source", layer.source},
                              {"visible", layer.visible},
                              {"bounds", QJsonArray{round(bounds.x()), round(bounds.y()), round(bounds.width()), round(bounds.height())}},
                              {"pivot", QJsonArray{round(pivot.x()), round(pivot.y())}},
                              {"blend", layer.blend}};
            if (!layer.parent.isEmpty())
                entry["parent"] = layer.parent;
            QJsonArray animated;
            for (auto it = layer.keys.begin(); it != layer.keys.end(); ++it)
                animated.append(it.key());
            entry["animated"] = animated;
            if (!links.isEmpty())
                entry["links"] = links;
            entry["follow"] = followOf(scene, i);
            const QJsonObject cut = layer.raw.value("cut").toObject();
            if (!cut.isEmpty())
                entry["cutFrom"] = cut.value("from");
            layers.append(entry);
        }
        QJsonArray cameraAnimated;
        for (auto it = scene.camera.keys.begin(); it != scene.camera.keys.end(); ++it)
            cameraAnimated.append(it.key());
        QJsonObject camera{{"x", scene.value(scene.camera, "x", t)},
                           {"y", scene.value(scene.camera, "y", t)},
                           {"zoom", scene.value(scene.camera, "zoom", t)},
                           {"rotation", scene.value(scene.camera, "rotation", t)},
                           {"animated", cameraAnimated}};
        QJsonObject cameraLinks;
        for (auto it = scene.camera.links.begin(); it != scene.camera.links.end(); ++it)
            cameraLinks.insert(it.key(), Scene::linkText(it.value()));
        if (!cameraLinks.isEmpty())
            camera["links"] = cameraLinks;
        const QJsonObject info{{"directory", directory},
                               {"camera", camera},
                               {"width", scene.width},
                               {"height", scene.height},
                               {"fps", scene.fps},
                               {"duration", scene.duration},
                               {"at", t},
                               {"layers", layers}};
        QTextStream(stdout) << QJsonDocument(info).toJson(QJsonDocument::Indented);
        return 0;
    }

    if (args.at(1) == "link") {
        if (args.size() != 6)
            return fail("link takes a project, a layer (or camera), a property, and a link or --none.");
        Scene edited = scene;
        const int layer = args.at(3) == "camera" ? Scene::CameraIndex : edited.indexOf(args.at(3));
        if (!edited.at(layer))
            return fail(QString("There's no layer named \"%1\".").arg(args.at(3)));
        Link link;
        if (args.at(5) != "--none" && !Scene::parseLink(args.at(5), &link, &error))
            return fail(error);
        if (!edited.setLink(layer, args.at(4), link, &error) || !Project::save(directory, edited, &error))
            return fail(error);
        return 0;
    }

    if (args.at(1) == "pivot") {
        if (args.size() < 5)
            return fail("pivot takes a project, a layer, and an x,y point on the canvas.");
        Scene edited = scene;
        const int layer = edited.indexOf(args.at(3));
        if (layer < 0)
            return fail(QString("There's no layer named \"%1\".").arg(args.at(3)));
        const QStringList xy = args.at(4).split(',');
        bool okX = false, okY = false;
        const QPointF point = xy.size() == 2 ? QPointF(xy.at(0).toDouble(&okX), xy.at(1).toDouble(&okY)) : QPointF();
        if (!okX || !okY)
            return fail(QString("\"%1\" isn't a point like 120,340.").arg(args.at(4)));
        double t = 0;
        for (int i = 5; i < args.size(); ++i) {
            if (args.at(i) != "--at")
                return fail(QString("pivot doesn't know \"%1\".").arg(args.at(i)));
            if (!numberAfter(args, i, &t))
                return 1;
        }
        bool invertible = false;
        layerTransform(edited, assets, layer, t).inverted(&invertible);
        if (!invertible)
            return fail(QString("\"%1\" is scaled to nothing at %2 s, so it has no pivot to move.").arg(args.at(3)).arg(t));
        Project::setPivot(edited, assets, layer, point, t);
        if (!Project::save(directory, edited, &error))
            return fail(error);
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
        for (int i = 5; i < args.size(); ++i) {
            if (args.at(i) == "--amount") {
                if (!numberAfter(args, i, &amount))
                    return 1;
            } else if (args.at(i) == "--at") {
                if (!numberAfter(args, i, &t))
                    return 1;
            } else {
                return fail(QString("preset doesn't know \"%1\".").arg(args.at(i)));
            }
        }
        if (!Project::applyPreset(edited, assets, layer, args.at(4), t, amount, &error) ||
            !Project::save(directory, edited, &error))
            return fail(error);
        return 0;
    }

    if (args.at(1) == "particles") {
        if (args.size() < 4)
            return fail("particles takes a project and a kind.");
        QJsonObject settings;
        for (int i = 4; i < args.size(); ++i) {
            const QString setting = args.at(i).mid(2);
            if (!args.at(i).startsWith("--") || i + 1 >= args.size())
                return fail(QString("particles settings go --name value; not \"%1\".").arg(args.at(i)));
            const QString value = args.at(++i);
            bool number = false;
            const double v = value.toDouble(&number);
            settings.insert(setting, number ? QJsonValue(v) : QJsonValue(value));
        }
        Scene edited = scene;
        const int index = Project::addParticles(edited, args.at(3), settings, &error);
        if (index < 0 || !Project::save(directory, edited, &error))
            return fail(error);
        QTextStream(stdout) << edited.layers.at(index).name << "\n";
        return 0;
    }

    if (args.at(1) == "audio") {
        Scene edited = scene;
        if (args.at(3) == "--none" && args.size() > 4)
            return fail("--none takes the sound away; there's nothing left to set.");
        if (args.at(3) == "--none") {
            edited.raw.remove("audio");
        } else if (!Project::setAudio(edited, directory, QFileInfo(args.at(3)).absoluteFilePath(), &error)) {
            return fail(error);
        }
        QJsonObject audio = edited.raw.value("audio").toObject();
        for (int i = 4; i < args.size(); ++i) {
            if (args.at(i) != "--offset" && args.at(i) != "--volume")
                return fail(QString("audio doesn't know \"%1\".").arg(args.at(i)));
            const QString name = args.at(i).mid(2);
            double value = 0;
            if (!numberAfter(args, i, &value))
                return 1;
            if (name == "offset" && value < 0)
                return fail("--offset is seconds into the sound, from 0.");
            if (name == "volume" && (value < 0 || value > 4))
                return fail("--volume goes from 0 (silent) to 4.");
            audio[name] = value;
        }
        if (!audio.isEmpty())
            edited.raw["audio"] = audio;
        if (!Project::save(directory, edited, &error))
            return fail(error);
        return 0;
    }

    if (args.at(1) == "motion") {
        if (args.size() < 6 || (args.at(2) != "save" && args.at(2) != "apply"))
            return fail("motion is: list, save <project> <layer> <name>, or apply <project> <layer> <name>.");
        Scene edited = scene;
        const int layer = args.at(4) == "camera" ? Scene::CameraIndex : edited.indexOf(args.at(4));
        if (!edited.at(layer))
            return fail(QString("There's no layer named \"%1\".").arg(args.at(4)));
        if (args.at(2) == "save") {
            QList<KeyRef> refs;
            for (double t : edited.keyTimes(layer))
                refs.append(KeyRef{layer, t});
            return Project::saveMotion(args.at(5), edited.copyKeys(refs), &error) ? 0 : fail(error);
        }
        double t = 0;
        for (int i = 6; i < args.size(); ++i) {
            if (args.at(i) != "--at")
                return fail(QString("motion apply doesn't know \"%1\".").arg(args.at(i)));
            if (!numberAfter(args, i, &t))
                return 1;
        }
        const QList<KeyClip> keys = Project::loadMotion(args.at(5), &error);
        if (keys.isEmpty())
            return fail(error);
        edited.pasteKeys(layer, t, keys);
        return Project::save(directory, edited, &error) ? 0 : fail(error);
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
                                       !std::strcmp(argv[1], "preset") || !std::strcmp(argv[1], "pivot") ||
                                       !std::strcmp(argv[1], "info") || !std::strcmp(argv[1], "particles") || !std::strcmp(argv[1], "audio") ||
                                       !std::strcmp(argv[1], "motion") || !std::strcmp(argv[1], "link") ||
                                       !std::strcmp(argv[1], "models"));
    if (headless)
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QGuiApplication app(argc, argv);
    app.setApplicationName("omotion");
    app.setApplicationVersion("0.3.0");

    const QStringList args = app.arguments();
    if (args.size() > 1 && (args.at(1) == "--help" || args.at(1) == "-h")) {
        QTextStream(stdout) << Usage;
        return 0;
    }
    if (args.size() > 1 && args.at(1) == "--version") {
        QTextStream(stdout) << "omotion " << app.applicationVersion() << "\n";
        return 0;
    }
    if (headless && args.at(1) == "motion" && args.size() > 2 && args.at(2) == "list") {
        for (const QString &name : Project::savedMotions())
            QTextStream(stdout) << name << "\n";
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
        const bool short_ = args.at(1) == "split" || args.at(1) == "clean" || args.at(1) == "info";
        if (args.size() < (short_ ? 3 : 4))
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
