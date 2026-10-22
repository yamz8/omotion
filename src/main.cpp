// omotion — preview, review, and render motion scenes written as functions of time.

#include <QCommandLineParser>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <cstdio>

#include "backend.h"
#include "cli.h"
#include "portalfilepicker.h"
#include "scene.h"
#include "theme.h"
#include "translator.h"

int main(int argc, char *argv[]) {
    // Commands draw no window, so they must not need a display, even where the
    // desktop exports QT_QPA_PLATFORM=wayland. Bare omotion prints help, as a
    // command line tool should; the launcher says omotion open.
    const bool command = argc == 1 || isCliCommand(argv[1]);
    bool windowless = command;
    for (int i = 1; i < argc; ++i) {
        const QByteArray argument(argv[i]);
        windowless = windowless || argument == "--help" || argument == "-h" || argument == "--version" ||
                     argument == "-v";
    }
    if (windowless) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        // Nothing here is shown, so there's nothing for accessibility to reach.
        qputenv("NO_AT_BRIDGE", "1");
    }

    QGuiApplication app(argc, argv);
    app.setApplicationName("omotion");
    app.setOrganizationName("omotion");
    app.setApplicationVersion("0.1.0");

    // Matches the window to omotion.desktop, so the compositor (Wayland app_id)
    // and the launcher pick up the installed icon.
    app.setDesktopFileName("omotion");
    app.setWindowIcon(QIcon::fromTheme("omotion"));

    if (command)
        return runCli(app.arguments());

    QCommandLineParser args;
    args.setApplicationDescription("Preview, review, and render motion scenes.\n\n" + cliSummary());
    args.addHelpOption();
    args.addVersionOption();
    args.addPositionalArgument("scene", "Scene file (.qml)", "[scene]");
    args.addOption({"at", "Start at this time", "seconds"});
    args.addOption({"format", "Start in this format", "format"});
    args.addOption({"sheet", "Start on the contact sheet"});
    args.addOption({"screenshot", "Save a picture of the window and exit", "file"});
    QStringList arguments = app.arguments();
    if (arguments.value(1) == "open")
        arguments.removeAt(1);
    args.process(arguments);

    QQuickStyle::setStyle("Material");
    registerSceneTypes();

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    Theme theme;
    PortalFilePicker picker;
    Backend backend(&picker);

    QQmlApplicationEngine engine;
    backend.setEngine(&engine);
    // A scene's runtime errors show in the window, which is where its author is looking.
    QObject::connect(&engine, &QQmlEngine::warnings, &backend, [&backend](const QList<QQmlError> &warnings) {
        for (const auto &warning : warnings) {
            fprintf(stderr, "%s\n", qPrintable(warning.toString()));
            if (QFileInfo(warning.url().toLocalFile()).absolutePath() == QFileInfo(backend.scenePath()).absolutePath())
                backend.addWarning(QString("%1:%2: %3").arg(warning.url().fileName()).arg(warning.line())
                                       .arg(warning.description()));
        }
    });
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.rootContext()->setContextProperty("startTime", args.value("at").toDouble());
    engine.rootContext()->setContextProperty("startFormat", args.value("format"));
    engine.rootContext()->setContextProperty("startSheet", args.isSet("sheet"));

    // The last scene opens again, as an editor's last file would.
    QSettings settings;
    QObject::connect(&backend, &Backend::sceneChanged, &app, [&] {
        settings.setValue("lastScene", backend.scenePath());
    });
    const QString scene = args.positionalArguments().value(0, settings.value("lastScene").toString());
    if (!scene.isEmpty() && (QFileInfo::exists(scene) || !args.positionalArguments().isEmpty()))
        backend.load(scene);

    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    if (args.isSet("screenshot")) {
        QTimer::singleShot(1500, &app, [&] {
            auto window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
            const bool ok = window && window->grabWindow().save(args.value("screenshot"));
            app.exit(ok ? 0 : 1);
        });
    }
    return app.exec();
}
