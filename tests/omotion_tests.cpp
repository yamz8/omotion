#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QTemporaryDir>
#include <QtTest>

#include "backend.h"
#include "exporter.h"
#include "project.h"
#include "scene.h"
#include "theme.h"

namespace {

// A picture with something in every corner, so misplaced pixels show.
QString writePicture(const QString &path, const QSize &size = {200, 100}) {
    QImage image(size, QImage::Format_ARGB32);
    image.fill(QColor("#204060"));
    QPainter painter(&image);
    painter.fillRect(QRect(10, 10, 60, 40), QColor("#ff3300"));
    painter.fillRect(QRect(120, 50, 50, 30), QColor("#33ff66"));
    painter.end();
    image.save(path);
    return path;
}

QByteArray hash(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}

// How many pixels differ by more than a little in any channel.
int differences(const QImage &a, const QImage &b) {
    int count = 0;
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x) {
            const QRgb p = a.pixel(x, y), q = b.pixel(x, y);
            if (std::abs(qRed(p) - qRed(q)) > 6 || std::abs(qGreen(p) - qGreen(q)) > 6 ||
                std::abs(qBlue(p) - qBlue(q)) > 6 || std::abs(qAlpha(p) - qAlpha(q)) > 6)
                ++count;
        }
    return count;
}

void writeText(const QString &path, const QByteArray &text) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(text);
}

}

class OmotionTests : public QObject {
    Q_OBJECT

private slots:
    void easesStartAndEndInPlace() {
        for (const QString &ease : Scene::eases()) {
            QCOMPARE(Scene::ease(ease, 0), 0.0);
            QCOMPARE(Scene::ease(ease, 1), 1.0);
        }
        QCOMPARE(Scene::ease("hold", .99), 0.0);
        QVERIFY(Scene::ease("outBack", .7) > 1);  // It overshoots.
    }

    void interpolatesBetweenKeyframes() {
        Scene scene;
        Layer layer;
        layer.keys["x"] = {Key{1, 100, "linear"}, Key{3, 300, "linear"}};
        layer.base["y"] = 42;
        scene.layers.append(layer);
        QCOMPARE(scene.value(layer, "x", 0), 100.0);
        QCOMPARE(scene.value(layer, "x", 2), 200.0);
        QCOMPARE(scene.value(layer, "x", 9), 300.0);
        QCOMPARE(scene.value(layer, "y", 2), 42.0);
        QCOMPARE(scene.value(layer, "scale", 2), 1.0);
    }

    void setValueKeysOnlyAnimatedProperties() {
        Scene scene;
        scene.layers.append(Layer{});
        scene.setValue(0, "x", 1, 50);
        QCOMPARE(scene.layers[0].base["x"], 50.0);
        QVERIFY(scene.layers[0].keys.isEmpty());

        scene.keyAll(0, 0);
        scene.setValue(0, "x", 2, 80);
        QCOMPARE(scene.layers[0].keys["x"].size(), 2);
        QCOMPARE(scene.value(scene.layers[0], "x", 2), 80.0);
        QCOMPARE(scene.keyTimes(0), (QList<double>{0, 2}));

        QVERIFY(scene.removeKeysAt(0, 2));
        QVERIFY(scene.removeKeysAt(0, 0));
        // The last key going leaves the value it held.
        QVERIFY(scene.layers[0].keys.isEmpty());
        QCOMPARE(scene.layers[0].base["x"], 50.0);
    }

    void cyclesEasing() {
        Scene scene;
        scene.layers.append(Layer{});
        scene.keyAll(0, 0);
        QCOMPARE(scene.cycleEaseAt(0, 0), QString("outBack"));
        QCOMPARE(scene.layers[0].keys["x"][0].ease, QString("outBack"));
        QVERIFY(scene.cycleEaseAt(0, 1).isEmpty());
    }

    void jsonRoundTripsAndKeepsUnknownFields() {
        const QByteArray json = R"({"width": 640, "height": 360, "fps": 24, "duration": 2, "notes": "mine",
            "layers": [{"name": "a", "source": "a.png", "x": 5, "comment": "hi",
                        "keys": {"rotation": [{"t": 1, "v": 90, "ease": "out"}, {"t": 0, "v": 0}]}},
                       {"name": "b", "source": "b.png", "parent": "a"}]})";
        QString error;
        Scene scene = Scene::fromJson(QJsonDocument::fromJson(json).object(), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(scene.layers[0].keys["rotation"][0].t, 0.0);  // Sorted.
        const QJsonObject out = scene.toJson();
        QCOMPARE(out["notes"].toString(), QString("mine"));
        QCOMPARE(out["layers"].toArray()[0].toObject()["comment"].toString(), QString("hi"));
        Scene again = Scene::fromJson(out, &error);
        QVERIFY(error.isEmpty());
        QCOMPARE(again.toJson(), out);
    }

    void rejectsBrokenScenes() {
        auto errorFor = [](const QByteArray &json) {
            QString error;
            Scene::fromJson(QJsonDocument::fromJson(json).object(), &error);
            return error;
        };
        QVERIFY(errorFor(R"({"layers": [{"name": "a"}]})").contains("no source"));
        QVERIFY(errorFor(R"({"layers": [{"name": "a", "source": "a"}, {"name": "a", "source": "b"}]})").contains("unique"));
        QVERIFY(errorFor(R"({"layers": [{"name": "a", "source": "a", "parent": "b"}, {"name": "b", "source": "b", "parent": "a"}]})").contains("ancestor"));
        QVERIFY(errorFor(R"({"layers": [{"name": "a", "source": "a", "parent": "zz"}]})").contains("doesn't exist"));
        QVERIFY(errorFor(R"({"layers": [{"name": "a", "source": "a", "keys": {"x": [{"t": 0, "v": 0, "ease": "bouncy"}]}}]})").contains("easing"));
        QVERIFY(errorFor(R"({"layers": [{"name": "a", "source": "a", "keys": {"size": []}}]})").contains("isn't a property"));
        QVERIFY(errorFor(R"({"fps": 0})").contains("fps"));
    }

    void openingAnImageCopiesItIntoANewProject() {
        QTemporaryDir dir;
        const QString picture = writePicture(dir.filePath("cat.png"));
        const QByteArray before = hash(picture);
        QString error;
        const QString project = Project::open(picture, &error);
        QVERIFY2(!project.isEmpty(), qPrintable(error));
        QCOMPARE(QFileInfo(project).fileName(), QString("cat.omotion"));
        QVERIFY(QFile::exists(project + "/assets/cat.png"));
        QCOMPARE(hash(picture), before);

        const Scene scene = Project::load(project, &error);
        QVERIFY(error.isEmpty());
        QCOMPARE(scene.width, 200);
        QCOMPARE(scene.layers.size(), 1);
        // Opening the picture again finds the same project rather than a new one.
        QCOMPARE(Project::open(picture, &error), project);
    }

    void openingAFolderOfImagesMakesALayerEach() {
        QTemporaryDir dir;
        writePicture(dir.filePath("1-sky.png"));
        writePicture(dir.filePath("2-tree.png"), {40, 80});
        QString error;
        QCOMPARE(Project::open(dir.path(), &error), dir.path());
        const Scene scene = Project::load(dir.path(), &error);
        QCOMPARE(scene.layers.size(), 2);
        QCOMPARE(scene.layers[1].name, QString("2-tree"));
        QVERIFY(Project::open(dir.filePath("nothing"), &error).isEmpty());
        QVERIFY(!error.isEmpty());
    }

    void bigImagesFitTheCanvas() {
        QTemporaryDir dir;
        const QString project = Project::open(writePicture(dir.filePath("big.png"), {4000, 3001}), nullptr);
        const Scene scene = Project::load(project, nullptr);
        QCOMPARE(scene.width, 1920);
        QCOMPARE(scene.height % 2, 0);
        QVERIFY(qAbs(scene.layers[0].base["scale"] - 0.48) < 1e-9);
    }

    void cuttingOutAPieceLooksTheSame() {
        QTemporaryDir dir;
        const QString project = Project::open(writePicture(dir.filePath("p.png")), nullptr);
        Scene scene = Project::load(project, nullptr);
        AssetCache assets(project);
        const QImage before = renderFrame(scene, assets, 0);
        const QByteArray original = hash(project + "/assets/p.png");

        QString error;
        const QPolygonF square{{5, 5}, {80, 5}, {80, 60}, {5, 60}};
        const int piece = Project::cutOut(scene, assets, 0, square, 0, true, &error);
        QVERIFY2(piece == 1, qPrintable(error));
        QCOMPARE(scene.layers[1].parent, scene.layers[0].name);
        QVERIFY(scene.layers[0].source != "assets/p.png");  // The hole is a new file...
        QCOMPARE(hash(project + "/assets/p.png"), original);  // ...and the old one is untouched.
        QCOMPARE(differences(renderFrame(scene, assets, 0), before), 0);

        // Moving the piece shows the hole behind it.
        scene.setValue(1, "x", 0, scene.layers[1].base["x"] + 100);
        QCOMPARE(renderFrame(scene, assets, 0).pixelColor(20, 20).alpha(), 255);
        QCOMPARE(renderFrame(scene, assets, 0).pixelColor(20, 20), QColor(Qt::black));
    }

    void cuttingATurnedLayerLooksTheSame() {
        QTemporaryDir dir;
        const QString project = Project::open(writePicture(dir.filePath("p.png")), nullptr);
        Scene scene = Project::load(project, nullptr);
        scene.layers[0].base["rotation"] = 30;
        scene.layers[0].base["scale"] = .7;
        AssetCache assets(project);
        const QImage before = renderFrame(scene, assets, 0);
        QString error;
        const QPolygonF shape{{60, 20}, {150, 30}, {120, 90}, {50, 70}};
        QCOMPARE(Project::cutOut(scene, assets, 0, shape, 0, true, &error), 1);
        // Only the antialiased seam along the cut may differ.
        QVERIFY(differences(renderFrame(scene, assets, 0), before) < 200 * 100 / 50);
    }

    void cutPiecesCanBeNamed() {
        QTemporaryDir dir;
        const QString project = Project::open(writePicture(dir.filePath("p.png")), nullptr);
        Scene scene = Project::load(project, nullptr);
        AssetCache assets(project);
        QString error;
        const QPolygonF square{{5, 5}, {80, 5}, {80, 60}, {5, 60}};
        QCOMPARE(Project::cutOut(scene, assets, 0, square, 0, false, &error, "Sun"), 1);
        QCOMPARE(scene.layers[1].name, QString("Sun"));
        QCOMPARE(scene.layers[1].source, QString("assets/sun.png"));
        QCOMPARE(scene.layers[0].source, QString("assets/p.png"));  // Copied out, so no hole.
        QCOMPARE(Project::cutOut(scene, assets, 0, square, 0, false, &error, "Sun"), -1);
    }

    void cuttingNothingFails() {
        QTemporaryDir dir;
        const QString project = Project::open(writePicture(dir.filePath("p.png")), nullptr);
        Scene scene = Project::load(project, nullptr);
        AssetCache assets(project);
        QString error;
        QCOMPARE(Project::cutOut(scene, assets, 0, {{500, 500}, {600, 500}, {600, 600}}, 0, true, &error), -1);
        QVERIFY(!error.isEmpty());
        QCOMPARE(scene.layers.size(), 1);
    }

    void movingThePivotKeepsTheLayerStill() {
        QTemporaryDir dir;
        const QString project = Project::open(writePicture(dir.filePath("p.png")), nullptr);
        Scene scene = Project::load(project, nullptr);
        scene.layers[0].base["rotation"] = 25;
        scene.layers[0].base["scaleX"] = 1.3;
        AssetCache assets(project);
        const QImage before = renderFrame(scene, assets, 0);
        Project::setPivot(scene, assets, 0, {30, 20}, 0);
        QVERIFY(scene.layers[0].anchor != QPointF(.5, .5));
        QVERIFY(differences(renderFrame(scene, assets, 0), before) < 20);
    }

    void backendEditsUndoAndSave() {
        QTemporaryDir dir;
        Backend backend;
        QVERIFY(backend.open(writePicture(dir.filePath("p.png"))));
        backend.select(0);
        backend.setProperty("x", 10);
        QCOMPARE(backend.values()["x"].toDouble(), 10.0);
        backend.undo();
        QCOMPARE(backend.values()["x"].toDouble(), 100.0);
        backend.redo();
        backend.flush();
        QString error;
        QCOMPARE(Project::load(backend.projectPath(), &error).layers[0].base["x"], 10.0);
    }

    void backendKeyframesAtThePlayhead() {
        QTemporaryDir dir;
        Backend backend;
        backend.open(writePicture(dir.filePath("p.png")));
        backend.select(0);
        backend.keyAll();
        backend.seek(1);
        backend.setProperty("rotation", 90);
        backend.seek(.5);
        QCOMPARE(backend.values()["rotation"].toDouble(), 45.0);  // inOut is halfway at the middle.
        QCOMPARE(backend.layers()[0].toMap()["keyTimes"].toList().size(), 2);
    }

    void backendDragsInTheParentsPixels() {
        QTemporaryDir dir;
        Backend backend;
        backend.open(writePicture(dir.filePath("p.png")));
        backend.select(0);
        backend.setTool("cut");
        for (const QPointF p : {QPointF(5, 5), QPointF(80, 5), QPointF(80, 60), QPointF(5, 60)})
            backend.addCutPoint(p.x(), p.y());
        backend.finishCut(true);
        QCOMPARE(backend.selected(), 1);
        backend.select(0);
        backend.setProperty("scale", 2);
        backend.select(1);
        const double x = backend.values()["x"].toDouble();
        backend.beginDrag(0, 0);
        backend.dragTo(20, 0);
        backend.endDrag();
        // 20 canvas pixels are 10 of the doubled parent's.
        QCOMPARE(backend.values()["x"].toDouble(), x + 10);
        backend.undo();
        QCOMPARE(backend.values()["x"].toDouble(), x);
    }

    void backendReloadsOutsideEdits() {
        QTemporaryDir dir;
        Backend backend;
        backend.open(writePicture(dir.filePath("p.png")));
        const QString file = Project::sceneFile(backend.projectPath());
        QJsonObject json = QJsonDocument::fromJson([&] { QFile f(file); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }()).object();
        json["duration"] = 9;
        writeText(file, QJsonDocument(json).toJson());
        QTRY_COMPARE(backend.sceneInfo()["duration"].toDouble(), 9.0);
        QVERIFY(backend.canUndo());  // The outside edit can be undone too.

        // While the file is broken, it's reported and left alone.
        writeText(file, "{ \"width\": ");
        QTRY_VERIFY(backend.error().contains("scene.json"));
        backend.select(0);
        backend.setProperty("x", 1);
        backend.flush();
        QFile broken(file);
        QVERIFY(broken.open(QIODevice::ReadOnly));
        QCOMPARE(broken.readAll(), QByteArray("{ \"width\": "));
    }

    void backendReportsMissingImages() {
        QTemporaryDir dir;
        Backend backend;
        backend.open(writePicture(dir.filePath("p.png")));
        QFile::remove(backend.projectPath() + "/assets/p.png");
        QTRY_VERIFY(backend.error().contains("assets/p.png"));
    }

    void exportsPngFrames() {
        QTemporaryDir dir;
        const QString project = Project::open(writePicture(dir.filePath("p.png")), nullptr);
        Scene scene = Project::load(project, nullptr);
        scene.duration = .5;
        scene.fps = 10;
        QString error;
        QVERIFY2(Exporter::run(scene, project, dir.filePath("frames"), &error), qPrintable(error));
        QCOMPARE(QDir(dir.filePath("frames")).entryList(QDir::Files).size(), 5);
        QVERIFY(!Exporter::run(scene, project, dir.filePath("out.mov"), &error));
        QVERIFY(error.contains(".mp4"));
    }

    void themeFallsBack() {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QString("black"));
    }

    void themeReadsAccent() {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        writeText(dir.path() + "/theme/colors.toml", "accent = \"#112233\"\n");
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#112233"));
        QCOMPARE(theme.accentForeground(), QString("white"));
    }
};

QTEST_MAIN(OmotionTests)
#include "omotion_tests.moc"
