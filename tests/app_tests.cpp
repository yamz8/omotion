// The whole app, driven the way a person and an agent use it: the real
// Main.qml offscreen, asserted on what's on screen, and the built omotion
// binary run as a command, asserted on the files it writes. Each test names
// the bug it guards against where there was one.
//
// Window tests need nothing else. CLI tests and evidence frames need the
// built binary in OMOTION_BINARY, which bin/test sets; without it they skip.
// OMOTION_SCREENS=<dir> makes the screens() case write the screenshot set
// that bin/screens produces for looking at.

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

#include "backend.h"
#include "engine.h"
#include "filepicker.h"
#include "scene.h"
#include "sound.h"
#include "studio.h"
#include "theme.h"
#include "translator.h"

namespace {
class FakePicker : public FilePicker {
public:
    void openScene(const QUrl &) override {}
    void newFilm(const QUrl &) override {}
};

// Returns whether it worked, so the caller fails where the cause is.
[[nodiscard]] bool write(const QString &path, const QByteArray &contents) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

QString binary() { return qEnvironmentVariable("OMOTION_BINARY"); }

// A scene small enough to render in a moment.
const QByteArray tinyScene =
    "import QtQuick\nimport Omotion\nRectangle {\n"
    "    property real t\n    property real duration: 1\n    property int fps: 10\n"
    "    property var formats: [\"16:9\", \"1:1\"]\n"
    "    property var beats: [{ at: 0, name: \"One\" }, { at: 0.5, name: \"Two\" }]\n"
    "    color: \"black\"\n"
    "    Rectangle { width: 200; height: 200; color: \"white\"; x: Motion.progress(parent.t, 0, 1) * 600; y: 300 }\n"
    "}\n";

// The app as main.cpp assembles it, around a temporary studio, with the
// default accent and a language of the test's choosing.
class App {
public:
    explicit App(const QString &language = "en") {
        ready = write(dir.filePath("language"), language.toUtf8() + "\n");
        translator = std::make_unique<Translator>(dir.filePath("language"));
        QCoreApplication::installTranslator(translator.get());
        theme = std::make_unique<Theme>(dir.filePath("no-omarchy"));
        backend = std::make_unique<Backend>(&picker, binary());
    }
    ~App() {
        engine.reset();
        QCoreApplication::removeTranslator(translator.get());
    }

    QString studio(const QString &name = "film") {
        QString error;
        const QString root = dir.filePath(name);
        if (!Studio::create(root, &error))
            qWarning() << error;
        return root;
    }

    // Opens the scene the way main.cpp does: loaded before the window exists,
    // and the engine wired by the app's own setUpEngine. With no scene, the
    // window opens on its welcome.
    bool open(const QString &scene, const QSize &size, double at = 0) {
        if (!ready)
            return false;
        if (!scene.isEmpty() && !backend->load(scene))
            return false;
        engine = std::make_unique<QQmlApplicationEngine>();
        setUpEngine(*engine, *backend, *translator, *theme, {at, {}, false});
        engine->load(QUrl("qrc:/Main.qml"));
        if (engine->rootObjects().isEmpty())
            return false;
        window = qobject_cast<QQuickWindow *>(engine->rootObjects().first());
        if (!window)
            return false;
        window->resize(size);
        if (scene.isEmpty())
            return QTest::qWaitFor([this] { return window->isVisible(); }, 5000);
        return QTest::qWaitFor([this] { return window->property("ready").toBool(); }, 5000);
    }

    // Searches the visual tree, which reaches list delegates that findChild
    // can't, since a view isn't their QObject parent.
    QQuickItem *item(const char *name) const { return find(window->contentItem(), QString::fromLatin1(name)); }
    static QQuickItem *find(QQuickItem *from, const QString &name) {
        if (from->objectName() == name)
            return from;
        for (QQuickItem *child : from->childItems())
            if (QQuickItem *found = find(child, name))
                return found;
        return nullptr;
    }
    QObject *object(const char *name) const { return window->findChild<QObject *>(name); }
    // Whether any item of that name is on screen, where the window keeps
    // more than one, such as a timeline for each transport.
    bool anyShown(const char *name) const { return findShown(window->contentItem(), QString::fromLatin1(name)); }
    static bool findShown(QQuickItem *from, const QString &name) {
        if (from->objectName() == name && from->isVisible() && from->width() > 0)
            return true;
        for (QQuickItem *child : from->childItems())
            if (findShown(child, name))
                return true;
        return false;
    }
    bool shown(const char *name) const {
        QQuickItem *found = item(name);
        return found && found->isVisible() && found->width() > 0;
    }
    void key(int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier, const QString &text = {}) {
        QKeyEvent press(QEvent::KeyPress, key, modifiers, text), release(QEvent::KeyRelease, key, modifiers, text);
        QCoreApplication::sendEvent(window, &press);
        QCoreApplication::sendEvent(window, &release);
        QTest::qWait(30);
    }
    void settle() { QTest::qWait(150); }

    QTemporaryDir dir;
    FakePicker picker;
    std::unique_ptr<Translator> translator;
    std::unique_ptr<Theme> theme;
    std::unique_ptr<Backend> backend;
    std::unique_ptr<QQmlApplicationEngine> engine;
    QQuickWindow *window = nullptr;
    bool ready = false;
};

struct Run {
    int code = -1;
    QByteArray out, err;
};

Run run(const QStringList &arguments, const QString &folder) {
    QProcess process;
    process.setWorkingDirectory(folder);
    process.start(binary(), arguments);
    // A command that never starts or doesn't finish in time is a failure, not
    // whatever exit code it hasn't produced yet.
    if (!process.waitForFinished(120000)) {
        process.kill();
        process.waitForFinished();
        return {-1, process.readAllStandardOutput(), "did not finish: " + process.errorString().toUtf8()};
    }
    return {process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1,
            process.readAllStandardOutput(), process.readAllStandardError()};
}

QStringList filesIn(const QString &folder) {
    QStringList files;
    QDirIterator it(folder, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        files << QDir(folder).relativeFilePath(it.next());
    files.sort();
    return files;
}
}

class AppTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QQuickStyle::setStyle("Material");
        registerSceneTypes();
    }

    // --- The window -------------------------------------------------------

    void wideWindowShowsThreeColumns() {
        App app;
        QVERIFY(app.open(QDir(app.studio()).filePath("scene.qml"), {1480, 900}));
        app.settle();
        QVERIFY(app.shown("studioPanel"));
        QVERIFY(app.shown("notesPanel"));
        QVERIFY(app.shown("preview"));
        QVERIFY(!app.shown("viewTabs"));
    }

    // Found on the desktop: Omarchy tiled the window to 621 px and both panels vanished.
    void tiledHalfScreenTakesTurnsAsTabs() {
        App app;
        QVERIFY(app.open(QDir(app.studio()).filePath("scene.qml"), {621, 670}));
        app.settle();
        QVERIFY(app.shown("viewTabs"));
        QVERIFY(app.shown("preview"));
        QVERIFY(!app.shown("studioPanel"));
        QVERIFY(!app.shown("notesPanel"));

        app.key(Qt::Key_P, Qt::NoModifier, "p");
        QTRY_VERIFY(app.shown("studioPanel"));
        QVERIFY(!app.shown("preview"));
        app.key(Qt::Key_P, Qt::NoModifier, "p");
        QTRY_VERIFY(app.shown("notesPanel"));
        QVERIFY(!app.shown("studioPanel"));
        app.key(Qt::Key_P, Qt::NoModifier, "p");
        QTRY_VERIFY(app.shown("preview"));
    }

    // Found on the desktop: at 621 px the timeline was squeezed to 70 px and lost its beats.
    void narrowTimelineKeepsItsBeatsReadable() {
        App app;
        QVERIFY(app.open(QDir(app.studio()).filePath("scene.qml"), {621, 670}));
        app.settle();
        QList<QQuickItem *> shown;
        for (QQuickItem *timeline : app.window->findChildren<QQuickItem *>("timeline"))
            if (timeline->isVisible())
                shown << timeline;
        QCOMPARE(shown.size(), 1);
        QVERIFY2(shown.first()->width() > app.window->width() * 0.8, qPrintable(QString::number(shown.first()->width())));
    }

    void questionMarkShowsTheShortcuts() {
        App app;
        QVERIFY(app.open(QDir(app.studio()).filePath("scene.qml"), {1480, 900}));
        QVERIFY(!app.shown("help"));
        app.key(Qt::Key_Question, Qt::ShiftModifier, "?");
        QTRY_VERIFY(app.shown("help"));
        app.key(Qt::Key_Escape);
        QTRY_VERIFY(!app.shown("help"));
    }

    // The loop: a click on the film, typed text, and Enter make a note in
    // notes.md, at that moment, with a ringed frame as evidence. Typing "q"
    // must not quit the app while the note is being written.
    void clickingTheFilmPinsANote() {
        App app;
        const QString root = app.studio();
        QVERIFY(app.open(QDir(root).filePath("scene.qml"), {1480, 900}, 6.0));
        app.settle();
        QSignalSpy quits(app.engine.get(), &QQmlEngine::quit);

        QQuickItem *preview = app.item("preview");
        QTest::mouseClick(app.window, Qt::LeftButton, Qt::NoModifier,
                          preview->mapToScene(QPointF(preview->width() / 2, preview->height() / 2)).toPoint());
        QTRY_VERIFY(app.object("noteEditor")->property("opened").toBool());
        for (const QChar c : QString("quiet here"))
            app.key(0, Qt::NoModifier, QString(c));
        app.key(Qt::Key_Return);

        QTRY_COMPARE(Studio::readNotes(root).size(), 1);
        const auto note = Studio::readNotes(root).first();
        QCOMPARE(note.text, QString("quiet here"));
        QCOMPARE(note.format, QString("16:9"));
        QCOMPARE(note.beat, QString("Review"));
        QVERIFY(qAbs(note.at - 6.0) < 1 / 30.0);
        QVERIFY(qAbs(note.point.x() - 960) < 4 && qAbs(note.point.y() - 540) < 4);
        QCOMPARE(quits.count(), 0);
        QVERIFY(!app.object("noteEditor")->property("opened").toBool());
    }

    // The note's evidence is the frame rendered by the built omotion, ringed
    // where the person clicked, and the card shows it once it's written.
    void pinnedNoteGetsItsEvidenceFrame() {
        if (binary().isEmpty())
            QSKIP("Set OMOTION_BINARY to render evidence frames.");
        App app;
        const QString root = app.studio();
        QVERIFY(app.open(QDir(root).filePath("scene.qml"), {1480, 900}, 6.0));
        app.settle();
        QQuickItem *preview = app.item("preview");
        QTest::mouseClick(app.window, Qt::LeftButton, Qt::NoModifier,
                          preview->mapToScene(QPointF(preview->width() / 2, preview->height() / 2)).toPoint());
        QTRY_VERIFY(app.object("noteEditor")->property("opened").toBool());
        for (const QChar c : QString("ring"))
            app.key(0, Qt::NoModifier, QString(c));
        app.key(Qt::Key_Return);
        QTRY_COMPARE(Studio::readNotes(root).size(), 1);
        const QString evidence = QDir(root).filePath(Studio::readNotes(root).first().evidence);
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(evidence).size() > 0, 20000);
        const QImage frame(evidence);
        QCOMPARE(frame.size(), QSize(1920, 1080));
        const QColor ring = frame.pixelColor(960 + 81, 540);
        QVERIFY2(ring.red() > 200 && ring.green() < 100, "The point the person clicked is ringed.");
        QTRY_VERIFY(!app.backend->notes().first().toMap()["evidenceUrl"].toUrl().isEmpty());
        QVERIFY(!app.backend->messageIsError());
    }

    // Found by review: the harness didn't wire live retranslation the way
    // main.cpp does, so a break there passed. Switching the Omarchy language
    // must change the window's words without a restart.
    void languageChangesLive() {
        App app;
        QVERIFY(app.open(QDir(app.studio()).filePath("scene.qml"), {1480, 900}));
        QCOMPARE(app.window->property("title").toString().endsWith("omotion"), true);
        QQuickItem *panel = app.item("notesPanel");
        auto heading = [&] {
            for (QQuickItem *label : panel->findChildren<QQuickItem *>())
                if (label->property("text").toString().startsWith("Not"))
                    return label->property("text").toString();
            return QString();
        };
        QCOMPARE(heading(), QString("Notes"));
        QVERIFY(write(app.dir.filePath("language"), "da\n"));
        QTRY_COMPARE(heading(), QString("Noter"));
    }

    // A scene's runtime errors reach the window, where its author is looking.
    // The window plays what the render will: the track, trimmed and
    // delayed, and the cues' sounds, which show on the timeline.
    void windowPlaysTheTrackAndCuesAsTheRenderWill() {
        App app;
        const QString track = app.dir.filePath("track.wav");
        QString error;
        QVERIFY(Sound::writeCues({{0.5, "hit"}, {1.5, "hit"}}, 4, track, &error));
        const QString scene = app.dir.filePath("s.qml");
        QVERIFY(write(scene, QByteArray(tinyScene).replace(
                                 "color: \"black\"\n",
                                 "color: \"black\"\n    property string audio: \"track.wav\"\n"
                                 "    property real audioTrim: 0.5\n    property real audioDelay: 0.25\n"
                                 "    property var cues: [{ at: 0.5, name: \"pop\", sound: \"click\" }]\n")));
        QVERIFY(app.open(scene, {1280, 820}));
        QObject *window = app.window;
        // The track's second 0.5 plays at 0.25 s of the film.
        QVariant position;
        QVERIFY(QMetaObject::invokeMethod(window, "trackPosition", Q_RETURN_ARG(QVariant, position),
                                          Q_ARG(QVariant, 0.25)));
        QCOMPARE(position.toDouble(), 500.0);
        QVERIFY(QMetaObject::invokeMethod(window, "trackPosition", Q_RETURN_ARG(QVariant, position),
                                          Q_ARG(QVariant, 0.0)));
        QVERIFY(position.toDouble() < 0);

        const QUrl cues = window->property("cueTrack").toUrl();
        QVERIFY(QFileInfo(cues.toLocalFile()).size() > 44);
        QCOMPARE(app.object("cuePlayer")->property("source").toUrl(), cues);
        QCOMPARE(app.object("trackPlayer")->property("source").toUrl(), QUrl::fromLocalFile(track));
        QVERIFY(app.anyShown("cue"));
        // The hit at 0.5 s of the track, a quarter second into the film.
        const QVariantList waveform = window->property("waveform").toList();
        QCOMPARE(waveform.size(), 240);
        QCOMPARE(waveform[int(0.25 / 1.0 * 240)].toDouble(), 1.0);
        QCOMPARE(waveform[5].toDouble(), 0.0);

        // Playing starts both, from where the film is.
        app.key(Qt::Key_Space, Qt::NoModifier, " ");
        QTRY_COMPARE(app.object("cuePlayer")->property("playbackState").toInt(), 1);
        app.key(Qt::Key_Space, Qt::NoModifier, " ");
        QTRY_VERIFY(app.object("cuePlayer")->property("playbackState").toInt() != 1);
    }

    void sceneErrorsShowInTheWindow() {
        App app;
        const QString scene = app.dir.filePath("broken.qml");
        QVERIFY(write(scene, QByteArray(tinyScene).replace("color: \"black\"\n",
                                                           "color: \"black\"\n    opacity: missing.value\n")));
        QVERIFY(app.open(scene, {1280, 820}));
        QTRY_VERIFY(app.backend->warning().contains("missing"));
    }

    // Found on the desktop: the agent's reply sat under a full-width evidence
    // frame, below the part of the card anyone could see.
    void agentAnswerShowsWithoutScrolling() {
        App app;
        const QString root = app.studio();
        QImage evidence(1920, 1080, QImage::Format_RGB32);
        evidence.fill(Qt::darkGray);
        QDir(root).mkpath("reviews/notes");
        QVERIFY(evidence.save(QDir(root).filePath("reviews/notes/001.png")));
        QFile notes(QDir(root).filePath("notes.md"));
        QVERIFY(notes.open(QIODevice::Append));
        notes.write("\n- [ ] 001 · 0.10s · 16:9 · Hook · (960, 540)\n  evidence: reviews/notes/001.png\n"
                    "  note: Is the black opening intentional? Reply under this note.\n"
                    "  reply: Half yes. The approved shot list opens on an empty dark frame, but its length falls "
                    "out of the first word's timing at 0.15 s and its fade-in; frames 0-4 are empty.\n");
        notes.close();

        // The size the window had on the desktop when the reply went missing.
        QVERIFY(app.open(QDir(root).filePath("scene.qml"), {1256, 670}));
        QTRY_VERIFY(app.item("noteAnswer"));
        app.settle();
        QQuickItem *answer = app.item("noteAnswer");
        QQuickItem *list = app.item("noteList");
        const QRectF placed = answer->mapRectToItem(list, QRectF(0, 0, answer->width(), answer->height()));
        QVERIFY2(placed.bottom() <= list->height(),
                 qPrintable(QString("answer ends at %1, list shows %2").arg(placed.bottom()).arg(list->height())));
    }

    // Found by review: picking a note in the panel cut the panel off from the window's selection.
    void panelAndWindowShareTheSelectedNote() {
        App app;
        const QString root = app.studio();
        for (const auto &text : {"first", "second"}) {
            Studio::Note note;
            note.at = 1;
            note.format = "16:9";
            note.text = text;
            QString error;
            QVERIFY(!Studio::addNote(root, note, &error).isEmpty());
        }
        QVERIFY(app.open(QDir(root).filePath("scene.qml"), {1480, 900}));
        QObject *panel = app.object("notesPanel");
        QMetaObject::invokeMethod(panel, "noteSelected", Q_ARG(QString, "001"));
        QTRY_COMPARE(app.window->property("selectedNote").toString(), QString("001"));
        QCOMPARE(panel->property("selected").toString(), QString("001"));
        QMetaObject::invokeMethod(app.window, "selectNote", Q_ARG(QVariant, "002"));
        QTRY_COMPARE(panel->property("selected").toString(), QString("002"));
    }

    // Found by review: opening another scene resized a window the person had sized.
    void openingAnotherSceneKeepsTheWindowSize() {
        App app;
        QVERIFY(app.open(QDir(app.studio()).filePath("scene.qml"), {1480, 900}));
        app.window->resize(1000, 700);
        const QString other = app.dir.filePath("plain.qml");
        QVERIFY(write(other, tinyScene));
        QVERIFY(app.backend->load(other));
        QTRY_VERIFY(app.window->property("ready").toBool());
        app.settle();
        QCOMPARE(app.window->size(), QSize(1000, 700));
    }

    void rightToLeftMirrorsThePanels() {
        App app("ar");
        QVERIFY(app.open(QDir(app.studio()).filePath("scene.qml"), {1480, 900}));
        app.settle();
        const double notes = app.item("notesPanel")->mapToScene(QPointF(0, 0)).x();
        const double studio = app.item("studioPanel")->mapToScene(QPointF(0, 0)).x();
        QVERIFY2(notes < studio, "In Arabic the notes belong on the left and the studio on the right.");
    }

    // --- The command line ---------------------------------------------------

    void cliNewMakesAStudioAndNeverRefills() {
        if (binary().isEmpty())
            QSKIP("Set OMOTION_BINARY to run the CLI tests.");
        QTemporaryDir dir;
        QCOMPARE(run({"new", "launch"}, dir.path()).code, 0);
        for (const auto &file : {"brief.md", "gates.md", "notes.md", "activity.md", "scene.qml", "AGENTS.md",
                                 ".claude/skills/omotion/SKILL.md"})
            QVERIFY2(QFileInfo(QDir(dir.path()).filePath(QString("launch/") + file)).isFile(), file);
        const Run again = run({"new", "launch"}, dir.path());
        QCOMPARE(again.code, 1);
        QVERIFY(again.err.contains("already has files"));
        QCOMPARE(run({"check", "launch/scene.qml"}, dir.path()).code, 0);
    }

    // Found by an agent: a file name given for several formats was ignored, and
    // the default names were written over earlier contact sheets.
    void cliKeepsAGivenNameAcrossFormats() {
        if (binary().isEmpty())
            QSKIP("Set OMOTION_BINARY to run the CLI tests.");
        QTemporaryDir dir;
        QVERIFY(write(dir.filePath("s.qml"), tinyScene));
        QCOMPARE(run({"sheet", "s.qml", "--format", "all", "-o", "proof.png"}, dir.path()).code, 0);
        QCOMPARE(filesIn(dir.path()), QStringList({"proof-16x9.png", "proof-1x1.png", "s.qml"}));
    }

    // Found by review: "-o out" with several formats became a file ffmpeg couldn't write.
    void cliPathWithoutAnExtensionIsAFolder() {
        if (binary().isEmpty())
            QSKIP("Set OMOTION_BINARY to run the CLI tests.");
        QTemporaryDir dir;
        QVERIFY(write(dir.filePath("s.qml"), tinyScene));
        QCOMPARE(run({"render", "s.qml", "--format", "all", "-o", "clips"}, dir.path()).code, 0);
        QCOMPARE(run({"frame", "s.qml", "--at", "0.5", "-o", "stills"}, dir.path()).code, 0);
        QCOMPARE(filesIn(dir.path()),
                 QStringList({"clips/s-16x9.mp4", "clips/s-1x1.mp4", "s.qml", "stills/s-16x9-0.50.png"}));
        QVERIFY(QFileInfo(dir.filePath("clips/s-1x1.mp4")).size() > 0);
    }

    void cliFrameRingsTheMarkedPoint() {
        if (binary().isEmpty())
            QSKIP("Set OMOTION_BINARY to run the CLI tests.");
        QTemporaryDir dir;
        QVERIFY(write(dir.filePath("s.qml"), tinyScene));
        QCOMPARE(run({"frame", "s.qml", "--at", "0", "--mark", "960,540", "-o", "f.png"}, dir.path()).code, 0);
        const QImage frame(dir.filePath("f.png"));
        // The ring's radius is 7.5% of the short side.
        bool pink = false;
        for (int x = 1035; x <= 1047 && !pink; ++x) {
            const QColor c = frame.pixelColor(x, 540);
            pink = c.red() > 200 && c.green() < 100 && c.blue() > 70;
        }
        QVERIFY(pink);
        QVERIFY(frame.pixelColor(960, 540) == QColor(Qt::black));
    }

    void cliCheckFailsOnAClockWithJson() {
        if (binary().isEmpty())
            QSKIP("Set OMOTION_BINARY to run the CLI tests.");
        QTemporaryDir dir;
        QVERIFY(write(dir.filePath("s.qml"), QByteArray(tinyScene).replace("color: \"black\"\n",
                                                                           "color: \"black\"\n    Timer { running: true }\n")));
        const Run result = run({"check", "s.qml", "--json"}, dir.path());
        QCOMPARE(result.code, 1);
        const QJsonObject json = QJsonDocument::fromJson(result.out).object();
        QCOMPARE(json["ok"].toBool(true), false);
        QVERIFY(QString::fromUtf8(result.out).contains("Timer runs on the wall clock"));
    }

    // Found in film 2: a frame between beats that no sheet showed.
    void cliSheetShowsTheFastestChangesAndAStrayFrame() {
        if (binary().isEmpty())
            QSKIP("Set OMOTION_BINARY to run the CLI tests.");
        QTemporaryDir dir;
        // A slow drift, a cut at 1.5 s, and one stray frame at 0.5 s.
        QVERIFY(write(dir.filePath("s.qml"),
                      "import QtQuick\nRectangle {\n    property real t\n    property real duration: 2\n"
                      "    property int fps: 10\n    color: t < 1.5 ? \"black\" : \"#1040a0\"\n"
                      "    Rectangle { width: 200; height: 200; x: 300 + parent.t * 40; y: 300; color: \"white\" }\n"
                      "    Rectangle { width: 600; height: 600; x: 900; y: 200; color: \"#e04040\"; "
                      "visible: Math.round(parent.t * 10) === 5 }\n}\n"));
        const Run sheet = run({"sheet", "s.qml", "--json"}, dir.path());
        QCOMPARE(sheet.code, 0);
        const QJsonArray strips = QJsonDocument::fromJson(sheet.out).object()["sheets"].toArray()[0]
                                      .toObject()["fast_changes"].toArray();
        QStringList found;
        for (const auto &strip : strips)
            found << QString("%1 %2").arg(strip.toObject()["kind"].toString()).arg(strip.toObject()["frame"].toInt());
        QVERIFY2(found.contains("change 15"), qPrintable(found.join(", ")));
        QVERIFY2(found.contains("flash 5"), qPrintable(found.join(", ")));
        QCOMPARE(strips[0].toObject()["times"].toArray().size(), 5);
        // Asked for moments, it shows only those.
        const Run times = run({"sheet", "s.qml", "--times", "0.5", "--json"}, dir.path());
        QCOMPARE(QJsonDocument::fromJson(times.out).object()["sheets"].toArray()[0].toObject()["fast_changes"]
                     .toArray().size(), 0);
    }

    void cliRendersTheInstagramFeedFormat() {
        if (binary().isEmpty())
            QSKIP("Set OMOTION_BINARY to run the CLI tests.");
        QTemporaryDir dir;
        QVERIFY(write(dir.filePath("s.qml"), QByteArray(tinyScene).replace("\"1:1\"]", "\"4:5\"]")));
        const Run frame = run({"frame", "s.qml", "--at", "0.5", "--format", "4:5", "--json"}, dir.path());
        QCOMPARE(frame.code, 0);
        QCOMPARE(QImage(QDir(dir.path()).filePath("s-4x5-0.50.png")).size(), QSize(1080, 1350));
        QCOMPARE(run({"check", "s.qml"}, dir.path()).code, 0);
    }

    void cliStatusSaysWhereTheFilmIs() {
        if (binary().isEmpty())
            QSKIP("Set OMOTION_BINARY to run the CLI tests.");
        QTemporaryDir dir;
        QCOMPARE(run({"new", "film"}, dir.path()).code, 0);
        const Run fresh = run({"status", "film"}, dir.path());
        QCOMPARE(fresh.code, 0);
        QVERIFY2(fresh.out.contains("at gate 1, References and assets"), fresh.out);

        // A passed gate whose evidence is gone fails, and says what's missing.
        QVERIFY(write(dir.filePath("film/gates.md"),
                      "- [x] 1 References and assets: approved by the person, 2026-10-23\n"
                      "- [ ] 2 Style guide and shot list: waiting for the person's approval since 2026-10-23\n"));
        QVERIFY(write(dir.filePath("film/notes.md"), "# Notes\n\n- [ ] 001 · 1.00s · 16:9 · Hook · (10, 10)\n  note: bigger\n"));
        const Run json = run({"status", "--json"}, dir.filePath("film/reviews"));
        QCOMPARE(json.code, 1);
        const QJsonObject status = QJsonDocument::fromJson(json.out).object();
        QCOMPARE(status["gate"].toInt(), 2);
        QVERIFY(status["gates"].toArray()[1].toObject()["waiting_for_person"].toBool());
        QCOMPARE(status["open_notes"].toArray()[0].toObject()["id"].toString(), QString("001"));
        QCOMPARE(status["problems"].toArray()[0].toString(), QString("Gate 1 is passed, but brief.md is still the template."));
    }

    void cliSkillInstallOnlyWritesIntoAStudio() {
        if (binary().isEmpty())
            QSKIP("Set OMOTION_BINARY to run the CLI tests.");
        QTemporaryDir dir;
        const Run outside = run({"skill", "install"}, dir.path());
        QCOMPARE(outside.code, 1);
        QVERIFY(outside.err.contains("isn't inside a film"));
        QCOMPARE(filesIn(dir.path()), QStringList());
    }

    // --- Screenshots to look at (bin/screens) -------------------------------

    void screens() {
        const QString out = qEnvironmentVariable("OMOTION_SCREENS");
        if (out.isEmpty())
            QSKIP("Set OMOTION_SCREENS to a folder to write the screenshot set.");
        QDir().mkpath(out);
        auto shoot = [&](App &app, const QString &name) {
            app.settle();
            QVERIFY(app.window->grabWindow().save(QDir(out).filePath(name + ".png")));
        };
        {
            App app;
            const QString root = app.studio();
            Studio::Note note;
            note.at = 6;
            note.format = "16:9";
            note.beat = "Review";
            note.text = "This highlight should land a beat later.";
            QString error;
            Studio::addNote(root, note, &error);
            QVERIFY(write(QDir(root).filePath("activity.md"), "# Activity\n\n- 14:02 rebuilt the review beat\n"));
            QVERIFY(app.open(QDir(root).filePath("scene.qml"), {1480, 900}, 6.0));
            shoot(app, "wide");
            app.key(Qt::Key_Question, Qt::ShiftModifier, "?");
            shoot(app, "help");
            app.key(Qt::Key_Escape);
            QQuickItem *preview = app.item("preview");
            QTest::mouseClick(app.window, Qt::LeftButton, Qt::NoModifier,
                              preview->mapToScene(QPointF(preview->width() * 0.6, preview->height() * 0.4)).toPoint());
            shoot(app, "note-editor");
        }
        {
            App app;
            QVERIFY(app.open(QDir(app.studio()).filePath("scene.qml"), {621, 670}, 6.0));
            shoot(app, "tiled-film");
            app.key(Qt::Key_P, Qt::NoModifier, "p");
            shoot(app, "tiled-studio");
            app.key(Qt::Key_P, Qt::NoModifier, "p");
            shoot(app, "tiled-notes");
        }
        {
            App app("ar");
            QVERIFY(app.open(QDir(app.studio()).filePath("scene.qml"), {1480, 900}, 6.0));
            shoot(app, "arabic");
        }
        {
            App app;
            QVERIFY(app.open(QString(), {1280, 820}));
            shoot(app, "welcome");
        }
        qInfo().noquote() << "Wrote the screenshot set to" << out;
    }
};

QTEST_MAIN(AppTests)
#include "app_tests.moc"
