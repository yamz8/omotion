#pragma once

#include <QString>

class Backend;
class QQmlApplicationEngine;
class Theme;
class Translator;

// Where the window starts: the moment, the format, and whether on the sheet.
struct Start {
    double time = 0;
    QString format;
    bool sheet = false;
};

// Connects the engine to the app the way the window expects: the objects QML
// reads, live retranslation, and the scene's runtime errors shown in the
// window. main() and the window tests both use it, so they can't drift apart.
void setUpEngine(QQmlApplicationEngine &engine, Backend &backend, Translator &translator, Theme &theme,
                 const Start &start);
