#include "engine.h"

#include "backend.h"
#include "theme.h"
#include "translator.h"

#include <QFileInfo>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <cstdio>

void setUpEngine(QQmlApplicationEngine &engine, Backend &backend, Translator &translator, Theme &theme,
                 const Start &start) {
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
    engine.rootContext()->setContextProperty("startTime", start.time);
    engine.rootContext()->setContextProperty("startFormat", start.format);
    engine.rootContext()->setContextProperty("startSheet", start.sheet);
}
