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

#include "backend.h"
#include "exporter.h"
#include "portalfilepicker.h"
#include "project.h"
#include "sceneview.h"
#include "theme.h"

namespace {

const char *Usage = R"(Usage:
  omotion [image | folder]               Open the editor
  omotion render <project> <output>      Render to .mp4, .webm, .gif, or a folder of PNG frames
  omotion frame <project> <seconds> <output.png>
                                         Render one frame as a PNG
  omotion cut <project> <layer> <x,y> <x,y> <x,y>... [--copy] [--name <name>] [--soft <px>] [--at <seconds>]
                                         Cut the part of a layer inside a polygon (canvas
                                         pixels) out into a new layer, as the C tool does

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
        for (int i = 4; i < args.size(); ++i) {
            const QString arg = args.at(i);
            if (arg == "--copy") {
                punch = false;
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
        const int piece = Project::cutOut(edited, assets, layer, polygon, t, punch, &error, name, feather);
        if (piece < 0)
            return fail(error);
        if (!Project::save(directory, edited, &error))
            return fail(error);
        QTextStream(stdout) << edited.layers.at(piece).name << "\n";
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

    if (args.size() != 4)
        return fail("render takes a project and an output.");
    if (!Exporter::run(scene, directory, args.at(3), &error))
        return fail(error);
    QTextStream(stdout) << args.at(3) << "\n";
    return 0;
}

}

int main(int argc, char *argv[]) {
    const bool headless = argc > 1 && (!std::strcmp(argv[1], "render") || !std::strcmp(argv[1], "frame") ||
                                       !std::strcmp(argv[1], "cut"));
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
    if (headless) {
        if (args.size() < 4)
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
