#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>
#include <numeric>
#include <random>
#include <QElapsedTimer>

#include "backend.h"
#include "filepicker.h"
#include "deliver.h"
#include "measure.h"
#include "reference.h"
#include "review.h"
#include "sound.h"
#include "style.h"
#include "scene.h"
#include "studio.h"
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
    void newFilm(const QUrl &) override {}
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
        // The Instagram feed's tall post, which two test agents asked for.
        QVERIFY(Formats::find("4x5", &format));
        QCOMPARE(format.size, QSize(1080, 1350));
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
        for (const auto &format : {"16:9", "9:16", "1:1", "4:5"}) {
            Scene scene;
            QString error;
            QVERIFY2(scene.load(path, format, &error), qPrintable(error));
            QCOMPARE(scene.duration(), 9.0);
            QCOMPARE(scene.frameCount(), 270);
            QCOMPARE(scene.beats().size(), 4);
            QCOMPARE(scene.formats(), QStringList({"16:9", "9:16", "1:1", "4:5"}));
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

    // State set between the moments a normal check samples is found only by
    // asking for every frame.
    void thoroughCheckAsksForEveryFrame()
    {
        QTemporaryDir dir;
        write(dir.filePath("s.qml"), sceneSource("    property bool passed: false\n"
                                                 "    onTChanged: if (t > 1.15 && t < 1.25) passed = true\n"
                                                 "    Rectangle { width: 300; height: 300; color: \"red\"; visible: parent.passed }\n"));
        QString error;
        Scene sampled, thorough;
        QVERIFY(sampled.load(dir.filePath("s.qml"), "16:9", &error));
        QVERIFY(!anyMessage(sampled.check(), "rendered before it"));
        QVERIFY(thorough.load(dir.filePath("s.qml"), "16:9", &error));
        QVERIFY(anyMessage(thorough.check(true), "of 20 frames, the first at"));
    }

    void checkFindsTextOutsideTheFrame()
    {
        const QByteArray wide = "    Text { x: 40; y: 300; text: \"A headline much too wide to fit\"; font.pixelSize: 100; color: \"white\" }\n";
        QVERIFY(!anyMessage(problemsFor(sceneSource(wide), "16:9"), "outside the"));
        QVERIFY(anyMessage(problemsFor(sceneSource(wide), "9:16"), "runs outside the 9:16 frame"));
        // Clipped away by its parent, it isn't on screen to overflow.
        QVERIFY(!anyMessage(problemsFor(sceneSource("    Item { width: 600; height: 600; clip: true\n" + wide + "    }\n"), "9:16"),
                            "outside the"));
    }

    void checkReadsBeatsAndAudio()
    {
        const auto problems = problemsFor(sceneSource({}, "    property string audio: \"missing.wav\"\n").replace(
            "{ at: 1, name: \"Two\" }", "{ at: 3, name: \"Late\" }"));
        QVERIFY(anyMessage(problems, "outside the scene's 2.00s"));
        QVERIFY(anyMessage(problems, "missing.wav does not exist"));
    }

    // The review checklist's text items, measured at every frame.
    void measuresTextAViewerCanRead()
    {
        auto text = [](const QByteArray &items, const QString &format = "16:9") {
            QTemporaryDir dir;
            write(dir.filePath("s.qml"), sceneSource(items));
            Scene scene;
            QString error;
            if (!scene.load(dir.filePath("s.qml"), format, &error))
                return QList<Problem>{Problem{true, error, {}, -1, {}}};
            return Measure::text(scene);
        };
        const QByteArray big = "font.pixelSize: 60; color: \"white\"";
        QCOMPARE(text("    Text { x: 300; y: 300; text: \"Readable\"; " + big + " }\n").size(), 0);

        // On screen for 0.2 s, where 0.6 s is the least anything needs.
        const QList<Problem> brief = text("    Text { x: 300; y: 300; text: \"Gone\"; " + big +
                                          "; visible: parent.t >= 0.5 && parent.t < 0.7 }\n");
        QVERIFY(anyMessage(brief, "Text \"Gone\" is fully on screen for 0.20 s (0.50s to 0.70s)"));
        QCOMPARE(brief.first().at, 0.5);
        QVERIFY(!brief.first().area.isEmpty());

        // Typing out, it's read in its final form only.
        QCOMPARE(text("    Text { x: 300; y: 300; text: \"Typed out\".substring(0, Math.ceil(parent.t * 20)); " + big +
                      " }\n").size(), 0);

        QVERIFY(anyMessage(text("    Text { x: 300; y: 300; text: \"Small\"; font.pixelSize: 30 }\n"),
                           "Text at 30 px is 6.1 px when the film plays 390 px wide on a phone; make it at least 50 px "
                           "in 16:9: \"Small\"."));
        // 30 px is readable in a 1080-wide frame.
        QVERIFY(!anyMessage(text("    Text { x: 300; y: 600; text: \"Small\"; font.pixelSize: 30 }\n", "9:16"), "px"));
        // A scaled parent scales the text it holds.
        QVERIFY(anyMessage(text("    Item { scale: 0.5; transformOrigin: Item.TopLeft\n"
                                "        Text { x: 300; y: 300; text: \"Shrunk\"; " + big + " } }\n"),
                           "Text at 30 px"));

        QVERIFY(anyMessage(text("    Text { x: 20; y: 300; text: \"Edge\"; " + big + " }\n"),
                           "Text \"Edge\" crosses the 16:9 safe area"));
        // The bottom fifth of a vertical film sits under the platform's controls.
        QVERIFY(anyMessage(text("    Text { x: 200; y: 1700; text: \"Low\"; " + big + " }\n", "9:16"),
                           "crosses the 9:16 safe area (the top 12%, the bottom 20%"));

        QVERIFY(anyMessage(text("    Text { x: 300; y: 300; text: \"First\"; " + big + " }\n"
                                "    Text { x: 320; y: 310; text: \"Second\"; " + big + " }\n"),
                           "runs into text"));
        // Text set on a card belongs there; text crossing its edge has run into it.
        const QByteArray card = "    Rectangle { x: 600; y: 600; width: 400; height: 200; color: \"#333\" }\n";
        QCOMPARE(text(card + "    Text { x: 620; y: 620; text: \"On it\"; " + big + " }\n").size(), 0);
        QVERIFY(anyMessage(text(card + "    Text { x: 900; y: 620; text: \"Across it\"; " + big + " }\n"),
                           "Text \"Across it\" runs into a 400 x 200 rectangle from 0.00s"));
    }

    void measuresTheHookAndTheFastestChanges()
    {
        QTemporaryDir dir;
        // Black until a square arrives at 2.5 s.
        write(dir.filePath("late.qml"), "import QtQuick\nRectangle {\n    property real t\n    property real duration: 3\n"
                                        "    color: \"black\"\n    Rectangle { width: 300; height: 300; color: \"white\"; "
                                        "visible: parent.t > 2.5 }\n}\n");
        Scene late;
        QString error;
        QVERIFY2(late.load(dir.filePath("late.qml"), "16:9", &error), qPrintable(error));
        const QList<Problem> hook = Measure::hook(late);
        QVERIFY(anyMessage(hook, "Frame 0 shows only one color"));
        QVERIFY(anyMessage(hook, "Nothing visibly changes in the first two seconds"));

        Scene small;
        QVERIFY(small.load(dir.filePath("late.qml"), "16:9", &error, 0.1));
        QCOMPARE(small.frame(0).size(), QSize(192, 108));
        const QList<double> changes = Measure::changes(small);
        QCOMPARE(changes.size(), 90);
        QCOMPARE(Measure::fastestChanges(changes, 30), QList<double>{76 / 30.0});

        QList<double> made(40, 0);
        made[5] = 0.3, made[6] = 0.2, made[20] = 0.1, made[30] = 0.05, made[35] = 0.01;
        QCOMPARE(Measure::fastestChanges(made, 10, 3, 0.5), QList<double>({0.5, 2.0, 3.0}));
    }

    void measuresTheFilmAgainstItsStyleGuide()
    {
        QTemporaryDir dir;
        QString error;
        QVERIFY(Studio::create(dir.filePath("film"), &error));
        const QString root = dir.filePath("film");
        write(QDir(root).filePath("scene.qml"),
              sceneSource("    Text { x: 300; y: 300; text: \"Hi\"; font.family: \"Serif Pro\"; color: \"#ff0000\" }\n"));
        Scene scene;
        QVERIFY(scene.load(QDir(root).filePath("scene.qml"), "16:9", &error));
        // The template guide holds no palette yet.
        QCOMPARE(Measure::style(scene, QDir(root).filePath("style-guide.md")).size(), 0);
        write(QDir(root).filePath("style-guide.md"), "Background black, type #FFFFFF in Serif Pro.\n");
        const QList<Problem> off = Measure::style(scene, QDir(root).filePath("style-guide.md"));
        QCOMPARE(off.size(), 1);
        QVERIFY(off[0].message.startsWith("Colors that style-guide.md's palette doesn't name: #ff0000 (beat \"One\")"));
    }

    void reviewWritesTheNextRoundWithItsEvidence()
    {
        QTemporaryDir dir;
        QString error;
        QVERIFY(Studio::create(dir.filePath("film"), &error));
        const QString scene = dir.filePath("film/scene.qml");
        write(scene, sceneSource("    Text { x: 300; y: 300; text: \"Small\"; font.pixelSize: 20; color: \"white\" }\n"));
        QJsonObject result;
        QVERIFY2(Review::write(scene, {"16:9", "9:16"}, &result, &error), qPrintable(error));
        QCOMPARE(result["number"].toString(), QString("001"));
        QVERIFY(result["sent_back"].toBool());
        QCOMPARE(result["scores"].toObject()["readability"].toInt(), 3);
        const QJsonObject defect = result["defects"].toArray()[0].toObject();
        QVERIFY(defect["message"].toString().startsWith("Text at 20 px"));
        QCOMPARE(defect["evidence"].toString(), QString("reviews/001/defect-1-16x9.png"));
        // The crop holds the text, not the whole frame.
        const QImage crop(dir.filePath("film/" + defect["evidence"].toString()));
        QVERIFY(!crop.isNull() && crop.width() < 1920);
        for (const auto &file : {"001.md", "001/scene-16x9-sheet.png", "001/scene-9x16-sheet.png", "001/poster-16x9.png",
                                 "001/poster-9x16.png"})
            QVERIFY2(QFileInfo(dir.filePath(QString("film/reviews/") + file)).isFile(), file);
        QFile text(dir.filePath("film/reviews/001.md"));
        QVERIFY(text.open(QIODevice::ReadOnly));
        const QString markdown = QString::fromUtf8(text.readAll());
        QVERIFY(markdown.contains("**Verdict:** Sent back"));
        QVERIFY(markdown.contains("Local fix: ✎"));

        // The next round never writes over the last.
        write(scene, sceneSource().replace("{ at: 0, name: \"One\" }, { at: 1, name: \"Two\" }",
                                           "{ at: 0, name: \"One\", purpose: \"a\", enter: \"b\", exit: \"c\" }, "
                                           "{ at: 1, name: \"Two\", purpose: \"a\", enter: \"b\", exit: \"c\" }"));
        QVERIFY(Review::write(scene, {"16:9"}, &result, &error));
        QCOMPARE(result["number"].toString(), QString("002"));
        QVERIFY(!result["sent_back"].toBool());
        QVERIFY(QFileInfo(dir.filePath("film/reviews/001/defect-1-16x9.png")).isFile());
    }

    void findsTheBeatOfATrack()
    {
        // Kicks on each beat at 97 BPM, ticks between them, and a bed of noise.
        QTemporaryDir dir;
        const double period = 60.0 / 97;
        QList<Sound::Cue> drums;
        for (double at = 0.31; at < 12; at += period) {
            drums << Sound::Cue{at, "hit"};
            drums << Sound::Cue{at + period / 2, "tick"};
        }
        QString error;
        QVERIFY(Sound::writeCues(drums, 12, dir.filePath("drums.wav"), &error));
        const QList<float> samples = Sound::decode(dir.filePath("drums.wav"), 22050, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(samples.size(), 12 * 22050);
        const Sound::Grid grid = Sound::beats(samples, 22050);
        QVERIFY2(std::abs(grid.tempo - 97) < 1, qPrintable(QString::number(grid.tempo)));
        // Every kick found, within a frame at 30 fps.
        for (int i = 0; i < drums.size(); i += 2) {
            const double kick = drums[i].at;
            const double nearest = *std::min_element(grid.beats.cbegin(), grid.beats.cend(), [&](double a, double b) {
                return std::abs(a - kick) < std::abs(b - kick);
            });
            QVERIFY2(std::abs(nearest - kick) < 1 / 30.0, qPrintable(QString("%1 vs %2").arg(kick).arg(nearest)));
        }
        const QList<float> peaks = Sound::peaks(samples, 100);
        QCOMPARE(peaks.size(), 100);
        QCOMPARE(*std::max_element(peaks.cbegin(), peaks.cend()), 1.0f);
    }

    void cuesLandOnWhatTheyBelongTo()
    {
        QTemporaryDir dir;
        // A square appears at 1.0 s; the beats are at 0 and 1 s, at 10 fps.
        auto problems = [&](const QByteArray &cues, const QByteArray &more = {}) {
            write(dir.filePath("s.qml"),
                  "import QtQuick\nRectangle {\n    property real t\n    property real duration: 2\n"
                  "    property int fps: 10\n    color: \"black\"\n"
                  "    property var beats: [{ at: 0, name: \"One\" }, { at: 1, name: \"Two\" }]\n"
                  "    property var cues: [" + cues + "]\n" + more +
                  "    Rectangle { width: 400; height: 400; color: \"white\"; visible: parent.t >= 1 }\n}\n");
            Scene scene;
            QString error;
            if (!scene.load(dir.filePath("s.qml"), "16:9", &error))
                return QList<Problem>{Problem{true, error, {}, -1, {}}};
            return scene.check();
        };
        QVERIFY(!anyMessage(problems(R"({ at: 1.0, name: "pop", sound: "click" })"), "Cue"));
        QVERIFY(anyMessage(problems(R"({ at: 1.3, name: "pop", sound: "click" })"),
                           "Cue \"pop\" at 1.30s is 3 frames after the change on screen at 1.00s."));
        QVERIFY(anyMessage(problems(R"({ at: 0.5, name: "pop" })"), "Nothing visible changes within 0.25 s of cue"));
        QVERIFY(anyMessage(problems(R"({ at: 1.2, name: "pop", on: "Two" })"),
                           "Cue \"pop\" at 1.20s is 2 frames after the beat \"Two\" it's on, at 1.00s."));
        QVERIFY(anyMessage(problems(R"({ at: 1.0, name: "pop", on: "Three" })"), "which isn't one of the beats"));
        QVERIFY(anyMessage(problems(R"({ at: 1.0, name: "pop", sound: "boom" })"), "use click, tick, hit, whoosh"));

        // Against the music: the track starts 0.5 s in, trimmed by 0.2 s, so
        // its beat at 0.9 s plays at 1.2 s of the film.
        QString error;
        QVERIFY(Sound::writeCues({}, 3, dir.filePath("track.wav"), &error));
        write(dir.filePath("track.wav.beats.json"), R"({ "tempo": 60, "beats": [0.9, 1.9] })");
        const QByteArray track = "    property string audio: \"track.wav\"\n"
                                 "    property real audioTrim: 0.2\n    property real audioDelay: 0.5\n";
        QVERIFY(anyMessage(problems({}, track), "Beat \"Two\" cuts at 1.00s, 2 frames before the music's beat at 1.20s."));
    }

    void renderMixesTheCuesAndTheTrackToOneLoudness()
    {
        if (QStandardPaths::findExecutable("ffmpeg").isEmpty())
            QSKIP("ffmpeg is not installed");
        QTemporaryDir dir;
        QProcess make;
        make.start("ffmpeg", {"-loglevel", "error", "-y", "-f", "lavfi", "-i", "sine=frequency=220:duration=4,volume=0.02",
                              dir.filePath("track.wav")});
        QVERIFY(make.waitForFinished() && make.exitCode() == 0);
        auto loudness = [&](const QString &video) {
            QProcess probe;
            probe.start("ffmpeg", {"-hide_banner", "-i", video, "-af", "ebur128", "-f", "null", "-"});
            probe.waitForFinished(-1);
            const QString log = QString::fromUtf8(probe.readAllStandardError());
            const auto match = QRegularExpression(R"(Integrated loudness:\s+I:\s+(-?[\d.]+) LUFS)").match(log);
            return match.hasMatch() ? match.captured(1).toDouble() : 0.0;
        };
        write(dir.filePath("s.qml"), sceneSource("    property string audio: \"track.wav\"\n"
                                                 "    property var cues: [{ at: 1, name: \"hit\", sound: \"hit\" }]\n"));
        Scene scene;
        QString error;
        QVERIFY(scene.load(dir.filePath("s.qml"), "16:9", &error));
        QVERIFY2(scene.render(dir.filePath("out.mp4"), &error), qPrintable(error));
        QProcess probe;
        probe.start("ffprobe", {"-v", "error", "-show_entries", "stream=codec_type", "-of", "csv=p=0", dir.filePath("out.mp4")});
        QVERIFY(probe.waitForFinished());
        QCOMPARE(QString::fromUtf8(probe.readAllStandardOutput()).split('\n').filter("audio").size(), 1);

        // A quiet track alone comes out at the feeds' loudness.
        write(dir.filePath("t.qml"), sceneSource("    property string audio: \"track.wav\"\n"));
        Scene quiet;
        QVERIFY(quiet.load(dir.filePath("t.qml"), "16:9", &error));
        QVERIFY(quiet.render(dir.filePath("quiet.mp4"), &error));
        const double lufs = loudness(dir.filePath("quiet.mp4"));
        QVERIFY2(std::abs(lufs + 14) < 1.5, qPrintable(QString::number(lufs)));
    }

    // The style guide's tokens reach the scene, and check holds it to them.
    // A font from this machine renders differently on the next one.
    void studioFilmsLoadTheirOwnFonts()
    {
        QTemporaryDir dir;
        QString error;
        QVERIFY(Studio::create(dir.filePath("film"), &error));
        QVERIFY(QFile::copy("/usr/share/fonts/liberation/LiberationMono-Regular.ttf", dir.filePath("film/assets/mono.ttf"))
                || !QFileInfo::exists("/usr/share/fonts/liberation/LiberationMono-Regular.ttf"));
        if (!QFileInfo::exists(dir.filePath("film/assets/mono.ttf")))
            QSKIP("Liberation Mono is not installed");
        write(dir.filePath("film/scene.qml"),
              sceneSource("    FontLoader { id: mono; source: \"assets/mono.ttf\" }\n"
                          "    Text { x: 300; y: 300; text: \"Ours\"; font.family: mono.name; font.pixelSize: 60 }\n"
                          "    Text { x: 300; y: 500; text: \"Theirs\"; font.family: \"DejaVu Sans\"; font.pixelSize: 60 }\n"));
        Scene scene;
        QVERIFY(scene.load(dir.filePath("film/scene.qml"), "16:9", &error));
        QStringList found;
        for (const auto &problem : scene.check())
            if (problem.message.contains("machine's fonts"))
                found << problem.message;
        QCOMPARE(found.size(), 1);
        QVERIFY(found[0].startsWith("Text \"Theirs\" is set in DejaVu Sans"));
    }

    void scenesUseTheStyleGuidesTokens()
    {
        QTemporaryDir dir;
        write(dir.filePath("style-guide.md"),
              "# Style guide\n\nPALETTE\ncolor ignored #000000\n\nTOKENS\ncolor accent #FFD60A\n"
              "font headline Adwaita Sans\nrule panel 120 22\nrule micro 300 24\nrule broken 0 3\n");
        const StyleTokens::Tokens tokens = StyleTokens::read(dir.filePath("style-guide.md"));
        QCOMPARE(tokens.colors, QVariantMap({{"accent", "#ffd60a"}}));
        QCOMPARE(tokens.fonts["headline"].toString(), QString("Adwaita Sans"));
        QCOMPARE(tokens.rules.keys(), QStringList({"micro", "panel"}));

        write(dir.filePath("s.qml"),
              sceneSource("    property real ruled: Motion.rule(\"panel\", t, 0.2)\n"
                          "    property real sprung: Motion.spring(t, 0.2, 120, 22)\n"
                          "    property var state: Motion.between([{ at: 0, x: 0, c: \"#000000\", label: \"a\" },"
                          " { at: 1, x: 100, c: \"#ffffff\", label: \"b\", over: 1 }], t)\n"
                          "    property int seed: 0\n"
                          "    property string format\n"
                          "    property real square: Motion.per(format, { \"1:1\": 56 }, 64)\n"
                          "    property var clear: Motion.safe(\"9:16\", 1080, 1920)\n"
                          "    Rectangle { objectName: \"card\"; width: 50; height: 50; color: Style.colors.accent }\n"
                          "    Rectangle { width: 10; height: 10; x: Motion.spring(parent.t, 0, 260, 20) }\n"
                          "    Rectangle { width: 10; height: 10; x: Motion.rule(\"bounce\", parent.t, 0) }\n"));
        Scene::setSeed(7);
        Scene scene;
        QString error;
        QVERIFY2(scene.load(dir.filePath("s.qml"), "16:9", &error), qPrintable(error));
        Scene::setSeed(0);
        QCOMPARE(scene.root()->property("seed").toInt(), 7);
        scene.seek(0.7);
        QCOMPARE(scene.root()->property("ruled").toDouble(), scene.root()->property("sprung").toDouble());
        QVERIFY(scene.root()->property("ruled").toDouble() > 0.3);
        scene.seek(0.5);
        const QVariantMap state = scene.root()->property("state").toMap();
        QCOMPARE(state["x"].toDouble(), 50.0);
        QCOMPARE(state["label"].toString(), QString("b"));
        QCOMPARE(state["c"].value<QColor>().name(), QString("#808080"));
        QCOMPARE(scene.root()->property("square").toDouble(), 64.0);
        QCOMPARE(scene.root()->property("clear").toMap()["y"].toDouble(), 1920 * 0.12);
        QCOMPARE(scene.frame(0).pixelColor(10, 10), QColor("#ffd60a"));

        const QList<Problem> problems = scene.check();
        QVERIFY(anyMessage(problems, "springs at 260/20, which matches no motion rule in style-guide.md"));
        QVERIFY(anyMessage(problems, "asks for the motion rule \"bounce\", which style-guide.md doesn't have"));
    }

    void cameraPansAndZoomsByItsKeys()
    {
        QTemporaryDir dir;
        // A red square at (1000, 600), brought to the middle of the frame and
        // doubled by 1 s; a white band at depth 0 stays where the frame is.
        write(dir.filePath("s.qml"),
              "import QtQuick\nimport Omotion\nRectangle {\n    id: scene\n    property real t\n"
              "    property real duration: 2\n    color: \"black\"\n    Camera {\n        id: camera\n"
              "        anchors.fill: parent\n        t: scene.t\n"
              "        keys: [{ at: 0 }, { at: 1, x: 1050, y: 650, zoom: 2, over: 1 }]\n"
              "        Rectangle { x: 1000; y: 600; width: 100; height: 100; color: \"red\" }\n"
              "        Item { readonly property var view: camera.plane(0); x: view.x; y: view.y; scale: view.scale;"
              " transformOrigin: Item.TopLeft\n"
              "            Rectangle { x: 0; y: 0; width: 1920; height: 40; color: \"white\" } }\n    }\n}\n");
        Scene scene;
        QString error;
        QVERIFY2(scene.load(dir.filePath("s.qml"), "16:9", &error), qPrintable(error));
        QCOMPARE(scene.frame(0).pixelColor(960, 540), QColor(Qt::black));
        const QImage closeUp = scene.frame(1.5);
        QCOMPARE(closeUp.pixelColor(960, 540), QColor(Qt::red));
        // Twice the size: the square's edge is 100 px from the middle.
        QCOMPARE(closeUp.pixelColor(960 + 95, 540), QColor(Qt::red));
        QCOMPARE(closeUp.pixelColor(960 + 105, 540), QColor(Qt::black));
        // The band at depth 0 doesn't pan: it stays at the top of the frame.
        QCOMPARE(scene.frame(0).pixelColor(960, 20), QColor(Qt::white));
        QCOMPARE(closeUp.pixelColor(960, 20), QColor(Qt::white));
    }

    // The v0.6 done-when: three reference films, with colors and shot
    // lengths known, measured within ΔE 5 and 20%; and a film made from what
    // was measured lands within the same tolerances.
    void referenceMeasuresPaletteAndPacing()
    {
        if (QStandardPaths::findExecutable("ffmpeg").isEmpty())
            QSKIP("ffmpeg is not installed");
        QTemporaryDir dir;
        // A film that cuts between backgrounds every shot seconds, with a
        // square of the last color drifting across, or (graphics) a headline
        // bar replaced on one ground.
        auto film = [&](const QString &name, const QStringList &colors, double shot, bool graphics) {
            QString shots;
            for (int i = 0; i < colors.size() - 1; ++i)
                shots += "\"" + colors[i] + "\",";
            QByteArray source =
                "import QtQuick\nimport Omotion\nRectangle {\n    property real t\n    property real duration: 9\n"
                "    property int fps: 10\n";
            if (graphics)
                source += QString("    color: \"%1\"\n    readonly property int shot: Math.floor(t / %2)\n"
                                  "    Rectangle { x: 200 + (parent.shot % 3) * 300; y: 300 + (parent.shot % 2) * 300;"
                                  " width: 700; height: 220; color: \"%3\"; opacity: 1 - Motion.progress(parent.t % %2,"
                                  " %2 - 0.2, %2) }\n}\n").arg(colors[0]).arg(shot).arg(colors[1]).toUtf8();
            else
                source += QString("    readonly property var grounds: [%1]\n"
                                  "    color: grounds[Math.floor(t / %2) % grounds.length]\n"
                                  "    Rectangle { x: 200 + parent.t * 120; y: 380; width: 360; height: 360; color: \"%3\" }\n}\n")
                              .arg(shots).arg(shot).arg(colors.last()).toUtf8();
            write(dir.filePath(name + ".qml"), source);
            Scene scene;
            QString error;
            if (!scene.load(dir.filePath(name + ".qml"), "16:9", &error) ||
                !scene.render(dir.filePath(name + ".mp4"), &error))
                qWarning().noquote() << error;
            Reference::Analysis analysis;
            if (!Reference::analyze({dir.filePath(name + ".mp4")}, &analysis, &error))
                qWarning().noquote() << error;
            return analysis;
        };
        auto within = [](const Reference::Analysis &analysis, const QStringList &colors, double shot) {
            for (const auto &color : colors) {
                double nearest = 1e9;
                for (const auto &swatch : analysis.palette)
                    nearest = qMin(nearest, Reference::deltaE(swatch.color, QColor(color)));
                if (nearest > 5) {
                    qWarning().noquote() << color << "is" << nearest << "from the nearest measured color";
                    return false;
                }
            }
            if (std::abs(analysis.shotLength - shot) > shot * 0.2) {
                qWarning().noquote() << "shot length" << analysis.shotLength << "against" << shot;
                return false;
            }
            return true;
        };
        const QList<QPair<QStringList, QPair<double, bool>>> references{
            {{"#1d3557", "#e63946", "#f1faee", "#a8dadc"}, {1.5, false}},
            {{"#0b0b0e", "#ffd60a"}, {2.25, true}},
            {{"#2a9d8f", "#264653", "#e9c46a"}, {3.0, false}},
        };
        for (int i = 0; i < references.size(); ++i) {
            const auto &[colors, how] = references[i];
            const Reference::Analysis measured = film(QString("ref%1").arg(i), colors, how.first, how.second);
            QVERIFY2(within(measured, colors, how.first), qPrintable(QString("reference %1").arg(i)));
            // A film made from the measured guide: its colors, its pacing.
            QStringList guide;
            for (const auto &swatch : measured.palette)
                guide << swatch.color.name();
            const Reference::Analysis made = film(QString("made%1").arg(i), guide, measured.shotLength, how.second);
            QVERIFY2(within(made, colors, how.first), qPrintable(QString("made from reference %1").arg(i)));
        }

        // The measured lines go into the guide in place of the template's.
        QString error;
        QVERIFY(Studio::create(dir.filePath("film"), &error));
        Reference::Analysis analysis;
        analysis.palette = {{QColor("#112233"), 0.8}};
        analysis.duration = 9;
        analysis.shotLength = 1.5;
        QVERIFY(Reference::writeInto(dir.filePath("film/style-guide.md"), analysis, &error));
        QFile guide(dir.filePath("film/style-guide.md"));
        QVERIFY(guide.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(guide.readAll());
        QVERIFY(text.contains("Palette (measured): #112233 80%"));
        QVERIFY(text.contains("Pacing (measured): a scene change every 1.5 s"));
        QVERIFY(!text.contains("Palette: actual colors"));
        QVERIFY(text.contains("TOKENS"));
    }

    // The v0.6 spike: a clip's frames, asked for in any order, are the
    // frames it has at those moments, exactly.
    void clipShowsTheFrameAtItsTime()
    {
        if (QStandardPaths::findExecutable("ffmpeg").isEmpty())
            QSKIP("ffmpeg is not installed");
        QTemporaryDir dir;
        QString error;
        QVERIFY(Studio::create(dir.filePath("film"), &error));
        const QDir film(dir.filePath("film"));
        // A clip whose frame n is gray 8n: 30 frames, a second at 30 fps.
        write(dir.filePath("counter.qml"), "import QtQuick\nRectangle {\n    property real t\n    property real duration: 1\n"
                                           "    property int fps: 30\n    color: Qt.rgba(Math.round(t * 30) * 8 / 255,"
                                           " Math.round(t * 30) * 8 / 255, Math.round(t * 30) * 8 / 255, 1)\n}\n");
        Scene counter;
        QVERIFY(counter.load(dir.filePath("counter.qml"), "16:9", &error));
        QVERIFY2(counter.render(film.filePath("assets/counter.mp4"), &error), qPrintable(error));
        write(film.filePath("assets/assets.md"), "| Asset | File | Source | Status |\n| --- | --- | --- | --- |\n"
                                                 "| Counter | counter.mp4 | made by the test | have |\n");

        write(film.filePath("scene.qml"),
              "import QtQuick\nimport Omotion\nRectangle {\n    id: scene\n    property real t\n    property real duration: 2\n"
              "    property int fps: 30\n    color: \"black\"\n"
              "    Clip { id: footage; video: \"assets/counter.mp4\"; time: scene.t - 0.5; fps: scene.fps;"
              " width: 1920; height: 1080 }\n}\n");
        Scene scene;
        QVERIFY2(scene.load(film.filePath("scene.qml"), "16:9", &error), qPrintable(error));
        QElapsedTimer timer;
        timer.start();
        QList<int> order(60);
        std::iota(order.begin(), order.end(), 0);
        std::mt19937 random(7);
        std::shuffle(order.begin(), order.end(), random);
        for (int f : order) {
            // The clip starts at 0.5 s: its first frame before, its last after.
            const int shown = qBound(0, f - 15, 29);
            const int gray = scene.frame(f / 30.0).pixelColor(960, 540).red();
            QVERIFY2(std::abs(gray - shown * 8) <= 3, qPrintable(QString("film frame %1 shows %2, not clip frame %3")
                                                                      .arg(f).arg(gray / 8.0).arg(shown)));
        }
        qInfo() << "60 clip frames, decoding included, in" << timer.elapsed() << "ms";
        // Decoded once, under the film's cache; the clip is the asset it loads.
        QCOMPARE(QDir(film.filePath("renders/.cache/clips")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 1);
        for (const auto &problem : scene.check())
            QVERIFY2(!problem.message.contains("assets"), qPrintable(problem.message));
    }

    void deliverHandsOverEverything()
    {
        if (QStandardPaths::findExecutable("ffmpeg").isEmpty())
            QSKIP("ffmpeg is not installed");
        QTemporaryDir dir;
        QString error;
        QVERIFY(Studio::create(dir.filePath("film"), &error));
        const QString scene = dir.filePath("film/scene.qml");
        write(scene, sceneSource());
        write(dir.filePath("film/notes.md"), "# Notes\n\n- [ ] 001 · 1.00s · 16:9 · Two · (10, 10)\n  note: bigger\n");
        QJsonObject result;
        QStringList steps;
        QVERIFY2(Deliver::write(scene, {"16:9", "9:16"}, {}, &result, &error, [&](const QString &s) { steps << s; }),
                 qPrintable(error));
        const QDir out(dir.filePath("film/deliverables"));
        for (const auto &file : {"videos/film-16x9.mp4", "videos/film-9x16.mp4", "posters/film-16x9.png",
                                 "posters/film-9x16.png", "sheets/film-16x9-sheet.png", "storyboard.pdf",
                                 "source/scene.qml", "source/brief.md", "assets.md", "REVIEW.md"})
            QVERIFY2(QFileInfo(out.filePath(file)).size() > 0, file);
        QVERIFY(!QFileInfo::exists(out.filePath("source/.claude")));
        QFile review(out.filePath("REVIEW.md"));
        QVERIFY(review.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(review.readAll());
        QVERIFY(text.contains("- Gate 1, References and assets"));
        QVERIFY(text.contains("- Note 001 at 1.00s (16:9) is open: bigger"));
        QVERIFY(text.contains("has no purpose"));
        QVERIFY(text.contains("the music's license"));
        QVERIFY(steps.contains("Storyboard"));
        // A second delivery encodes nothing again.
        Scene again;
        QVERIFY(again.load(scene, "16:9", &error));
        QVERIFY(again.render(Scene::defaultOutput(scene, "16:9"), &error));
        QCOMPARE(again.reusedFrames(), 20);
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

    // A patch to one beat encodes that beat again and keeps the rest.
    void renderEncodesOnlyTheBeatsThatChanged()
    {
        if (QStandardPaths::findExecutable("ffmpeg").isEmpty())
            QSKIP("ffmpeg is not installed");
        QTemporaryDir dir;
        // Beats at 0 and 1 s, ten frames each; the caption shows only in the second.
        auto source = [](const QByteArray &caption) {
            return sceneSource("    Text { x: 300; y: 300; font.pixelSize: 60; color: \"white\"; text: \"" + caption +
                               "\"; visible: parent.t >= 1 }\n    property var captions: [\"" + caption + "\"]\n");
        };
        write(dir.filePath("scene.qml"), source("First"));
        const QString output = dir.filePath("renders/scene-16x9.mp4");
        auto render = [&](bool fresh = false) {
            Scene scene;
            QString error;
            if (!scene.load(dir.filePath("scene.qml"), "16:9", &error) || !scene.render(output, &error, {}, fresh)) {
                qWarning().noquote() << error;
                return -1;
            }
            return scene.reusedFrames();
        };
        QCOMPARE(render(), 0);
        QCOMPARE(render(), 20);
        write(dir.filePath("scene.qml"), source("Second"));
        QCOMPARE(render(), 10);
        QCOMPARE(render(true), 0);

        // A file the scene loads may change without its name changing.
        write(dir.filePath("scene.qml"), source("Second") + "\n");
        QCOMPARE(render(), 20);
        write(dir.filePath("logo.svg"), "<svg/>");
        QCOMPARE(render(), 0);

        // What paints by itself is encoded every time.
        write(dir.filePath("scene.qml"), sceneSource("    AnimatedImage { source: \"logo.gif\"; playing: false }\n"));
        QCOMPARE(render(), 0);
        QCOMPARE(render(), 0);

        QProcess probe;
        probe.start("ffprobe", {"-v", "error", "-count_frames", "-select_streams", "v:0", "-show_entries",
                                "stream=nb_read_frames", "-of", "csv=p=0", output});
        QVERIFY(probe.waitForFinished());
        QCOMPARE(probe.readAllStandardOutput().trimmed(), QByteArray("20"));
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

    void backendNeverFillsAFolderWithWork()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("mine");
        write(dir.filePath("mine/notes.txt"), "keep me");
        FakePicker picker;
        Backend backend(&picker);
        QVERIFY(!backend.createFilm(dir.filePath("mine")));
        QVERIFY(backend.messageIsError());
        QCOMPARE(QDir(dir.filePath("mine")).entryList(QDir::Files), QStringList{"notes.txt"});
        QVERIFY(backend.createFilm(dir.filePath("launch")));
        QVERIFY(backend.load(dir.filePath("launch/scene.qml")));
        QVERIFY(!backend.load(dir.filePath("mine/notes.txt")));
    }

    void newFilmIsAWholeStudio()
    {
        QTemporaryDir dir;
        const QString root = dir.filePath("jotter-sync");
        QString error;
        QStringList created;
        QVERIFY2(Studio::create(root, &error, &created), qPrintable(error));
        for (const auto &file : {"brief.md", "style-guide.md", "shotlist.md", "assets/assets.md", "gates.md",
                                 "notes.md", "activity.md", "scene.qml", "AGENTS.md",
                                 ".claude/skills/omotion/SKILL.md"})
            QVERIFY2(QFileInfo(QDir(root).filePath(file)).isFile(), file);
        QVERIFY(QFileInfo(QDir(root).filePath("reviews")).isDir());
        QVERIFY(QFileInfo(QDir(root).filePath("renders")).isDir());
        QVERIFY(created.contains(".claude/skills/omotion/SKILL.md"));
        // A folder with anything in it is never filled with templates.
        QVERIFY(!Studio::create(root, &error));
        QVERIFY(error.contains("already has files"));

        // The studio is found from inside it, and the starter scene in it is clean.
        QDir(root).mkpath("reviews/old");
        QCOMPARE(Studio::rootOf(QDir(root).filePath("reviews/old")), QFileInfo(root).absoluteFilePath());
        QCOMPARE(Studio::rootOf(dir.path()), QString());
        Scene scene;
        QVERIFY(scene.load(QDir(root).filePath("scene.qml"), "16:9", &error));
        QCOMPARE(scene.check().size(), 0);
    }

    void skillInAStudioStaysCurrent()
    {
        QTemporaryDir dir;
        const QString root = dir.filePath("film");
        QString error;
        QVERIFY(Studio::create(root, &error));
        QCOMPARE(Studio::skillProblem(root), QString());
        const QString skill = QDir(root).filePath(".claude/skills/omotion/SKILL.md");

        // One written by another omotion is reported, and install replaces it.
        write(skill, "old skill\n<!-- omotion 0.0.1 skill 000000000000 -->\n");
        QVERIFY(Studio::skillProblem(root).contains("omotion 0.0.1"));
        Scene scene;
        QVERIFY(scene.load(QDir(root).filePath("scene.qml"), "16:9", &error));
        bool reported = false;
        for (const auto &problem : scene.check())
            reported = reported || problem.message.contains("agent skill");
        QVERIFY(reported);
        QVERIFY(Studio::installSkill(root, &error));
        QCOMPARE(Studio::skillProblem(root), QString());

        // Without the stamp it's the person's own, and left alone; AGENTS.md they wrote is kept too.
        write(skill, "my own skill\n");
        QCOMPARE(Studio::skillProblem(root), QString());
        write(QDir(root).filePath("AGENTS.md"), "mine");
        QStringList written;
        QVERIFY(Studio::installSkill(root, &error, &written));
        QCOMPARE(written, QStringList{".claude/skills/omotion/SKILL.md"});
        QFile agents(QDir(root).filePath("AGENTS.md"));
        QVERIFY(agents.open(QIODevice::ReadOnly));
        QCOMPARE(agents.readAll(), QByteArray("mine"));

        QVERIFY(QFile::remove(skill));
        QVERIFY(Studio::skillProblem(root).contains("no agent skill"));
    }

    void studioReadsItsGates()
    {
        QTemporaryDir dir;
        QString error;
        QVERIFY(Studio::create(dir.filePath("film"), &error));
        const QString root = dir.filePath("film");
        auto gates = Studio::readGates(root);
        QCOMPARE(gates.size(), 6);
        QCOMPARE(gates[1].title, QString("Style guide and shot list"));
        QCOMPARE(gates[1].status, QString("the person approves"));
        QVERIFY(!gates[1].passed);

        write(QDir(root).filePath("gates.md"),
              "- [x] 1 References and assets: approved by the person, 2026-10-23\n"
              "- [ ] 2 Style guide and shot list: waiting for the person's approval since 2026-10-23\n");
        gates = Studio::readGates(root);
        QVERIFY(gates[0].passed);
        QCOMPARE(gates[0].status, QString("approved by the person, 2026-10-23"));
        QVERIFY(gates[1].waitingForPerson());
        QVERIFY(!gates[0].waitingForPerson());
    }

    void studioSaysWhatEachGateLacks()
    {
        QTemporaryDir dir;
        QString error;
        QVERIFY(Studio::create(dir.filePath("film"), &error));
        const QString root = dir.filePath("film");
        const QDir film(root);
        QCOMPARE(Studio::missingEvidence(root, 1, {}),
                 QStringList({"brief.md is still the template", "assets/assets.md is still the template"}));
        QCOMPARE(Studio::missingEvidence(root, 3, {}),
                 QStringList({"scene.qml is still the template", "no stills in reviews/"}));

        write(film.filePath("brief.md"), "# Brief\nfilled\n");
        write(film.filePath("assets/assets.md"), "| Asset | File | Source | Status |\n| --- | --- | --- | --- |\n"
                                                 "| Logo | | | missing |\n");
        QVERIFY(Studio::missingEvidence(root, 1, {}).isEmpty());

        // The person's note frames aren't the agent's stills.
        film.mkpath("reviews/notes");
        write(film.filePath("reviews/notes/001.png"), "png");
        QCOMPARE(Studio::missingEvidence(root, 3, {}).last(), QString("no stills in reviews/"));
        write(film.filePath("reviews/scene-16x9-1.00.png"), "png");
        QCOMPARE(Studio::missingEvidence(root, 4, {}), QStringList{"no contact sheet in reviews/"});
        film.mkpath("reviews/round2");
        write(film.filePath("reviews/round2/scene-16x9-sheet.png"), "png");
        QVERIFY(Studio::missingEvidence(root, 4, {}).isEmpty());
        write(film.filePath("reviews/001.md"), "# Review\n");
        QVERIFY(Studio::missingEvidence(root, 5, {}).isEmpty());

        // Gate 6 wants a render per format, made after the scene's last change.
        write(film.filePath("renders/scene-16x9.mp4"), "mp4");
        QFile video(film.filePath("renders/scene-16x9.mp4"));
        QVERIFY(video.open(QIODevice::ReadWrite));
        video.setFileTime(QFileInfo(film.filePath("scene.qml")).lastModified().addSecs(-60),
                          QFileDevice::FileModificationTime);
        video.close();
        QCOMPARE(Studio::missingEvidence(root, 6, {"16:9", "9:16"}),
                 QStringList({"renders/scene-16x9.mp4 is older than scene.qml", "renders/scene-9x16.mp4 is missing"}));
    }

    // Found in film 2: the agent quietly built a beat differently from the
    // shot the person approved, and nothing noticed.
    void sceneAnswersToTheShotList()
    {
        QTemporaryDir dir;
        QString error;
        QVERIFY(Studio::create(dir.filePath("film"), &error));
        const QString root = dir.filePath("film");
        write(QDir(root).filePath("shotlist.md"),
              "# Shot list\n\n00.0-01.0s   ONE: \"a subtitle\"\n"
              "  enters: an empty frame\n  action: ignored\n  exits: the slide,\n    full frame.\n  why: the hook\n\n"
              "01.0-02.0s   TWO (the poster)\n  enters: the slide\n  exits: the end card\n\nFORMATS\n16:9: wide\n");
        const auto shots = Studio::readShotList(root);
        QCOMPARE(shots.size(), 2);
        QCOMPARE(shots[0].name, QString("ONE"));
        QCOMPARE(shots[0].exits, QString("the slide, full frame."));
        QCOMPARE(shots[0].why, QString("the hook"));
        QCOMPARE(shots[1].start, 1.0);
        QCOMPARE(shots[1].end, 2.0);

        auto problems = [&](const QByteArray &beats) {
            write(QDir(root).filePath("scene.qml"), sceneSource({}, "    property var beats: [" + beats + "]\n")
                                                      .replace("    property var beats: [{ at: 0, name: \"One\" }, "
                                                               "{ at: 1, name: \"Two\" }]\n", ""));
            Scene scene;
            QString error;
            if (!scene.load(QDir(root).filePath("scene.qml"), "16:9", &error))
                return QStringList{error};
            QStringList found;
            for (const auto &problem : scene.check())
                if (!problem.message.contains("skill"))
                    found << QString(problem.error ? "error: " : "warning: ") + problem.message;
            return found;
        };
        const QByteArray one = R"({ at: 0, name: "One", purpose: "hook", enter: "An empty frame.", exit: "the slide, full frame" })";
        QCOMPARE(problems(one + R"(, { at: 1, name: "Two", purpose: "close", enter: "the slide", exit: "the end card" })"),
                 QStringList());
        const QStringList drift = problems(
            one + R"(, { at: 1.4, name: "Close", purpose: "close", enter: "the slide", exit: "the slide, scaled down" })");
        QCOMPARE(drift.size(), 3);
        QVERIFY2(drift[0].startsWith("error: Beat \"Close\" starts at 1.40s, but shotlist.md starts TWO at 1.00s"),
                 qPrintable(drift[0]));
        QVERIFY(drift[1].startsWith("warning: Beat \"Close\" is \"TWO\" in shotlist.md"));
        QVERIFY(drift[2].startsWith("warning: Beat \"Close\"'s exit state differs from shotlist.md's \"the end card\""));
        QVERIFY(problems(one)[0].startsWith("error: The scene has 1 beats and shotlist.md has 2 (ONE, TWO)"));
        QVERIFY(problems(R"({ at: 0, name: "One" }, { at: 1, name: "Two", purpose: "close" })")[0]
                    .startsWith("error: Beat \"One\" has no purpose"));
    }

    // A film shows only files the person approved, each with its source.
    void sceneLoadsOnlyListedAssets()
    {
        QTemporaryDir dir;
        QString error;
        QVERIFY(Studio::create(dir.filePath("film"), &error));
        const QDir film(dir.filePath("film"));
        film.mkpath("assets/screens");
        for (const auto &name : {"assets/logo.png", "assets/screens/a.png", "assets/extra.png", "outside.png"})
            QVERIFY(QImage(4, 4, QImage::Format_RGB32).save(film.filePath(name)));
        write(film.filePath("assets/assets.md"),
              "| Asset | File | Source | Status |\n| --- | --- | --- | --- |\n"
              "| Logo | `logo.png` | the person | have |\n"
              "| Screens | screens/*.png | the app | have |\n"
              "| Deck | deck.png + gone.png | the app | have |\n"
              "| Music | — | the person | missing |\n");
        QCOMPARE(Studio::listedAssetFiles(film.path()), QStringList({"logo.png", "screens/*.png", "deck.png", "gone.png"}));
        QVERIFY(Studio::isListedAsset(film.path(), "screens/a.png"));
        QVERIFY(!Studio::isListedAsset(film.path(), "screens/deep/a.png"));
        QCOMPARE(Studio::listedAssetsNotFound(film.path()), QStringList({"deck.png", "gone.png"}));
        QCOMPARE(Studio::missingEvidence(film.path(), 1, {}).mid(1),
                 QStringList({"assets.md lists deck.png, which isn't in assets/",
                              "assets.md lists gone.png, which isn't in assets/"}));

        write(film.filePath("scene.qml"),
              sceneSource("    Image { source: \"assets/logo.png\" }\n"
                          "    Repeater { model: 1; Image { source: \"assets/screens/a.png\" } }\n"
                          "    Image { source: parent.t > 1 ? \"assets/extra.png\" : \"\" }\n"
                          "    Image { source: \"outside.png\" }\n"
                          "    Image { source: \"https://example.com/x.png\" }\n"));
        Scene scene;
        QVERIFY2(scene.load(film.filePath("scene.qml"), "16:9", &error), qPrintable(error));
        QStringList found;
        for (const auto &problem : scene.check())
            if (problem.message.contains("assets"))
                found << QString(problem.error ? "error: " : "warning: ") + problem.message;
        found.sort();
        QCOMPARE(found, QStringList({
            "error: Image loads https://example.com/x.png from the network; download it into assets/ and list it in "
            "assets/assets.md with where it came from.",
            "error: Image loads outside.png from outside assets/; copy it into assets/ and list it in "
            "assets/assets.md with where it came from.",
            "error: assets/assets.md lists deck.png, which isn't in assets/. Add the file, or mark the asset missing "
            "and ask the person for it.",
            "error: assets/assets.md lists gone.png, which isn't in assets/. Add the file, or mark the asset missing "
            "and ask the person for it.",
            "warning: assets/extra.png isn't listed in assets/assets.md; add it with where it came from.",
        }));
    }

    void notesRoundTripWithTheAgent()
    {
        QTemporaryDir dir;
        QString error;
        QVERIFY(Studio::create(dir.filePath("film"), &error));
        const QString root = dir.filePath("film");
        QCOMPARE(Studio::readNotes(root).size(), 0);

        Studio::Note note;
        note.at = 12.6;
        note.format = "16:9";
        note.beat = "Gates";
        note.point = {1152, 446};
        note.target = "you approve";
        note.text = "these marks\nleave too soon";
        note.evidence = "ignored: the id decides it";
        QCOMPARE(Studio::addNote(root, note, &error), QString("001"));
        note.text = "second";
        note.target.clear();
        QCOMPARE(Studio::addNote(root, note, &error), QString("002"));

        // The agent ticks the first with its proof, as the skill says.
        QFile file(QDir(root).filePath("notes.md"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QString text = QString::fromUtf8(file.readAll());
        file.close();
        QVERIFY(text.contains("- [ ] 001 · 12.60s · 16:9 · Gates · (1152, 446) · on \"you approve\""));
        text.replace("- [ ] 001", "- [x] 001");
        text.replace("  note: these marks leave too soon\n",
                     "  note: these marks leave too soon\n  fixed: held to 13.05 s; proof\n    reviews/fix.png\n");
        write(QDir(root).filePath("notes.md"), text.toUtf8());

        const auto notes = Studio::readNotes(root);
        QCOMPARE(notes.size(), 2);
        QVERIFY(notes[0].done);
        QCOMPARE(notes[0].text, QString("these marks leave too soon"));
        QCOMPARE(notes[0].point, QPoint(1152, 446));
        QCOMPARE(notes[0].answers, QStringList{"fixed: held to 13.05 s; proof reviews/fix.png"});
        QCOMPARE(notes[0].evidence, QString("reviews/notes/001.png"));
        QVERIFY(!notes[1].done);
        QCOMPARE(notes[1].target, QString());
        QCOMPARE(notes[1].evidence, QString("reviews/notes/002.png"));
        QVERIFY(Studio::noteReference(notes[0]).startsWith("Note 001 in notes.md (12.60s in 16:9, beat Gates"));
        // Adding another keeps the agent's edits.
        QCOMPARE(Studio::addNote(root, note, &error), QString("003"));
        QVERIFY(Studio::readNotes(root)[0].done);

        // A beat or target holding the heading's separator still reads back, so
        // no note goes missing and no id is used twice.
        note.beat = "Intro · Logo";
        note.target = "a · b";
        note.at = 1 / 30.0;
        QCOMPARE(Studio::addNote(root, note, &error), QString("004"));
        const auto all = Studio::readNotes(root);
        QCOMPARE(all.size(), 4);
        QCOMPARE(all[3].beat, QString("Intro - Logo"));
        QCOMPARE(all[3].target, QString("a - b"));
        // Frame-precise, so the note and its evidence are the same frame.
        QCOMPARE(std::lround(all[3].at * 30), 1L);
        QCOMPARE(Studio::noteTime(12.6), QString("12.60"));
        QCOMPARE(Studio::noteTime(1 / 30.0), QString("0.033"));
        QCOMPARE(Studio::addNote(root, note, &error), QString("005"));
    }

    void studioListsMissingAssetsAndActivity()
    {
        QTemporaryDir dir;
        QString error;
        QVERIFY(Studio::create(dir.filePath("film"), &error));
        const QString root = dir.filePath("film");
        write(QDir(root).filePath("assets/assets.md"),
              "| Asset | File | Source | Status |\n| --- | --- | --- | --- |\n"
              "| Logo | - | - | MISSING: requested |\n| Icon | icon.svg | given | have |\n");
        QCOMPARE(Studio::missingAssets(root), QStringList{"Logo"});
        write(QDir(root).filePath("activity.md"), "# Activity\n\n- 03:06 first\n- 03:14 second\n");
        QCOMPARE(Studio::readActivity(root), QStringList({"03:14 second", "03:06 first"}));
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
