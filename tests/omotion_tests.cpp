#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QLinearGradient>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineF>
#include <QPainter>
#include <QTemporaryDir>
#include <QtTest>

#include "ai.h"
#include "backend.h"
#include "exporter.h"
#include "project.h"
#include "scene.h"
#include "select.h"
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

    void shiftsKeyColumnsTogether() {
        Scene scene;
        scene.layers.append(Layer{});
        scene.layers[0].keys["x"] = {Key{0, 0, "inOut"}, Key{1, 10, "out"}, Key{2, 20, "linear"}};
        scene.layers[0].keys["rotation"] = {Key{1, 45, "inOut"}};
        const QList<KeyRef> moved = scene.shiftKeys({{0, 1}}, .51);
        // Snapped to a frame, and every property's key at that time came along.
        QCOMPARE(moved, (QList<KeyRef>{{0, 1.5}}));
        QCOMPARE(scene.layers[0].keys["x"][1].t, 1.5);
        QCOMPARE(scene.layers[0].keys["x"][1].ease, QString("out"));
        QCOMPARE(scene.layers[0].keys["rotation"][0].t, 1.5);

        // Landing on another key replaces it.
        scene.shiftKeys({{0, 1.5}}, .5);
        QCOMPARE(scene.layers[0].keys["x"].size(), 2);
        QCOMPARE(scene.value(scene.layers[0], "x", 2), 10.0);
    }

    void shiftedColumnsDontTrampleEachOther() {
        Scene scene;
        scene.layers.append(Layer{});
        scene.layers[0].keys["x"] = {Key{1, 10}, Key{2, 20}};
        // Moving both a second later passes 1 onto 2's old time; both survive.
        scene.shiftKeys({{0, 1}, {0, 2}}, 1);
        QCOMPARE(scene.layers[0].keys["x"].size(), 2);
        QCOMPARE(scene.layers[0].keys["x"][0].t, 2.0);
        QCOMPARE(scene.layers[0].keys["x"][0].v, 10.0);
        QCOMPARE(scene.layers[0].keys["x"][1].t, 3.0);
    }

    void copiesAndPastesKeysOntoAnotherLayer() {
        Scene scene;
        scene.layers.append(Layer{});
        scene.layers.append(Layer{});
        scene.layers[0].keys["y"] = {Key{1, 5, "outBack"}, Key{1.5, 50, "inOut"}};
        const QList<KeyClip> clip = scene.copyKeys({{0, 1}, {0, 1.5}});
        QCOMPARE(clip.size(), 2);
        QCOMPARE(scene.pasteKeys(1, 3, clip), (QList<KeyRef>{{1, 3}, {1, 3.5}}));
        QCOMPARE(scene.layers[1].keys["y"][0].ease, QString("outBack"));
        QCOMPARE(scene.value(scene.layers[1], "y", 3.5), 50.0);
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
        const int piece = Project::cutOut(scene, assets, 0, square, 0, true, &error, {}, 0, false);
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

    void softCutsStillAddUpToTheOriginal() {
        QTemporaryDir dir;
        const QString project = Project::open(writePicture(dir.filePath("p.png")), nullptr);
        Scene scene = Project::load(project, nullptr);
        AssetCache assets(project);
        const QImage before = renderFrame(scene, assets, 0);
        QString error;
        const QPolygonF square{{20, 20}, {90, 20}, {90, 70}, {20, 70}};
        QCOMPARE(Project::cutOut(scene, assets, 0, square, 0, true, &error, "soft", 8, false), 1);
        QCOMPARE(differences(renderFrame(scene, assets, 0), before), 0);

        // The piece fades out across its edge...
        const QImage piece = assets.image(scene.layers[1].source);
        const QJsonArray origin = scene.layers[1].raw["cut"].toObject()["origin"].toArray();
        const int edgeX = 20 - origin[0].toInt(), midY = 45 - origin[1].toInt();
        QVERIFY(qAlpha(piece.pixel(edgeX, midY)) > 60 && qAlpha(piece.pixel(edgeX, midY)) < 200);
        QCOMPARE(qAlpha(piece.pixel(edgeX + 15, midY)), 255);
        // ...and the hole only opens where the piece is solid.
        const QImage holed = assets.image(scene.layers[0].source);
        QCOMPARE(qAlpha(holed.pixel(55, 45)), 0);
        QCOMPARE(qAlpha(holed.pixel(20, 45)), 255);
    }

    void reshapingKeepsTheAnimationAndPivot() {
        QTemporaryDir dir;
        const QString project = Project::open(writePicture(dir.filePath("p.png")), nullptr);
        Scene scene = Project::load(project, nullptr);
        AssetCache assets(project);
        const QImage before = renderFrame(scene, assets, 0);
        QString error;
        QCOMPARE(Project::cutOut(scene, assets, 0, {{10, 10}, {70, 10}, {70, 50}, {10, 50}}, 0, true, &error), 1);
        Project::setPivot(scene, assets, 1, {15, 15}, 0);
        scene.keyAll(1, 0);
        scene.setValue(1, "rotation", 0, 0);
        const QList<Key> keys = scene.layers[1].keys["rotation"];
        const double x = scene.layers[1].base["x"];

        const QPolygonF bigger{{5, 5}, {100, 5}, {100, 60}, {5, 60}};
        QVERIFY2(Project::reshapeCut(scene, assets, 1, bigger, 0, 0, &error), qPrintable(error));
        QCOMPARE(scene.layers[1].keys["rotation"].size(), keys.size());
        QCOMPARE(scene.layers[1].base["x"], x);
        QCOMPARE(differences(renderFrame(scene, assets, 0), before), 0);
        // The pivot is still on the picture's 15,15.
        const QTransform toCanvas = layerTransform(scene, assets, 1, 0);
        const QSizeF size = assets.size(scene.layers[1].source);
        const QPointF pivot = toCanvas.map(QPointF(scene.layers[1].anchor.x() * size.width(),
                                                   scene.layers[1].anchor.y() * size.height()));
        QVERIFY(QLineF(pivot, QPointF(15, 15)).length() < .01);
        QCOMPARE(Project::cutOutline(scene, assets, 1, 0).boundingRect(), bigger.boundingRect());
    }

    void reshapingOnePieceKeepsTheOthersHoles() {
        QTemporaryDir dir;
        const QString project = Project::open(writePicture(dir.filePath("p.png")), nullptr);
        Scene scene = Project::load(project, nullptr);
        AssetCache assets(project);
        const QImage before = renderFrame(scene, assets, 0);
        QString error;
        QCOMPARE(Project::cutOut(scene, assets, 0, {{10, 10}, {60, 10}, {60, 50}, {10, 50}}, 0, true, &error, "a", 0, false), 1);
        QCOMPARE(Project::cutOut(scene, assets, 0, {{120, 50}, {170, 50}, {170, 80}, {120, 80}}, 0, true, &error, "b", 0, false), 1);
        QVERIFY(Project::reshapeCut(scene, assets, scene.indexOf("a"), {{10, 10}, {40, 10}, {40, 40}, {10, 40}}, 0, 3, &error));
        const QImage holed = assets.image(scene.layers[0].source);
        QCOMPARE(qAlpha(holed.pixel(150, 65)), 0);   // b's hole is still there.
        QCOMPARE(qAlpha(holed.pixel(55, 45)), 255);  // a's old hole is filled back in.
        QCOMPARE(differences(renderFrame(scene, assets, 0), before), 0);
        QCOMPARE(scene.layers[0].raw["uncut"].toString(), QString("assets/p.png"));
    }

    void reshapingRedrawsPiecesCutFromPieces() {
        QTemporaryDir dir;
        const QString project = Project::open(writePicture(dir.filePath("p.png")), nullptr);
        Scene scene = Project::load(project, nullptr);
        AssetCache assets(project);
        const QImage before = renderFrame(scene, assets, 0);
        QString error;
        Project::cutOut(scene, assets, 0, {{5, 5}, {90, 5}, {90, 70}, {5, 70}}, 0, true, &error, "arm", 0, false);
        QVERIFY(Project::cutOut(scene, assets, scene.indexOf("arm"), {{15, 15}, {40, 15}, {40, 40}, {15, 40}}, 0, true,
                                &error, "hand", 0, false) > 0);
        QVERIFY2(Project::reshapeCut(scene, assets, scene.indexOf("arm"), {{5, 5}, {110, 5}, {110, 80}, {5, 80}}, 0, 2, &error),
                 qPrintable(error));
        QCOMPARE(differences(renderFrame(scene, assets, 0), before), 0);
        // The hand's hole in the arm survived the arm being redrawn.
        const Layer &arm = scene.layers[scene.indexOf("arm")];
        QVERIFY(arm.raw.contains("uncut"));
        QVERIFY(arm.source != arm.raw["uncut"].toString());
    }

    void renamingASourceKeepsItsPiecesReshapable() {
        QTemporaryDir dir;
        Backend backend;
        backend.open(writePicture(dir.filePath("p.png")));
        backend.select(0);
        backend.setTool("cut");
        for (const QPointF p : {QPointF(5, 5), QPointF(80, 5), QPointF(80, 60), QPointF(5, 60)})
            backend.addCutPoint(p.x(), p.y());
        backend.finishCut(true);
        QVERIFY(backend.renameLayer(0, "body"));
        backend.select(1);
        backend.startReshape();
        QCOMPARE(backend.tool(), QString("cut"));
        QCOMPARE(backend.cutPoints().size(), 4);
        // Drag a corner out and add a point on an edge, then apply.
        backend.moveCutPoint(backend.cutPointNear(80, 60, 2), 120, 90);
        QCOMPARE(backend.insertCutPoint(40, 5.5, 2), 1);
        backend.finishCut(true);
        QCOMPARE(backend.error(), QString());
        QCOMPARE(Project::cutOutline(backend.scene(), backend.assets(), 1, 0).size(), 5);
        backend.undo();
        QCOMPARE(Project::cutOutline(backend.scene(), backend.assets(), 1, 0).size(), 4);
    }

    void layersNotCutHereCantBeReshaped() {
        QTemporaryDir dir;
        Backend backend;
        backend.open(writePicture(dir.filePath("p.png")));
        backend.select(0);
        backend.startReshape();
        QCOMPARE(backend.tool(), QString("move"));
        QVERIFY(backend.notice().contains("wasn't cut"));
    }

    // A scene of two flat colour squares, one over the other, for checking looks.
    static Scene twoSquares(QTemporaryDir &dir, AssetCache &assets) {
        auto square = [&](const QString &name, const QColor &color) {
            QImage image(40, 40, QImage::Format_ARGB32);
            image.fill(color);
            image.save(dir.filePath(name));
        };
        square("under.png", QColor(0, 0, 200));
        square("over.png", QColor(200, 0, 0));
        Scene scene = Scene::fromJson(QJsonDocument::fromJson(R"({"width": 100, "height": 100, "layers": [
            {"name": "under", "source": "under.png", "x": 50, "y": 50},
            {"name": "over", "source": "over.png", "x": 50, "y": 50}]})").object());
        assets.setDirectory(dir.path());
        return scene;
    }

    void blendsMixWithTheLayersBelow() {
        QTemporaryDir dir;
        AssetCache assets;
        Scene scene = twoSquares(dir, assets);
        QCOMPARE(renderFrame(scene, assets, 0).pixelColor(50, 50), QColor(200, 0, 0));
        scene.layers[1].blend = "add";
        QCOMPARE(renderFrame(scene, assets, 0).pixelColor(50, 50), QColor(200, 0, 200));
        scene.layers[1].blend = "multiply";
        QCOMPARE(renderFrame(scene, assets, 0).pixelColor(50, 50), QColor(0, 0, 0));
        scene.layers[1].blend = "screen";
        QCOMPARE(renderFrame(scene, assets, 0).pixelColor(50, 50), QColor(200, 0, 200));

        QString error;
        Scene::fromJson(QJsonDocument::fromJson(R"({"layers": [{"name": "a", "source": "a", "blend": "glowy"}]})").object(), &error);
        QVERIFY(error.contains("blend"));
        QCOMPARE(scene.toJson()["layers"].toArray()[1].toObject()["blend"].toString(), QString("screen"));
        QVERIFY(!scene.toJson()["layers"].toArray()[0].toObject().contains("blend"));
    }

    void toneChangesColour() {
        QTemporaryDir dir;
        AssetCache assets;
        Scene scene = twoSquares(dir, assets);
        scene.layers[1].base["brightness"] = 1;
        QCOMPARE(renderFrame(scene, assets, 0).pixelColor(50, 50), QColor(255, 255, 255));
        scene.layers[1].base["brightness"] = -1;
        QCOMPARE(renderFrame(scene, assets, 0).pixelColor(50, 50), QColor(0, 0, 0));
        scene.layers[1].base["brightness"] = 0;
        scene.layers[1].base["saturation"] = 0;
        const QColor grey = renderFrame(scene, assets, 0).pixelColor(50, 50);
        QCOMPARE(grey.red(), grey.green());
        QCOMPARE(grey.green(), grey.blue());
        scene.layers[1].base["saturation"] = 1;
        scene.layers[1].base["contrast"] = 3;
        QCOMPARE(renderFrame(scene, assets, 0).pixelColor(50, 50), QColor(255, 0, 0));
        // Looks can be keyframed like anything else.
        scene.layers[1].keys["brightness"] = {Key{0, 0}, Key{1, -1}};
        QVERIFY(renderFrame(scene, assets, .5).pixelColor(50, 50).red() < 200);
    }

    void blurAndGlowSpreadPastTheEdge() {
        QTemporaryDir dir;
        AssetCache assets;
        Scene scene = twoSquares(dir, assets);
        scene.layers.removeFirst();
        QCOMPARE(renderFrame(scene, assets, 0).pixelColor(27, 50), QColor(0, 0, 0));
        scene.layers[0].base["blur"] = 10;
        const QImage blurred = renderFrame(scene, assets, 0);
        QVERIFY(blurred.pixelColor(27, 50).red() > 10);        // Spread outwards...
        QVERIFY(blurred.pixelColor(50, 50).red() > 190);       // ...the middle stays...
        QVERIFY(qAbs(blurred.pixelColor(30, 50).red() - blurred.pixelColor(69, 50).red()) < 8);  // ...and centred.
        scene.layers[0].base["blur"] = 0;
        scene.layers[0].base["glow"] = 10;
        const QImage glowing = renderFrame(scene, assets, 0);
        QVERIFY(glowing.pixelColor(27, 50).red() > 10);
        QVERIFY(qAbs(glowing.pixelColor(50, 50).red() - (255 - 55 * 55 / 255.0)) <= 1);  // Screened over itself.
    }

    void keyAllLeavesUnusedLooksAlone() {
        Scene scene;
        scene.layers.append(Layer{});
        scene.layers[0].base["glow"] = 4;
        scene.keyAll(0, 0);
        QVERIFY(scene.layers[0].keys.contains("glow"));
        QVERIFY(!scene.layers[0].keys.contains("blur"));
        QVERIFY(scene.layers[0].keys.contains("rotation"));
        QVERIFY(!scene.toJson()["layers"].toArray()[0].toObject().contains("blur"));
    }

    void smallDrawingUsesShrunkCopiesThatMatch() {
        QTemporaryDir dir;
        const QString project = Project::open(writePicture(dir.filePath("p.png"), {800, 400}), nullptr);
        Scene scene = Project::load(project, nullptr);
        AssetCache assets(project);
        double factor = 0;
        QCOMPARE(assets.mip("assets/p.png", .2, &factor).width(), 200);
        QCOMPARE(factor, .25);
        assets.mip("assets/p.png", 2, &factor);
        QCOMPARE(factor, 1.0);
        // A quarter-size render from the shrunk copy looks like the full one shrunk.
        const QImage small = renderFrame(scene, assets, 0, {200, 100});
        const QImage reference = renderFrame(scene, assets, 0).scaled(200, 100, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        QVERIFY(differences(small, reference) < 200 * 100 / 50);
    }

    void duplicatesDontInheritCuts() {
        QTemporaryDir dir;
        Backend backend;
        backend.open(writePicture(dir.filePath("p.png")));
        backend.select(0);
        backend.setTool("cut");
        for (const QPointF p : {QPointF(5, 5), QPointF(80, 5), QPointF(80, 60), QPointF(5, 60)})
            backend.addCutPoint(p.x(), p.y());
        backend.finishCut(true);
        backend.setBlend("add");
        backend.duplicateLayer();
        QCOMPARE(backend.selected(), 2);
        const Layer &copy = backend.scene().layers[2];
        QCOMPARE(copy.name, QString("p piece copy"));
        QCOMPARE(copy.blend, QString("add"));
        QVERIFY(!Project::isCut(backend.scene(), 2));
        QVERIFY(Project::isCut(backend.scene(), 1));
    }

    void filledHolesTakeTheColoursAround() {
        QTemporaryDir dir;
        // Left half one colour, right half another, with a red square on the seam.
        QImage picture(200, 100, QImage::Format_ARGB32);
        QPainter painter(&picture);
        painter.fillRect(0, 0, 100, 100, QColor(0, 0, 200));
        painter.fillRect(100, 0, 100, 100, QColor(0, 200, 0));
        painter.fillRect(80, 30, 40, 40, QColor(255, 0, 0));
        painter.end();
        picture.save(dir.filePath("p.png"));
        const QString project = Project::open(dir.filePath("p.png"), nullptr);
        Scene scene = Project::load(project, nullptr);
        AssetCache assets(project);
        const QImage before = renderFrame(scene, assets, 0);
        QString error;
        QCOMPARE(Project::cutOut(scene, assets, 0, {{78, 28}, {122, 28}, {122, 72}, {78, 72}}, 0, true, &error, "red", 2), 1);
        QVERIFY(scene.layers[1].raw["cut"].toObject()["fill"].toBool());
        // Nothing shows until the piece moves, but a thin mix along the soft edge...
        QVERIFY(differences(renderFrame(scene, assets, 0), before) < 4 * 44 * 4);
        // ...and then the gap is the colours around it, no red, no see-through.
        scene.layers[1].visible = false;
        const QImage filled = renderFrame(scene, assets, 0);
        const QColor left = filled.pixelColor(85, 50), right = filled.pixelColor(115, 50);
        QCOMPARE(left.alpha(), 255);
        QVERIFY(left.red() < 30 && left.blue() > left.green());
        QVERIFY(right.red() < 30 && right.green() > right.blue());
        // Reshaping can turn it off again.
        QVERIFY(Project::reshapeCut(scene, assets, 1, {{78, 28}, {122, 28}, {122, 72}, {78, 72}}, 0, 2, &error, 0));
        QCOMPARE(renderFrame(scene, assets, 0).pixelColor(100, 50), QColor(Qt::black));
    }

    void bigHolesFillQuicklyAtLowerResolution() {
        QTemporaryDir dir;
        QImage picture(3000, 2000, QImage::Format_ARGB32);
        QPainter painter(&picture);
        QLinearGradient gradient(0, 0, 3000, 0);
        gradient.setColorAt(0, QColor(250, 0, 0));
        gradient.setColorAt(1, QColor(0, 0, 250));
        painter.fillRect(picture.rect(), gradient);
        painter.end();
        picture.save(dir.filePath("p.png"));
        const QString project = Project::open(dir.filePath("p.png"), nullptr);
        Scene scene = Project::load(project, nullptr);
        AssetCache assets(project);
        QElapsedTimer timer;
        timer.start();
        QString error;
        // A third of the canvas, which is 1920 wide.
        QVERIFY(Project::cutOut(scene, assets, 0, {{640, 100}, {1280, 100}, {1280, 1100}, {640, 1100}}, 0, true, &error) > 0);
        QVERIFY2(timer.elapsed() < 5000, qPrintable(QString::number(timer.elapsed())));
        scene.layers[1].visible = false;
        const QColor middle = renderFrame(scene, assets, 0).pixelColor(960, 600);
        // Halfway along a red-to-blue gradient, the fill is a mix of both.
        QVERIFY2(middle.red() > 80 && middle.blue() > 80, qPrintable(middle.name()));
    }

    void smartSelectFindsTheClickedArea() {
        // A blue background with a red ring around a white middle.
        QImage image(100, 80, QImage::Format_ARGB32);
        image.fill(QColor(0, 0, 200));
        QPainter painter(&image);
        painter.fillRect(20, 20, 50, 40, QColor(200, 0, 0));
        painter.fillRect(35, 30, 20, 20, Qt::white);
        painter.end();

        const QImage red = Select::region(image, {22, 22}, 30);
        QCOMPARE(Select::regionCount(red), 1);
        // The white middle is inside the red, so it's taken too: the shape is solid.
        QCOMPARE(red.pixelColor(45, 40).alpha(), 255);
        QCOMPARE(red.pixelColor(10, 10).alpha(), 0);
        const QRectF bounds = Select::outline(red).boundingRect();
        QVERIFY(qAbs(bounds.left() - 20.5) < 1 && qAbs(bounds.right() - 69.5) < 1);
        QVERIFY(qAbs(bounds.top() - 20.5) < 1 && qAbs(bounds.bottom() - 59.5) < 1);
        QVERIFY(Select::outline(red).size() <= 8);  // A rectangle simplifies to a few corners.

        // The background is the outside; it doesn't swallow the ring.
        const QImage blue = Select::region(image, {2, 2}, 30);
        QCOMPARE(blue.pixelColor(22, 22).alpha(), 0);
        // Outside the image, or on clear pixels, there's nothing.
        QCOMPARE(Select::regionCount(Select::region(image, {200, 2}, 30)), 0);
    }

    void smartSelectToleranceSetsHowFarColoursReach() {
        QImage image(256, 10, QImage::Format_ARGB32);
        for (int x = 0; x < 256; ++x)
            for (int y = 0; y < 10; ++y)
                image.setPixelColor(x, y, QColor(x, x, x));
        auto width = [&](int tolerance) {
            return Select::outline(Select::region(image, {128, 5}, tolerance)).boundingRect().width();
        };
        QVERIFY(width(10) < width(40));
        QVERIFY(qAbs(width(40) - 80) < 3);  // 128 ± 40.
    }

    void smartSelectWorksOnTurnedLayersAndJoinsClicks() {
        QTemporaryDir dir;
        Backend backend;
        backend.open(writePicture(dir.filePath("p.png")));  // 200×100: red at 10,10 60×40; green at 120,50 50×30.
        backend.select(0);
        backend.setProperty("rotation", 90);
        backend.setTool("wand");
        QCOMPARE(backend.tool(), QString("wand"));
        if (backend.wandMode() == "ai")
            backend.toggleWandMode();  // Colour select, which works without the AI.
        // Turned 90° clockwise about the centre (100, 50): image (40, 30) is canvas (120, -10).
        backend.smartSelect(120, -10, false);
        const QRectF red = QPolygonF(backend.cutPolygon()).boundingRect();
        // 40×60 turned, traced through pixel centres, and grown 2 px for the blended edge.
        QVERIFY2(qAbs(red.width() - 43) < 2 && qAbs(red.height() - 63) < 2,
                 qPrintable(QString("%1x%2").arg(red.width()).arg(red.height())));

        // A second shift-click on something that doesn't touch keeps the biggest.
        backend.smartSelect(85, 95, true);  // Image (145, 65): the green.
        QVERIFY(backend.notice().contains("touch"));

        // On to a real cut, with the shape the wand found.
        backend.smartSelect(120, -10, false);
        backend.finishCut(true);
        QCOMPARE(backend.error(), QString());
        QCOMPARE(backend.scene().layers.size(), 2);
        QCOMPARE(backend.tool(), QString("move"));
    }

    void smartSelectHandsItsShapeToTheCutTool() {
        QTemporaryDir dir;
        Backend backend;
        backend.open(writePicture(dir.filePath("p.png")));
        backend.select(0);
        backend.setTool("wand");
        if (backend.wandMode() == "ai")
            backend.toggleWandMode();
        backend.smartSelect(40, 30, false);
        const int points = backend.cutPoints().size();
        QVERIFY(points >= 4);
        backend.setTool("cut");
        QCOMPARE(backend.cutPoints().size(), points);
        backend.adjustTolerance(10);  // Not the wand any more: no reselect.
        QCOMPARE(backend.cutPoints().size(), points);
    }

    // A picture in three horizontal bands, with a depth map to match: far at
    // the top, near at the bottom.
    static QString bandedPicture(QTemporaryDir &dir, QImage *depth) {
        QImage picture(300, 150, QImage::Format_ARGB32);
        QPainter painter(&picture);
        painter.fillRect(0, 0, 300, 50, QColor(250, 120, 60));
        painter.fillRect(0, 50, 300, 50, QColor(120, 40, 160));
        painter.fillRect(0, 100, 300, 50, QColor(40, 20, 60));
        painter.end();
        picture.save(dir.filePath("bands.png"));
        *depth = QImage(60, 30, QImage::Format_Grayscale16);
        for (int y = 0; y < 30; ++y)
            for (int x = 0; x < 60; ++x)
                reinterpret_cast<quint16 *>(depth->scanLine(y))[x] = y < 10 ? 2000 : y < 20 ? 30000 : 60000;
        return dir.filePath("bands.png");
    }

    void depthSplitsIntoLayersThatAddUp() {
        QTemporaryDir dir;
        QImage depth;
        const QString project = Project::open(bandedPicture(dir, &depth), nullptr);
        Scene scene = Project::load(project, nullptr);
        AssetCache assets(project);
        const QImage before = renderFrame(scene, assets, 0);
        QString error;
        const QList<int> created = Project::splitByDepth(scene, assets, 0, depth, 3, 0, &error);
        QVERIFY2(created.size() == 2, qPrintable(error));
        QCOMPARE(scene.layers.size(), 3);
        // Nearest on top, and named for it.
        QCOMPARE(scene.layers[2].name, QString("foreground"));
        QCOMPARE(scene.layers[1].name, QString("middle ground"));
        QVERIFY(scene.layers[2].raw["cut"].toObject()["depth"].toDouble() >
                scene.layers[1].raw["cut"].toObject()["depth"].toDouble());
        // Put together, it's the picture; the shapes follow the depth.
        QVERIFY(differences(renderFrame(scene, assets, 0), before) < 300 * 6);
        const QImage near = assets.image(scene.layers[2].source);
        const QJsonArray origin = scene.layers[2].raw["cut"].toObject()["origin"].toArray();
        QVERIFY(origin[1].toInt() >= 90 && origin[1].toInt() <= 102);
        QCOMPARE(near.height() + origin[1].toInt(), 150);
        // Behind the nearer layers, each one is filled in: no holes to show.
        for (Layer &layer : scene.layers)
            layer.visible = layer.name == "middle ground";
        const QImage middle = renderFrame(scene, assets, 0);
        QCOMPARE(middle.pixelColor(150, 140).alpha(), 255);
        QVERIFY(middle.pixelColor(150, 140).blue() > 100);  // The middle band's purple, carried down.
        QCOMPARE(middle.pixelColor(150, 20), QColor(0, 0, 0));  // Above it, the background shows through.

        // Splitting again isn't allowed; flat depth has nothing to split.
        QVERIFY(Project::splitByDepth(scene, assets, 0, depth, 3, 0, &error).isEmpty());
        QImage flat(10, 10, QImage::Format_Grayscale16);
        flat.fill(Qt::gray);
        Scene fresh = Project::load(project, nullptr);
        fresh.layers.resize(1);
        fresh.layers[0].source = "assets/bands.png";
        QVERIFY(Project::splitByDepth(fresh, assets, 0, flat, 3, 0, &error).isEmpty());
    }

    void parallaxMovesNearerLayersFurther() {
        QTemporaryDir dir;
        QImage depth;
        const QString project = Project::open(bandedPicture(dir, &depth), nullptr);
        Scene scene = Project::load(project, nullptr);
        AssetCache assets(project);
        QString error;
        Project::splitByDepth(scene, assets, 0, depth, 3, 0, &error);
        QVERIFY(Project::animateParallax(scene, assets, 2, 30, &error));  // A piece stands for its source.
        auto canvasX = [&](int layer, double t) { return layerTransform(scene, assets, layer, t).map(QPointF(0, 0)).x(); };
        const double half = scene.duration / 2;
        const double far = canvasX(0, half) - canvasX(0, 0), middle = canvasX(1, half) - canvasX(1, 0),
                     near = canvasX(2, half) - canvasX(2, 0);
        QVERIFY2(near < middle && middle < far, qPrintable(QString("%1 %2 %3").arg(far).arg(middle).arg(near)));
        // It loops: the end is the start.
        QCOMPARE(canvasX(2, scene.duration), canvasX(2, 0));
        // And never shows past the picture's edge.
        for (double t = 0; t <= scene.duration; t += .25) {
            const QRectF covered = layerTransform(scene, assets, 0, t).mapRect(QRectF(0, 0, 300, 150));
            QVERIFY(covered.contains(QRectF(0, 0, scene.width, scene.height)));
        }
        QVERIFY(!Project::animateParallax(scene, assets, -1, 30, &error));
    }

    void masksCutLikeOutlines() {
        QTemporaryDir dir;
        const QString project = Project::open(writePicture(dir.filePath("p.png")), nullptr);
        Scene scene = Project::load(project, nullptr);
        AssetCache assets(project);
        // Two separate patches in one shape: something an outline can't be.
        QImage mask(200, 100, QImage::Format_Grayscale8);
        mask.fill(0);
        QPainter painter(&mask);
        painter.fillRect(10, 10, 60, 40, Qt::white);
        painter.fillRect(120, 50, 50, 30, Qt::white);
        painter.end();
        mask.copy(5, 5, 170, 80).save(project + "/assets/two-shape.png");
        Layer piece;
        piece.name = "two";
        piece.parent = "p";
        for (const QString &property : Scene::properties())
            piece.base.insert(property, Scene::defaultValue(property));
        piece.raw["cut"] = QJsonObject{{"from", "p"}, {"mask", "assets/two-shape.png"}, {"maskOrigin", QJsonArray{5, 5}},
                                       {"feather", 0}, {"punch", true}, {"fill", false}, {"order", 0}};
        scene.layers.append(piece);
        QString error;
        // Reshaping anything of p's redraws it all, the mask piece included.
        const QImage before = renderFrame(scene, assets, 0);
        QVERIFY(Project::cutOut(scene, assets, 0, {{180, 5}, {195, 5}, {195, 20}, {180, 20}}, 0, true, &error, {}, 0, false) > 0);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const Layer &cut = scene.layers[scene.indexOf("two")];
        QCOMPARE(assets.size(cut.source), QSizeF(170, 80));  // The stored mask's area.
        QCOMPARE(qAlpha(assets.image(scene.layers[0].source).pixel(30, 30)), 0);   // Both patches left holes...
        QCOMPARE(qAlpha(assets.image(scene.layers[0].source).pixel(140, 60)), 0);
        QCOMPARE(qAlpha(assets.image(scene.layers[0].source).pixel(100, 30)), 255);  // ...and nothing between.
        QVERIFY(differences(renderFrame(scene, assets, 0), before) < 10);
    }

    void aiSaysWhatsMissing() {
        QTemporaryDir dir;
        Ai::setPaths(dir.filePath("no-such-library.so"), dir.path());
        // A fresh Ai can't be made, so this checks the model side, which
        // doesn't need the runtime.
        QCOMPARE(Ai::missingModels().size(), Ai::models().size());
        QVERIFY(Ai::modelBytes() > 100000000);
        Ai::setPaths({}, {});
    }

    void aiFindsDepthAndThings() {
        if (!Ai::instance().problem().isEmpty())
            QSKIP(qPrintable("The AI isn't set up here: " + Ai::instance().problem()));
        // A dark disc on a light floor that fades with height, like a ball on a road.
        QImage picture(400, 300, QImage::Format_RGB32);
        QPainter painter(&picture);
        QLinearGradient floor(0, 0, 0, 300);
        floor.setColorAt(0, QColor(200, 220, 255));
        floor.setColorAt(1, QColor(120, 110, 100));
        painter.fillRect(picture.rect(), floor);
        painter.setBrush(QColor(200, 30, 30));
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(QPointF(200, 200), 50, 50);
        painter.end();
        QString error;
        const QImage depth = Ai::instance().depth(picture, &error);
        QVERIFY2(!depth.isNull(), qPrintable(error));
        // The ball stands out nearer than the sky at the top.
        const auto at = [&](double x, double y) {
            return reinterpret_cast<const quint16 *>(depth.constScanLine(int(y * depth.height())))[int(x * depth.width())];
        };
        QVERIFY(at(.5, .66) > at(.5, .05));

        const QList<QImage> masks = Ai::instance().segment(picture, "ball", {QPointF(200, 200)}, {1}, &error);
        QVERIFY2(!masks.isEmpty(), qPrintable(error));
        const QRectF bounds = Select::outline(masks.first()).boundingRect();
        QVERIFY2(qAbs(bounds.center().x() - 200) < 10 && qAbs(bounds.width() - 100) < 16,
                 qPrintable(QString("%1,%2 %3x%4").arg(bounds.x()).arg(bounds.y()).arg(bounds.width()).arg(bounds.height())));
    }

    static QString ballPicture(QTemporaryDir &dir) {
        QImage picture(400, 300, QImage::Format_RGB32);
        QPainter painter(&picture);
        QLinearGradient floor(0, 0, 0, 300);
        floor.setColorAt(0, QColor(200, 220, 255));
        floor.setColorAt(1, QColor(120, 110, 100));
        painter.fillRect(picture.rect(), floor);
        painter.setBrush(QColor(200, 30, 30));
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(QPointF(200, 200), 50, 50);
        painter.end();
        picture.save(dir.filePath("ball.png"));
        return dir.filePath("ball.png");
    }

    void backendAsksForMissingModels() {
        if (Ai::instance().runtimeMissing())
            QSKIP("ONNX Runtime isn't installed here.");
        QTemporaryDir dir, empty;
        Backend backend;
        backend.open(ballPicture(dir));
        backend.select(0);
        Ai::setPaths({}, empty.path());
        backend.setTool("wand");
        backend.smartSelect(200, 200, false);
        QCOMPARE(backend.aiPrompt(), QString("download"));
        QCOMPARE(backend.busy(), QString());
        backend.dismissAiPrompt();
        QCOMPARE(backend.aiPrompt(), QString());
        backend.splitByDepth(3);
        QCOMPARE(backend.aiPrompt(), QString("download"));
        Ai::setPaths({}, {});
    }

    void backendSelectsAndSplitsWithAi() {
        if (!Ai::instance().problem().isEmpty())
            QSKIP(qPrintable("The AI isn't set up here: " + Ai::instance().problem()));
        QTemporaryDir dir;
        Backend backend;
        backend.open(ballPicture(dir));
        backend.select(0);
        backend.setTool("wand");
        QCOMPARE(backend.wandMode(), QString("ai"));
        backend.smartSelect(200, 200, false);
        QVERIFY(!backend.busy().isEmpty());  // It works in the background...
        backend.waitForJobs();
        QVERIFY(backend.cutPoints().size() >= 4);  // ...and hands its outline to the cut.
        QVERIFY(backend.aiChoices() >= 2);
        const QRectF ball = QPolygonF(backend.cutPolygon()).boundingRect();
        QVERIFY2(qAbs(ball.width() - 100) < 20, qPrintable(QString::number(ball.width())));
        backend.cycleTolerance();  // T: the next guess.
        backend.waitForJobs();
        QCOMPARE(backend.aiChoice(), 1);
        backend.toggleWandMode();
        QCOMPARE(backend.wandMode(), QString("colour"));
        backend.toggleWandMode();
        backend.smartSelect(200, 200, false);
        backend.waitForJobs();
        backend.finishCut(true);
        QCOMPARE(backend.error(), QString());
        QCOMPARE(backend.scene().layers.size(), 2);

        // Depth: a fresh picture, split into layers, and undone in one step.
        QTemporaryDir other;
        Backend splitter;
        splitter.open(ballPicture(other));
        splitter.splitByDepth(3);
        // The view refreshes as new images land; that's not an edit.
        QTimer::singleShot(200, &splitter, [&] { splitter.dismissError(); });
        splitter.waitForJobs();
        QCOMPARE(splitter.error(), QString());
        QVERIFY(splitter.scene().layers.size() >= 2);
        splitter.animateParallax();
        QVERIFY(splitter.scene().layers[0].keys.contains("x"));
        splitter.undo();
        splitter.undo();
        QCOMPARE(splitter.scene().layers.size(), 1);
    }

    void backendDropsASplitIfTheSceneChanged() {
        if (!Ai::instance().problem().isEmpty())
            QSKIP(qPrintable("The AI isn't set up here: " + Ai::instance().problem()));
        QTemporaryDir dir;
        Backend backend;
        backend.open(ballPicture(dir));
        backend.select(0);
        backend.splitByDepth(3);
        backend.setProperty("x", 10);  // While it works.
        backend.waitForJobs();
        QVERIFY(backend.error().contains("changed"));
        QCOMPARE(backend.scene().layers.size(), 1);
        QCOMPARE(backend.values()["x"].toDouble(), 10.0);
    }

    void bigCutsRunInTheBackground() {
        QTemporaryDir dir;
        QImage picture(3000, 2000, QImage::Format_RGB32);
        picture.fill(QColor(30, 90, 160));
        picture.save(dir.filePath("big.png"));
        Backend backend;
        backend.open(dir.filePath("big.png"));
        backend.select(0);
        backend.setTool("cut");
        for (const QPointF p : {QPointF(100, 100), QPointF(500, 100), QPointF(500, 400), QPointF(100, 400)})
            backend.addCutPoint(p.x(), p.y());
        backend.finishCut(true);
        QCOMPARE(backend.busy(), QString("Cutting…"));
        QCOMPARE(backend.scene().layers.size(), 1);  // Not yet.
        backend.waitForJobs();
        QCOMPARE(backend.error(), QString());
        QCOMPARE(backend.scene().layers.size(), 2);
        QCOMPARE(backend.tool(), QString("move"));
        backend.undo();
        QCOMPARE(backend.scene().layers.size(), 1);
    }

    void findsImagesNothingUses() {
        QTemporaryDir dir;
        const QString project = Project::open(writePicture(dir.filePath("p.png")), nullptr);
        Scene scene = Project::load(project, nullptr);
        AssetCache assets(project);
        QString error;
        Project::cutOut(scene, assets, 0, {{5, 5}, {80, 5}, {80, 60}, {5, 60}}, 0, true, &error, "a");
        const QJsonObject once = scene.toJson();
        Project::reshapeCut(scene, assets, 1, {{5, 5}, {90, 5}, {90, 70}, {5, 70}}, 0, 0, &error);
        // Its first shape's files are only in the history now.
        const QStringList unused = Project::unusedAssets(project, {scene.toJson()});
        QCOMPARE(unused.size(), 2);
        QVERIFY(!unused.contains("assets/p.png"));  // The original, kept as "uncut".
        QVERIFY(Project::unusedAssets(project, {scene.toJson(), once}).isEmpty());
        // Something omotion doesn't know about keeps its file too.
        QJsonObject noted = scene.toJson();
        noted["notes"] = QJsonObject{{"reference", unused.first()}};
        QCOMPARE(Project::unusedAssets(project, {noted}).size(), 1);
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

    void backendRetimesSelectedKeysAsOneUndo() {
        QTemporaryDir dir;
        Backend backend;
        backend.open(writePicture(dir.filePath("p.png")));
        backend.select(0);
        backend.keyAll();
        backend.seek(1);
        backend.setProperty("x", 300);
        backend.selectKey(0, 1, false);
        backend.beginKeyDrag();
        backend.dragKeys(.4);
        backend.dragKeys(9);  // Past the end: held at the last frame.
        backend.endKeyDrag();
        QCOMPARE(backend.layers()[0].toMap()["keyTimes"].toList().last().toDouble(), 4.0);
        QCOMPARE(backend.keySelection().size(), 1);
        QCOMPARE(backend.keySelection()[0].toMap()["t"].toDouble(), 4.0);
        backend.undo();
        QCOMPARE(backend.layers()[0].toMap()["keyTimes"].toList().last().toDouble(), 1.0);
    }

    void backendActsOnPickedKeysOrThePlayhead() {
        QTemporaryDir dir;
        Backend backend;
        backend.open(writePicture(dir.filePath("p.png")));
        backend.select(0);
        backend.keyAll();
        backend.seek(2);
        backend.keyAll();
        backend.seek(3);
        backend.keyAll();
        backend.selectKey(0, 0, false);
        backend.selectKey(0, 2, true);
        backend.cycleEase();
        QCOMPARE(backend.scene().layers[0].keys["x"][0].ease, QString("outBack"));
        QCOMPARE(backend.scene().layers[0].keys["x"][1].ease, QString("outBack"));
        QCOMPARE(backend.scene().layers[0].keys["x"][2].ease, QString("inOut"));

        backend.copyKeys();
        backend.removeKeys();
        QCOMPARE(backend.scene().keyTimes(0), (QList<double>{3}));
        backend.seek(1);
        backend.pasteKeys();
        QCOMPARE(backend.scene().keyTimes(0), (QList<double>{1, 3}));  // 3 replaced by the copy of 2.
        QCOMPARE(backend.scene().layers[0].keys["x"][1].ease, QString("outBack"));

        // With nothing picked, X takes the keys under the playhead.
        backend.clearKeySelection();
        backend.seek(1);
        backend.removeKeys();
        QCOMPARE(backend.scene().keyTimes(0), (QList<double>{3}));
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
