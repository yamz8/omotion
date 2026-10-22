#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include "backend.h"
#include "filepicker.h"
#include "scene.h"
#include "theme.h"
#include "translator.h"

namespace {
// A scene with everything a check looks for switched off, to which a test adds one fault.
QByteArray sceneSource(const QByteArray &extra = {}, const QByteArray &declarations = {}) {
    return "import QtQuick\nimport Omotion\nRectangle {\n"
           "    property real t\n    property real duration: 2\n    property int fps: 10\n"
           "    property var formats: [\"16:9\", \"9:16\"]\n"
           "    property var beats: [{ at: 0, name: \"One\" }, { at: 1, name: \"Two\" }]\n" +
           declarations + "    color: \"black\"\n"
           "    Rectangle { width: 100; height: 100; color: \"white\"; x: Motion.progress(parent.t, 0, 2) * 800 }\n" +
           extra + "}\n";
}

class FakePicker : public FilePicker {
public:
    void openScene(const QUrl &) override {}
    void newScene(const QUrl &) override {}
};
}

class OmotionTests : public QObject {
    Q_OBJECT

private slots:
    void formatsAreNamedBothWays()
    {
        Format format;
        QVERIFY(Formats::find("9x16", &format));
        QCOMPARE(format.name, QString("9:16"));
        QCOMPARE(format.size, QSize(1080, 1920));
        QCOMPARE(format.slug(), QString("9x16"));
        QVERIFY(!Formats::find("4:3"));
    }

    void motionHelpersArePure()
    {
        registerSceneTypes();
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData("import QtQml\nimport Omotion\nQtObject {\n"
                          "  property real before: Motion.spring(0.5, 1)\n"
                          "  property real settled: Motion.spring(5, 1)\n"
                          "  property real peak: Math.max(Motion.spring(1.2, 1, 300, 8), Motion.spring(1.25, 1, 300, 8), Motion.spring(1.3, 1, 300, 8))\n"
                          "  property real overdamped: Motion.spring(3, 0, 50, 40)\n"
                          "  property real middle: Motion.progress(1.5, 1, 2)\n"
                          "  property bool repeatable: Motion.random(7) === Motion.random(7) && Motion.random(7) !== Motion.random(8)\n"
                          "  property int beat: Motion.beatAt([{ at: 0 }, { at: 2 }], 2.5)\n}", QUrl());
        QScopedPointer<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        QCOMPARE(object->property("before").toDouble(), 0.0);
        QVERIFY(qAbs(object->property("settled").toDouble() - 1) < 1e-3);
        QVERIFY(object->property("peak").toDouble() > 1.05);
        QVERIFY(object->property("overdamped").toDouble() > 0.9 && object->property("overdamped").toDouble() < 1);
        QCOMPARE(object->property("middle").toDouble(), 0.5);
        QVERIFY(object->property("repeatable").toBool());
        QCOMPARE(object->property("beat").toInt(), 1);
    }

    void starterSceneIsClean()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("scene.qml");
        QVERIFY(QFile::copy(":/starter.qml", path));
        for (const auto &format : {"16:9", "9:16"}) {
            Scene scene;
            QString error;
            QVERIFY2(scene.load(path, format, &error), qPrintable(error));
            QCOMPARE(scene.duration(), 9.0);
            QCOMPARE(scene.frameCount(), 270);
            QCOMPARE(scene.beats().size(), 4);
            QCOMPARE(scene.formats(), QStringList({"16:9", "9:16"}));
            QCOMPARE(scene.frame(1).size(), scene.format().size);
            for (const auto &problem : scene.check())
                QFAIL(qPrintable(QString("%1 %2").arg(format, problem.message)));
        }
    }

    void sceneNeedsTimeAndDuration()
    {
        QTemporaryDir dir;
        write(dir.filePath("a.qml"), "import QtQuick\nItem { property real duration: 2 }\n");
        write(dir.filePath("b.qml"), "import QtQuick\nItem { property real t }\n");
        write(dir.filePath("c.qml"), "import QtQuick\nItem { property real t; property real duration: 2; Nope {} }\n");
        Scene scene;
        QString error;
        QVERIFY(!scene.load(dir.filePath("a.qml"), "16:9", &error));
        QVERIFY(error.contains("property real t"));
        QVERIFY(!scene.load(dir.filePath("b.qml"), "16:9", &error));
        QVERIFY(error.contains("duration"));
        QVERIFY(!scene.load(dir.filePath("c.qml"), "16:9", &error));
        QVERIFY2(error.contains("c.qml:2") && error.contains("Nope"), qPrintable(error));
    }

    void checkFindsClocks()
    {
        QCOMPARE(problemsFor(sceneSource()).size(), 0);
        QVERIFY(anyMessage(problemsFor(sceneSource("    Behavior on opacity { NumberAnimation {} }\n")), "Behavior runs on the wall clock"));
        QVERIFY(anyMessage(problemsFor(sceneSource("    Timer { running: true }\n")), "Timer runs on the wall clock"));
        QVERIFY(anyMessage(problemsFor(sceneSource("    opacity: Math.random()\n")), "calls Math.random()"));
    }

    void checkFindsStateBetweenFrames()
    {
        const auto problems = problemsFor(sceneSource("    property int seen: 0\n    onTChanged: seen++\n"
                                                      "    Rectangle { width: 10 + seen * 10; height: 10; color: \"red\" }\n"));
        QVERIFY(anyMessage(problems, "depends on what was rendered before it"));
    }

    void checkFindsTextOutsideTheFrame()
    {
        const QByteArray wide = "    Text { x: 40; y: 300; text: \"A headline much too wide to fit\"; font.pixelSize: 100; color: \"white\" }\n";
        QCOMPARE(problemsFor(sceneSource(wide), "16:9").size(), 0);
        QVERIFY(anyMessage(problemsFor(sceneSource(wide), "9:16"), "runs outside the 9:16 frame"));
        // Clipped away by its parent, it isn't on screen to overflow.
        QCOMPARE(problemsFor(sceneSource("    Item { width: 600; height: 600; clip: true\n" + wide + "    }\n"), "9:16").size(), 0);
    }

    void checkReadsBeatsAndAudio()
    {
        const auto problems = problemsFor(sceneSource({}, "    property string audio: \"missing.wav\"\n").replace(
            "{ at: 1, name: \"Two\" }", "{ at: 3, name: \"Late\" }"));
        QVERIFY(anyMessage(problems, "outside the scene's 2.00s"));
        QVERIFY(anyMessage(problems, "missing.wav does not exist"));
    }

    void contactSheetShowsTheMiddleOfEachBeat()
    {
        QTemporaryDir dir;
        write(dir.filePath("scene.qml"), sceneSource());
        Scene scene;
        QString error;
        QVERIFY(scene.load(dir.filePath("scene.qml"), "16:9", &error));
        QCOMPARE(scene.sheetTimes(), QList<double>({0.5, 1.5}));
        QCOMPARE(scene.beatNameAt(1.2), QString("Two"));
        const QImage sheet = scene.contactSheet(scene.sheetTimes());
        QVERIFY(sheet.width() > sheet.height());
    }

    void rendersAtomically()
    {
        if (QStandardPaths::findExecutable("ffmpeg").isEmpty())
            QSKIP("ffmpeg is not installed");
        QTemporaryDir dir;
        write(dir.filePath("scene.qml"), sceneSource());
        Scene scene;
        QString error;
        QVERIFY(scene.load(dir.filePath("scene.qml"), "9:16", &error));
        const QString output = Scene::defaultOutput(scene.path(), "9:16");
        QCOMPARE(QFileInfo(output).fileName(), QString("scene-9x16.mp4"));
        int last = 0;
        QVERIFY2(scene.render(output, &error, [&](int frame, int) { last = frame; }), qPrintable(error));
        QCOMPARE(last, 20);
        QVERIFY(QFileInfo(output).size() > 0);
        QVERIFY(!QFileInfo::exists(Scene::partialPath(output)));

        QProcess probe;
        probe.start("ffprobe", {"-v", "error", "-count_frames", "-select_streams", "v:0", "-show_entries",
                                "stream=width,height,nb_read_frames", "-of", "json", output});
        QVERIFY(probe.waitForFinished());
        const QJsonObject stream = QJsonDocument::fromJson(probe.readAllStandardOutput())["streams"][0].toObject();
        QCOMPARE(stream["width"].toInt(), 1080);
        QCOMPARE(stream["height"].toInt(), 1920);
        QCOMPARE(stream["nb_read_frames"].toString(), QString("20"));
    }

    void backendReloadsWhenTheSceneIsReplaced()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("scene.qml");
        write(path, sceneSource());
        FakePicker picker;
        Backend backend(&picker);
        QVERIFY(backend.load(path));
        QCOMPARE(backend.sceneName(), QString("scene.qml"));
        const int revision = backend.revision();
        QVERIFY(backend.sceneUrl().query().contains(QString::number(revision)));

        // Written beside and renamed over, as editors and agents save.
        QTest::qWait(20);
        write(path + ".new", sceneSource("    opacity: 0.5\n"));
        QVERIFY(QFile::remove(path));
        QVERIFY(QFile::rename(path + ".new", path));
        QTRY_VERIFY(backend.revision() > revision);
        QVERIFY(backend.resolve("track.wav").toLocalFile() == dir.filePath("track.wav"));
    }

    void backendNeverOverwritesAScene()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("mine.qml");
        write(path, "keep me");
        FakePicker picker;
        Backend backend(&picker);
        QVERIFY(!backend.createScene(path));
        QVERIFY(backend.messageIsError());
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("keep me"));
        QVERIFY(backend.createScene(dir.filePath("new.qml")));
        QVERIFY(!backend.load(dir.filePath("notes.txt")));
    }

    void themeFallsBack()
    {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QString("black"));
    }

    void themeReadsAccent()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.path() + "/theme/colors.toml", "accent = \"#112233\"\n");
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#112233"));
        QCOMPARE(theme.accentForeground(), QString("white"));
    }

    void themeFollowsHyprlandRounding()
    {
        QTemporaryDir dir;
        const QString response = dir.filePath("response.json"), command = dir.filePath("hyprctl");
        write(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n");
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        write(response, "{\"int\":0}");

        Theme theme(dir.path(), nullptr, command);
        QCOMPARE(theme.radius(), 0);
        write(response, "{\"int\":6}");
        QTRY_COMPARE(theme.radius(), 6);
    }

    void translatorFallsBackToEnglish()
    {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Save").isNull());
    }

    void translatorFollowsTheLanguageFile()
    {
        QTemporaryDir dir;
        write(dir.path() + "/language", "pt-BR\n");
        write(dir.path() + "/pt.json", R"({"Save": "Guardar", "Open": "Abrir"})");
        write(dir.path() + "/pt-BR.json", R"({"Save": "Salvar"})");

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("pt-BR"));
        QCOMPARE(translator.translate("Main", "Save"), QString("Salvar"));
        QCOMPARE(translator.translate("Main", "Open"), QString("Abrir"));
        QVERIFY(translator.translate("Main", "Quit").isNull());
    }

    void translatorPicksPluralForms()
    {
        QTemporaryDir dir;
        write(dir.path() + "/language", "ru\n");
        write(dir.path() + "/en.json", R"({"%n clips": {"one": "%n clip", "other": "%n clips"}})");
        write(dir.path() + "/ru.json", R"({"%n clips": {"one": "%n клип", "few": "%n клипа", "many": "%n клипов", "other": "%n клипа"}})");

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 21), QString("%n клип"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 22), QString("%n клипа"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 25), QString("%n клипов"));
        QCOMPARE(Translator::pluralCategory("ar", 11), QString("many"));
        QCOMPARE(Translator::pluralCategory("ja", 1), QString("other"));
    }

    // Every shipped catalog parses and keeps each placeholder of its English.
    void catalogsKeepTheirPlaceholders()
    {
        QDirIterator it(":/i18n", {"*.json"});
        int catalogs = 0;
        while (it.hasNext()) {
            QFile file(it.next());
            QVERIFY(file.open(QIODevice::ReadOnly));
            QJsonParseError error;
            const QJsonObject catalog = QJsonDocument::fromJson(file.readAll(), &error).object();
            QVERIFY2(error.error == QJsonParseError::NoError, qPrintable(file.fileName()));
            static const QRegularExpression placeholder("%\\d");
            for (auto entry = catalog.begin(); entry != catalog.end(); ++entry) {
                if (!entry.value().isString())
                    continue;
                QStringList wanted, found;
                for (auto m = placeholder.globalMatch(entry.key()); m.hasNext();) wanted << m.next().captured();
                for (auto m = placeholder.globalMatch(entry.value().toString()); m.hasNext();) found << m.next().captured();
                wanted.sort();
                found.sort();
                QVERIFY2(wanted == found, qPrintable(file.fileName() + ": " + entry.key()));
            }
            ++catalogs;
        }
        QVERIFY(catalogs >= 2);
    }

private:
    static void write(const QString &path, const QByteArray &contents)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(contents);
    }

    static QList<Problem> problemsFor(const QByteArray &source, const QString &format = "16:9")
    {
        QTemporaryDir dir;
        write(dir.filePath("scene.qml"), source);
        Scene scene;
        QString error;
        if (!scene.load(dir.filePath("scene.qml"), format, &error)) {
            qWarning().noquote() << error;
            return {Problem{true, error, {}, -1}};
        }
        return scene.check();
    }

    static bool anyMessage(const QList<Problem> &problems, const QString &text)
    {
        for (const auto &problem : problems)
            if (problem.message.contains(text))
                return true;
        for (const auto &problem : problems)
            qWarning().noquote() << "problem:" << problem.message;
        return false;
    }
};

QTEST_MAIN(OmotionTests)
#include "omotion_tests.moc"
