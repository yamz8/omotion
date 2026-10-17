// omotion — animates images: cut a picture into layers, keyframe them, and
// export the result. The whole animation is a scene.json anyone can edit.

#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineF>
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
#include "film.h"
#include "gltf.h"
#include "physics.h"
#include "graphview.h"
#include "portalfilepicker.h"
#include "project.h"
#include "steps.h"
#include "sceneview.h"
#include "theme.h"
#include "world.h"
#include "worldview.h"

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
  omotion world <project> [--layer <name>] [--horizon <0-1>]
                                         Turn the picture into a 3D world, with the AI: its
                                         distances, lens and ground (the bottom layer's
                                         picture unless named); --horizon says where the
                                         horizon is, down the picture, for which way is level
  omotion shape <project> <layer>        Give a piece of the world a 3D shape, with the AI
                                         (about 20 s): it can turn and has a back
  omotion warp <project> <layer> [--amount <px>] [--scale <px>] [--speed <n>] [--seed <n>]
                                         Make a layer billow, as smoke, cloud or water: its picture
                                         pushed about by slow noise, looping; --amount 0 stops it
  omotion video <project> <clip> [--start <s>] [--speed <x>] [--hold]
                                         Add a video clip as a layer that plays (its frames written
                                         into the project), from start seconds, at a speed, looping
                                         unless --hold
  omotion track <project> <video-layer> <x,y> [--from <s>] [--to <s>] [--onto <layer>]
                                         Follow the point of a video under x,y (canvas pixels) frame
                                         by frame, and key a layer (or a new empty) to follow it
  omotion model <project> <file> <right,up,ahead> [--size <meters>]
                                         Import a 3D model (glTF, GLB or OBJ) into a world, standing
                                         there, that tall at its longest
  omotion light <project> <name> <right,up,ahead> [--color <c>] [--power <n>] [--reach <m>]
  omotion light <project> <name> [--color <c>] [--power <n>] [--reach <m>]
                                         Add a light to a world (meters from the picture's camera),
                                         or change one; it adds to the picture's own light
  omotion material <project> <layer> [--shine <0-1>] [--roughness <0-1>]
                                         How a piece of the world shines under the lights
  omotion mesh <project> <layer> smooth [<times>] | simplify [<cells>] | none
                                         Modify a piece's 3D shape as Blender's modifiers do, kept
                                         apart from it: smooth relaxes the AI's lumps, simplify makes
                                         it lighter (cells across it), none takes them away
  omotion size <project> <layer> <meters>  Size the world so that piece is that tall (a car 1.4)
  omotion rig <project> <layer> ik <lower> [--bend -1]
  omotion rig <project> <layer> look <target>
  omotion rig <project> <layer> wheel <body>
  omotion rig <project> <layer> path <x,y> <x,y>... [--from <s>] [--to <s>] [--ease <ease>] [--turn]
  omotion rig <project> <layer> none
                                         Rig a layer as Blender's constraints do: ik makes it an
                                         upper bone reaching its child's tip for a new empty (move
                                         the empty, the arm follows); look turns it toward a layer;
                                         wheel rolls it as the body it's on moves; path moves it
                                         along canvas points over a time; none takes the rig away
  omotion modifier <project> <layer|camera> <property> cycle|noise|none [--amount <n>] [--speed <n>]
                                         A curve modifier, as Blender's: cycle repeats the property's
                                         keys over and over; noise adds a smooth wobble of that
                                         amount, speed times a second; none takes them away
  omotion clip <project> <layer|camera> <motion> [--at <s>] [--repeat <n>] [--speed <x>] [--amount <x>]
  omotion clip <project> <layer|camera> --none
                                         Lay a saved motion on as a clip (it adds to the animation,
                                         keys untouched), or take the clips away
  omotion look <project> [--<setting> <value>]... | --none
                                         The scene's look: exposure (stops), contrast, saturation,
                                         temperature, tint, vignette, bloom, bloomThreshold, and
                                         depth of field: focus (z, or meters in a world), aperture
  omotion body <project> <layer> [--mass <kg>] [--bounce <0-1>] [--friction <0-1>] [--fixed] [--no-tumble]
  omotion body <project> <layer> --none
                                         Make a layer a rigid body (or a fixed one, that others hit),
                                         or not; bodies spin as they're knocked and roll over,
                                         unless --no-tumble keeps them upright
  omotion simulate <project> [--from <s>] [--to <s>]
                                         Let the bodies fall, land, bounce and knock into each other,
                                         from the time they're keyed moving at; baked into keyframes
  omotion empty <project> <name> <x,y> [--parent <layer>]
                                         Add an empty: a point to aim at or move by, drawn as a cross
  omotion film <film.json> <output> [--scale <factor>]
  omotion film <film.json> --frame <seconds> <output.png>
                                         Shots in a row, from a film file: each an omotion project
                                         (or a stretch of one) joined by cuts, dissolves, fades or
                                         wipes; rendered as render does (see the README's Films)
  omotion gltf <project> <output.glb>    The scene for Blender or any 3D tool: a world's meshes and
                                         pictures, its pieces, models and camera moving as they do;
                                         a flat scene as cards moving as its layers do
  omotion steps <project> [<layer>]       The AI steps behind each layer (or one): what it was cut
                                         from, how the hole was filled, the world, its shape; and
                                         which are out of date, as JSON
  omotion redo <project> [<layer> <step> [ai|smooth|none]]
                                         Redo everything out of date (the world, then the shapes in
                                         it), or one step of a layer: world, shape, or fill (with how)
  omotion drive <project> <layer> --from <x,ahead> --to <x,ahead> [--speed <km/h>] [--at <seconds>] [--face]
                                         Drive a piece of the world over its ground, in meters
                                         (x right of the camera, ahead of it): it keeps to the
                                         ground, flies off crests and lands; baked as keyframes.
                                         It keeps facing as painted unless --face
  omotion ground <project> <x,ahead>...   The ground's height there, in meters (blank where the
                                         world doesn't reach)
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
        if (layer.particles.isEmpty() && !layer.source.isEmpty() && assets.image(layer.source).isNull())
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
        // The world, for placing things in it: each layer's pivot and the box
        // it fills, in meters (x right, y up from the ground, z toward the
        // camera's side), at rest.
        QJsonObject worldInfo;
        const World::Info world = World::info(scene);
        if (world.valid) {
            auto vector = [](const QVector3D &v) {
                auto round = [](float f) { return std::round(double(f) * 100) / 100; };
                return QJsonArray{round(v.x()), round(v.y()), round(v.z())};
            };
            QJsonObject parts;
            for (const int index : World::layers(scene)) {
                const World::Mesh mesh = World::mesh(scene, assets, index);
                parts.insert(scene.layers.at(index).name,
                             QJsonObject{{"pivot", vector(mesh.pivot)},
                                         {"low", vector(mesh.minimum)},
                                         {"high", vector(mesh.maximum)},
                                         {"heading", std::round(World::heading(scene, assets, index) * 10) / 10}});
            }
            worldInfo = QJsonObject{{"picture", world.picture},
                                    {"lens", world.focal},
                                    {"cameraHeight", world.height},
                                    {"layers", parts}};
        }
        const QJsonObject info{{"directory", directory},
                               {"world", worldInfo},
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

    if (args.at(1) == "world") {
        Scene edited = scene;
        int layer = 0;
        double horizon = -1;
        for (int i = 3; i < args.size(); ++i) {
            if (args.at(i) == "--layer" && i + 1 < args.size()) {
                layer = edited.indexOf(args.at(++i));
                if (layer < 0)
                    return fail(QString("There's no layer named \"%1\".").arg(args.at(i)));
            } else if (args.at(i) == "--horizon") {
                if (!numberAfter(args, i, &horizon))
                    return 1;
                if (horizon <= 0 || horizon >= 1)
                    return fail("--horizon is how far down the picture the horizon is, between 0 and 1.");
            } else {
                return fail(QString("world doesn't know \"%1\".").arg(args.at(i)));
            }
        }
        if (!World::build(edited, assets, layer, &error, horizon) || !Project::save(directory, edited, &error))
            return fail(error);
        const World::Info world = World::info(edited);
        QTextStream(stdout) << QString("World of %1: lens %2 px, camera %3 m above the ground, %4 layers.\n")
                                   .arg(world.picture)
                                   .arg(world.focal, 0, 'f', 0)
                                   .arg(world.height, 0, 'f', 2)
                                   .arg(World::layers(edited).size());
        return 0;
    }

    if (args.at(1) == "shape") {
        if (args.size() != 4)
            return fail("shape takes a project and a layer.");
        Scene edited = scene;
        const int layer = edited.indexOf(args.at(3));
        if (layer < 0)
            return fail(QString("There's no layer named \"%1\".").arg(args.at(3)));
        if (!World::shapeObject(edited, assets, layer, &error) || !Project::save(directory, edited, &error))
            return fail(error);
        const QJsonObject shape = edited.layers.at(layer).raw.value("shape").toObject();
        QTextStream(stdout) << QString("%1 has a shape (fits its outline %2%).\n")
                                   .arg(args.at(3))
                                   .arg(int(shape.value("fit").toDouble() * 100));
        return 0;
    }

    if (args.at(1) == "rig") {
        if (args.size() < 5)
            return fail("rig takes a project, a layer, and a rig: ik, look, wheel, path or none.");
        Scene edited = scene;
        const int layer = edited.indexOf(args.at(3));
        if (layer < 0)
            return fail(QString("There's no layer named \"%1\".").arg(args.at(3)));
        const QString kind = args.at(4);
        if (kind == "none") {
            if (!Project::unrig(edited, layer))
                return fail(QString("%1 isn't rigged.").arg(args.at(3)));
        } else if (kind == "path") {
            QJsonArray points;
            QJsonObject options;
            for (int i = 5; i < args.size(); ++i) {
                const QString arg = args.at(i);
                if ((arg == "--from" || arg == "--to" || arg == "--ease") && i + 1 < args.size()) {
                    const QString value = args.at(++i);
                    if (arg == "--ease")
                        options["ease"] = value;
                    else
                        options[arg == "--from" ? "start" : "end"] = value.toDouble();
                } else if (arg == "--turn") {
                    options["turn"] = true;
                } else {
                    const QStringList xy = arg.split(',');
                    bool okX = false, okY = false;
                    if (xy.size() != 2)
                        return fail(QString("%1 isn't a point; write x,y.").arg(arg));
                    points.append(QJsonArray{xy.at(0).toDouble(&okX), xy.at(1).toDouble(&okY)});
                    if (!okX || !okY)
                        return fail(QString("%1 isn't a point; write x,y.").arg(arg));
                }
            }
            options["points"] = points;
            if (!Project::rig(edited, assets, layer, "path", {}, &error, options))
                return fail(error);
        } else {
            if (args.size() < 6)
                return fail(QString("rig %1 takes the other layer too.").arg(kind));
            QJsonObject options;
            if (args.size() >= 8 && args.at(6) == "--bend")
                options["bend"] = args.at(7).toInt() < 0 ? -1 : 1;
            const QString type = kind == "look" ? QString("lookAt") : kind;
            if (!Project::rig(edited, assets, layer, type, args.at(5), &error, options))
                return fail(error);
        }
        if (!Project::save(directory, edited, &error))
            return fail(error);
        QTextStream(stdout) << QString("%1: %2.\n").arg(args.at(3), kind == "none" ? QString("no rig") : "rigged, " + kind);
        return 0;
    }

    if (args.at(1) == "modifier" || args.at(1) == "clip") {
        if (args.size() < 5)
            return fail(args.at(1) == "modifier" ? "modifier takes a project, a layer, a property and cycle, noise or none."
                                                 : "clip takes a project, a layer and a saved motion.");
        Scene edited = scene;
        const int layer = args.at(3) == "camera" ? Scene::CameraIndex : edited.indexOf(args.at(3));
        if (layer == -1)
            return fail(QString("There's no layer named \"%1\".").arg(args.at(3)));
        QHash<QString, double> options;
        const int first = args.at(1) == "modifier" ? 6 : 5;
        for (int i = first; i + 1 < args.size(); i += 2)
            options.insert(args.at(i), args.at(i + 1).toDouble());
        if (args.at(1) == "modifier") {
            if (args.size() < 6)
                return fail("modifier takes cycle, noise or none.");
            if (!Project::setModifier(edited, layer, args.at(4), args.at(5), options.value("--amount", 1), options.value("--speed", 2), &error))
                return fail(error);
        } else if (args.at(4) == "--none") {
            while (Project::removeClip(edited, layer, 0)) {
            }
        } else if (!Project::addClip(edited, layer, args.at(4), options.value("--at", 0), int(options.value("--repeat", 1)),
                                     options.value("--speed", 1), options.value("--amount", 1), &error)) {
            return fail(error);
        }
        if (!Project::save(directory, edited, &error))
            return fail(error);
        QTextStream(stdout) << "Done.\n";
        return 0;
    }

    if (args.at(1) == "look") {
        Scene edited = scene;
        if (args.size() == 4 && args.at(3) == "--none") {
            edited.raw.remove("look");
        } else {
            for (int i = 3; i < args.size(); i += 2) {
                bool ok = false;
                const double value = i + 1 < args.size() ? args.at(i + 1).toDouble(&ok) : 0;
                if (!args.at(i).startsWith("--") || !ok)
                    return fail(QString("look takes --setting value pairs; %1 isn't one.").arg(args.at(i)));
                if (!Project::setLook(edited, args.at(i).mid(2), value, &error))
                    return fail(error);
            }
        }
        if (!Project::save(directory, edited, &error))
            return fail(error);
        QTextStream(stdout) << QJsonDocument(edited.raw.value("look").toObject()).toJson(QJsonDocument::Compact) << "\n";
        return 0;
    }

    if (args.at(1) == "body") {
        if (args.size() < 4)
            return fail("body takes a project and a layer.");
        Scene edited = scene;
        const int layer = edited.indexOf(args.at(3));
        if (layer < 0)
            return fail(QString("There's no layer named \"%1\".").arg(args.at(3)));
        QJsonObject body;
        bool none = false;
        for (int i = 4; i < args.size(); ++i) {
            if (args.at(i) == "--none")
                none = true;
            else if (args.at(i) == "--fixed")
                body["fixed"] = true;
            else if (args.at(i) == "--no-tumble")
                body["tumble"] = false;
            else if (i + 1 < args.size() && (args.at(i) == "--mass" || args.at(i) == "--bounce" || args.at(i) == "--friction"))
                body[args.at(i).mid(2)] = args.at(++i).toDouble();
            else
                return fail(QString("body doesn't know %1.").arg(args.at(i)));
        }
        if (!none && body.isEmpty())
            body["mass"] = 1;
        if (!Physics::setBody(edited, layer, none ? QJsonObject() : body, &error) || !Project::save(directory, edited, &error))
            return fail(error);
        QTextStream(stdout) << QString("%1 %2.\n").arg(args.at(3), none ? "isn't a body" : "is a body");
        return 0;
    }

    if (args.at(1) == "simulate") {
        double from = 0, to = scene.duration;
        for (int i = 3; i + 1 < args.size(); i += 2) {
            if (args.at(i) == "--from")
                from = args.at(i + 1).toDouble();
            else if (args.at(i) == "--to")
                to = args.at(i + 1).toDouble();
            else
                return fail(QString("simulate doesn't know %1.").arg(args.at(i)));
        }
        Scene edited = scene;
        const int moved = Physics::bake(edited, assets, from, to, &error);
        if (moved < 0 || !Project::save(directory, edited, &error))
            return fail(error);
        QTextStream(stdout) << QString("%1 bodies baked from %2 s to %3 s.\n").arg(moved).arg(from).arg(to);
        return 0;
    }

    if (args.at(1) == "empty") {
        if (args.size() != 5 && args.size() != 7)
            return fail("empty takes a project, a name and x,y (and --parent and a layer).");
        const QStringList xy = args.at(4).split(',');
        if (xy.size() != 2)
            return fail(QString("%1 isn't a point; write x,y.").arg(args.at(4)));
        const QString parent = args.size() == 7 && args.at(5) == "--parent" ? args.at(6) : QString();
        if (!parent.isEmpty() && scene.indexOf(parent) < 0)
            return fail(QString("There's no layer named \"%1\".").arg(parent));
        Scene edited = scene;
        const int index = Project::addEmpty(edited, args.at(3), parent, QPointF(xy.at(0).toDouble(), xy.at(1).toDouble()));
        if (!Project::save(directory, edited, &error))
            return fail(error);
        QTextStream(stdout) << edited.layers.at(index).name << "\n";
        return 0;
    }

    if (args.at(1) == "gltf") {
        if (args.size() != 4)
            return fail("gltf takes a project and the .glb file to write.");
        QStringList skipped;
        if (!Gltf::write(scene, assets, args.at(3), &error, &skipped))
            return fail(error);
        for (const QString &why : skipped)
            QTextStream(stderr) << "Left out " << why << "\n";
        QTextStream(stdout) << args.at(3) << "\n";
        return 0;
    }

    if (args.at(1) == "steps") {
        if (args.size() != 3 && args.size() != 4)
            return fail("steps takes a project, and a layer if just its steps are wanted.");
        QJsonArray list;
        for (int i = 0; i < scene.layers.size(); ++i) {
            if (args.size() == 4 && scene.layers.at(i).name != args.at(3))
                continue;
            QJsonArray steps;
            for (const Steps::Step &step : Steps::of(scene, assets, i))
                steps.append(QJsonObject{{"step", step.name},
                                         {"title", step.title},
                                         {"detail", step.detail},
                                         {"done", step.done},
                                         {"stale", step.stale},
                                         {"why", step.why},
                                         {"actions", QJsonArray::fromStringList(step.actions)}});
            list.append(QJsonObject{{"layer", scene.layers.at(i).name}, {"steps", steps}});
        }
        if (args.size() == 4 && list.isEmpty())
            return fail(QString("There's no layer named \"%1\".").arg(args.at(3)));
        QTextStream(stdout) << QJsonDocument(list).toJson(QJsonDocument::Indented);
        return 0;
    }

    if (args.at(1) == "redo") {
        Scene edited = scene;
        if (args.size() == 3) {
            QStringList done;
            if (!Steps::redoStale(edited, assets, &error, &done) || !Project::save(directory, edited, &error))
                return fail(error);
            QTextStream(stdout) << (done.isEmpty() ? QString("Nothing is out of date.\n") : "Redone: " + done.join(", ") + ".\n");
            return 0;
        }
        if (args.size() != 5 && args.size() != 6)
            return fail("redo takes a project, or a project, a layer and a step (world, shape, or fill and how).");
        const int layer = edited.indexOf(args.at(3));
        if (layer < 0)
            return fail(QString("There's no layer named \"%1\".").arg(args.at(3)));
        const QString action = args.size() == 6 ? args.at(5) : QString("again");
        if (!Steps::run(edited, assets, layer, args.at(4), action, &error) || !Project::save(directory, edited, &error))
            return fail(error);
        QTextStream(stdout) << QString("Redone: %1 of %2.\n").arg(args.at(4), args.at(3));
        return 0;
    }

    if (args.at(1) == "model") {
        if (args.size() < 5)
            return fail("model takes a project, a file and right,up,ahead.");
        if (!World::info(scene).valid)
            return fail("A 3D model goes into a world: make one first (omotion world).");
        const QStringList at = args.at(4).split(',');
        if (at.size() != 3)
            return fail("Where it stands: right,up,ahead in meters.");
        const double size = args.size() >= 7 && args.at(5) == "--size" ? args.at(6).toDouble() : 1;
        Scene edited = scene;
        const int layer = Project::importModel(edited, directory, args.at(3), QVector3D(at.at(0).toFloat(), at.at(1).toFloat(), at.at(2).toFloat()), size, &error);
        if (layer < 0 || !Project::save(directory, edited, &error))
            return fail(error);
        QTextStream(stdout) << edited.layers.at(layer).name << "\n";
        return 0;
    }

    if (args.at(1) == "track") {
        if (args.size() < 5)
            return fail("track takes a project, a video layer and x,y.");
        Scene edited = scene;
        const int video = edited.indexOf(args.at(3));
        const QStringList xy = args.at(4).split(',');
        if (video < 0 || xy.size() != 2)
            return fail("track takes a video layer's name and a point, x,y.");
        double from = 0, to = edited.duration;
        QString onto;
        for (int i = 5; i + 1 < args.size(); i += 2) {
            if (args.at(i) == "--from")
                from = args.at(i + 1).toDouble();
            else if (args.at(i) == "--to")
                to = args.at(i + 1).toDouble();
            else if (args.at(i) == "--onto")
                onto = args.at(i + 1);
            else
                return fail(QString("track doesn't know %1.").arg(args.at(i)));
        }
        const int follower = Project::track(edited, assets, video, QPointF(xy.at(0).toDouble(), xy.at(1).toDouble()), from, to, &error, onto);
        if (follower < 0 || !Project::save(directory, edited, &error))
            return fail(error);
        QTextStream(stdout) << edited.layers.at(follower).name << " follows it.\n";
        return 0;
    }

    if (args.at(1) == "warp") {
        if (args.size() < 4)
            return fail("warp takes a project and a layer.");
        Scene edited = scene;
        const int layer = edited.indexOf(args.at(3));
        if (layer < 0)
            return fail(QString("There's no layer named \"%1\".").arg(args.at(3)));
        if (args.size() == 4 && !Project::setWarp(edited, layer, "amount", 6, &error))
            return fail(error);
        for (int i = 4; i + 1 < args.size(); i += 2)
            if (!args.at(i).startsWith("--") || !Project::setWarp(edited, layer, args.at(i).mid(2), args.at(i + 1).toDouble(), &error))
                return fail(error.isEmpty() ? QString("warp doesn't know %1.").arg(args.at(i)) : error);
        if (!Project::save(directory, edited, &error))
            return fail(error);
        QTextStream(stdout) << "Done.\n";
        return 0;
    }

    if (args.at(1) == "video") {
        if (args.size() < 4)
            return fail("video takes a project and a clip.");
        Scene edited = scene;
        const int layer = Project::importVideo(edited, directory, args.at(3), &error);
        if (layer < 0)
            return fail(error);
        for (int i = 4; i < args.size(); ++i) {
            const QString arg = args.at(i);
            const bool ok = arg == "--hold" ? Project::setVideo(edited, layer, "loop", false, &error)
                            : (arg == "--start" || arg == "--speed") && i + 1 < args.size()
                                ? Project::setVideo(edited, layer, arg.mid(2), args.at(++i), &error)
                                : (error = QString("video doesn't know %1.").arg(arg), false);
            if (!ok)
                return fail(error);
        }
        if (!Project::save(directory, edited, &error))
            return fail(error);
        QTextStream(stdout) << QString("%1: %2 frames.\n").arg(edited.layers.at(layer).name).arg(edited.layers.at(layer).raw.value("video").toObject().value("count").toInt());
        return 0;
    }

    if (args.at(1) == "light" || args.at(1) == "material") {
        if (args.size() < 4)
            return fail(QString("%1 takes a project and a %2.").arg(args.at(1), args.at(1) == "light" ? "name" : "layer"));
        Scene edited = scene;
        int layer = edited.indexOf(args.at(3));
        int i = 4;
        if (args.at(1) == "light" && layer < 0) {
            if (!World::info(edited).valid)
                return fail("Lights light a world: make one first (omotion world).");
            const QStringList at = args.size() > 4 ? args.at(4).split(',') : QStringList();
            if (at.size() != 3)
                return fail("A new light needs where: right,up,ahead in meters.");
            layer = Project::addLight(edited, args.at(3), QVector3D(at.at(0).toFloat(), at.at(1).toFloat(), at.at(2).toFloat()), {});
            i = 5;
        } else if (layer < 0) {
            return fail(QString("There's no layer named \"%1\".").arg(args.at(3)));
        }
        for (; i + 1 < args.size(); i += 2) {
            const QString name = args.at(i).mid(2);
            const bool ok = args.at(1) == "light" ? Project::setLight(edited, layer, name, args.at(i + 1), &error)
                                                  : Project::setMaterial(edited, layer, name, args.at(i + 1).toDouble(), &error);
            if (!ok)
                return fail(error);
        }
        if (!Project::save(directory, edited, &error))
            return fail(error);
        QTextStream(stdout) << edited.layers.at(layer).name << "\n";
        return 0;
    }

    if (args.at(1) == "mesh") {
        if (args.size() < 5)
            return fail("mesh takes a project, a layer and smooth, simplify or none.");
        Scene edited = scene;
        const int layer = edited.indexOf(args.at(3));
        if (layer < 0)
            return fail(QString("There's no layer named \"%1\".").arg(args.at(3)));
        const int amount = args.size() > 5 ? args.at(5).toInt() : args.at(4) == "smooth" ? 2 : 48;
        if (!World::setShapeModifier(edited, layer, args.at(4), amount, &error) || !Project::save(directory, edited, &error))
            return fail(error);
        QTextStream(stdout) << "Done.\n";
        return 0;
    }

    if (args.at(1) == "size") {
        if (args.size() != 5)
            return fail("size takes a project, a layer, and how tall it really is in meters.");
        Scene edited = scene;
        const int layer = edited.indexOf(args.at(3));
        bool ok = false;
        const double meters = args.at(4).toDouble(&ok);
        if (layer < 0)
            return fail(QString("There's no layer named \"%1\".").arg(args.at(3)));
        if (!ok || !World::setSize(edited, assets, layer, meters, &error) || !Project::save(directory, edited, &error))
            return fail(ok ? error : QString("\"%1\" isn't a size in meters.").arg(args.at(4)));
        QTextStream(stdout) << QString("The world is now %1 times as big as the AI read it.\n")
                                   .arg(World::info(edited).scale, 0, 'f', 2);
        return 0;
    }

    if (args.at(1) == "ground") {
        if (!World::info(scene).valid)
            return fail("There's no world yet; omotion world makes one.");
        QTextStream out(stdout);
        for (int i = 3; i < args.size(); ++i) {
            const QStringList xz = args.at(i).split(',');
            bool a = false, b = false;
            const double x = xz.value(0).toDouble(&a), ahead = xz.value(1).toDouble(&b);
            if (xz.size() != 2 || !a || !b)
                return fail(QString("\"%1\" isn't a place like 3,40 (meters right, ahead).").arg(args.at(i)));
            const double g = World::groundAt(scene, assets, x, -ahead);
            out << args.at(i) << " " << (std::isnan(g) ? QString() : QString::number(g, 'f', 2)) << "\n";
        }
        return 0;
    }

    if (args.at(1) == "drive") {
        if (args.size() < 4)
            return fail("drive takes a project, a layer, --from and --to.");
        Scene edited = scene;
        const int layer = edited.indexOf(args.at(3));
        if (layer < 0)
            return fail(QString("There's no layer named \"%1\".").arg(args.at(3)));
        QPointF from, to;
        bool hasFrom = false, hasTo = false;
        double speed = 60, t = 0;
        bool face = false;
        auto point = [&](const QString &text, QPointF *into) {
            const QStringList xy = text.split(',');
            bool a = false, b = false;
            if (xy.size() == 2)
                *into = QPointF(xy.at(0).toDouble(&a), xy.at(1).toDouble(&b));
            return a && b;
        };
        for (int i = 4; i < args.size(); ++i) {
            if (args.at(i) == "--from" && i + 1 < args.size()) {
                if (!(hasFrom = point(args.at(++i), &from)))
                    return fail(QString("\"%1\" isn't a place like 3,40 (meters right, ahead).").arg(args.at(i)));
            } else if (args.at(i) == "--to" && i + 1 < args.size()) {
                if (!(hasTo = point(args.at(++i), &to)))
                    return fail(QString("\"%1\" isn't a place like -2,5 (meters right, ahead).").arg(args.at(i)));
            } else if (args.at(i) == "--speed") {
                if (!numberAfter(args, i, &speed))
                    return 1;
            } else if (args.at(i) == "--at") {
                if (!numberAfter(args, i, &t))
                    return 1;
            } else if (args.at(i) == "--face") {
                face = true;
            } else {
                return fail(QString("drive doesn't know \"%1\".").arg(args.at(i)));
            }
        }
        if (!hasFrom || !hasTo)
            return fail("drive needs --from and --to.");
        QList<double> landings;
        if (!World::drive(edited, assets, layer, from, to, speed / 3.6, t, &landings, &error, face) ||
            !Project::save(directory, edited, &error))
            return fail(error);
        QStringList times;
        for (const double at : landings)
            times << QString::number(at, 'f', 2);
        QTextStream(stdout) << QString("Driven in %1 s%2.\n")
                                   .arg(QLineF(from, to).length() / (speed / 3.6), 0, 'f', 2)
                                   .arg(times.isEmpty() ? QString() : ", landing at " + times.join(", ") + " s");
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
                                       !std::strcmp(argv[1], "motion") || !std::strcmp(argv[1], "link") || !std::strcmp(argv[1], "world") || !std::strcmp(argv[1], "shape") || !std::strcmp(argv[1], "drive") || !std::strcmp(argv[1], "ground") || !std::strcmp(argv[1], "size") ||
                                       !std::strcmp(argv[1], "steps") || !std::strcmp(argv[1], "gltf") || !std::strcmp(argv[1], "rig") || !std::strcmp(argv[1], "modifier") || !std::strcmp(argv[1], "body") || !std::strcmp(argv[1], "look") || !std::strcmp(argv[1], "mesh") || !std::strcmp(argv[1], "light") || !std::strcmp(argv[1], "video") || !std::strcmp(argv[1], "warp") || !std::strcmp(argv[1], "track") || !std::strcmp(argv[1], "model") || !std::strcmp(argv[1], "material") || !std::strcmp(argv[1], "film") || !std::strcmp(argv[1], "simulate") || !std::strcmp(argv[1], "clip") || !std::strcmp(argv[1], "empty") || !std::strcmp(argv[1], "redo") ||
                                       !std::strcmp(argv[1], "models"));
    // No window, but a world still needs the GPU, which a desktop session's
    // platform gives without showing anything; elsewhere, offscreen.
    if (headless && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QGuiApplication app(argc, argv);
    app.setApplicationName("omotion");
    registerWorld();
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
    // A film isn't a project: its file names the projects.
    if (headless && args.at(1) == "film") {
        auto filmFail = [](const QString &message) {
            QTextStream(stderr) << "omotion: " << message << "\n";
            return 1;
        };
        if (args.size() < 4)
            return filmFail("film takes a film file and an output.");
        Film film;
        QString error;
        if (!film.load(args.at(2), &error))
            return filmFail(error);
        if (args.at(3) == "--frame") {
            if (args.size() != 6)
                return filmFail("film --frame takes a time and a .png.");
            if (!film.frame(args.at(4).toDouble(), film.size()).save(args.at(5)))
                return filmFail(QString("Couldn't write %1.").arg(args.at(5)));
            return 0;
        }
        const double scale = args.size() == 6 && args.at(4) == "--scale" ? args.at(5).toDouble() : 1;
        if (!(scale > 0 && scale <= 4))
            return filmFail("Scale films between 0 and 4 times their size.");
        QSize size(int(std::lround(film.size().width() * scale)), int(std::lround(film.size().height() * scale)));
        size = QSize(std::max(2, size.width() - size.width() % 2), std::max(2, size.height() - size.height() % 2));
        const Exporter::Source source = [&film](int frame, const QSize &at) { return film.frame(frame / film.fps(), at); };
        const QString output = QFileInfo(args.at(3)).absoluteFilePath();
        if (!Exporter::runFrames(source, film.frameCount(), film.fps(), size, output, &error))
            return filmFail(error);
        // MP4 and WebM carry the shots' sound; GIFs and frames don't.
        const QString format = Exporter::formatFor(output);
        if ((format == "mp4" || format == "webm") && !film.addSound(output, &error))
            return filmFail(error);
        QTextStream(stdout) << QFileInfo(args.at(3)).absoluteFilePath() << "\n";
        return 0;
    }
    if (headless) {
        const bool short_ = args.at(1) == "split" || args.at(1) == "clean" || args.at(1) == "info" || args.at(1) == "world" ||
                            args.at(1) == "simulate" || args.at(1) == "steps" || args.at(1) == "redo";
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
    qmlRegisterType<GraphView>("Omotion", 1, 0, "GraphView");

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
