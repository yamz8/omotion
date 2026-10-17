#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QProcess>
#include <QRandomGenerator>
#include <QStandardPaths>
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
#include "steps.h"
#include "film.h"
#include "gltf.h"
#include "physics.h"
#include "theme.h"
#include "world.h"

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

// A project with a made-up world: a 400×200 picture, a piece cut from its
// middle, sky over the top quarter and ground sloping away below; saved, so
// a Backend can open it.
QString madeUpWorld(QTemporaryDir &dir) {
    const QString project = Project::open(writePicture(dir.filePath("p.png"), {400, 200}), nullptr);
    Scene scene = Project::load(project, nullptr);
    AssetCache assets(project);
    QString error;
    Project::cutOut(scene, assets, 0, QPolygonF({QPointF(150, 50), QPointF(260, 50), QPointF(260, 140), QPointF(150, 140)}),
                    0, true, &error, "piece", 0, true, false);
    QImage depth(40, 20, QImage::Format_Grayscale16);
    for (int y = 0; y < 20; ++y)
        for (int x = 0; x < 40; ++x)
            reinterpret_cast<quint16 *>(depth.scanLine(y))[x] = y < 5 ? World::Sky : World::encodeDepth(3 + y * .4 + x * .05);
    QDir(project).mkpath("assets/world");
    depth.save(QDir(project).filePath("assets/world/d.png"));
    scene.raw["world"] = QJsonObject{{"picture", scene.layers[0].name}, {"depth", "assets/world/d.png"},
                                     {"behind", "assets/world/d.png"}, {"focal", 350},
                                     {"ground", QJsonObject{{"up", QJsonArray{.05, -.98, .15}}, {"height", 1.2}}}};
    Project::save(project, scene, nullptr);
    return project;
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
    void initTestCase() {
        // Cuts here fill smoothly, quickly and the same everywhere; the AI's
        // fill has its own test.
        qputenv("OMOTION_FILL", "smooth");
    }

    void easesStartAndEndInPlace() {
        for (const QString &ease : Scene::eases()) {
            QCOMPARE(Scene::ease(ease, 0), 0.0);
            QCOMPARE(Scene::ease(ease, 1), 1.0);
        }
        QCOMPARE(Scene::ease("hold", .99), 0.0);
        QVERIFY(Scene::ease("outBack", .7) > 1);  // It overshoots.
    }

    void customCurvesEase() {
        // A straight curve is linear; the inOut look-alike is close to inOut.
        for (double p : {.1, .3, .5, .8})
            QVERIFY(qAbs(Scene::ease("cubic(0,0,1,1)", p) - p) < 1e-4);
        for (double p : {.2, .5, .8})
            QVERIFY(qAbs(Scene::ease("cubic(0.65,0,0.35,1)", p) - Scene::ease("inOut", p)) < .06);
        // Handles above the top overshoot; ends stay put.
        double most = 0;
        for (double p = 0; p <= 1; p += .01)
            most = std::max(most, Scene::ease("cubic(0.3,1.8,0.6,1)", p));
        QVERIFY(most > 1.1);
        QCOMPARE(Scene::ease("cubic(0.3,1.8,0.6,1)", 1), 1.0);
        QCOMPARE(Scene::ease("cubic(0.3,1.8,0.6,1)", 0), 0.0);
        // Time can't go backwards, and it has to parse.
        QVERIFY(!Scene::validEase("cubic(1.2,0,0.5,1)"));
        QVERIFY(!Scene::validEase("cubic(0.2,0,0.5)"));
        QVERIFY(Scene::validEase(Scene::cubicEase(.25, .1, .25, 1)));
        QCOMPARE(Scene::cubicEase(.25, .1, .25, 1), QString("cubic(0.25,0.1,0.25,1)"));
        QCOMPARE(Scene::curveOf("outBack").size(), 4);
        QString error;
        Scene::fromJson(QJsonDocument::fromJson(R"json({"layers": [{"name": "a", "source": "a", "keys": {"x": [{"t": 0, "v": 0, "ease": "cubic(0.1,0.2,0.3,0.4)"}]}}]})json").object(), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
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

    void physicalEasesSettleBounceAndArc() {
        for (const QString &ease : {QString("spring"), QString("spring(3)"), QString("bounce"), QString("bounce(0)"),
                                    QString("arc(200)")}) {
            QVERIFY2(Scene::validEase(ease), qPrintable(ease));
            QCOMPARE(Scene::ease(ease, 0), 0.0);
            QVERIFY2(std::abs(Scene::ease(ease, 1) - 1) < 1e-9, qPrintable(ease));
            QVERIFY(Scene::curveOf(ease).isEmpty());
        }
        QVERIFY(!Scene::validEase("spring(lots)"));
        QVERIFY(!Scene::validEase("wobble(2)"));
        double highest = 0, lowest = 1;
        int landings = 0;
        double before = 0;
        for (int i = 1; i <= 1000; ++i) {
            const double p = i / 1000.0;
            highest = std::max(highest, Scene::ease("spring(2)", p));
            const double ball = Scene::ease("bounce(3)", p);
            QVERIFY(ball <= 1 + 1e-12);
            lowest = std::min(lowest, p > .5 ? ball : 1.0);
            // Counted where it turns from falling to rising: a landing.
            if (i > 1 && ball < before && before > Scene::ease("bounce(3)", (i - 2) / 1000.0))
                ++landings;
            before = ball;
        }
        QVERIFY(highest > 1.1);  // A spring overshoots...
        QVERIFY(std::abs(Scene::ease("spring(2)", .95) - 1) < .01);  // ...and settles.
        QCOMPARE(landings, 3);
        QVERIFY(lowest < 1);
        // Bounce(0) is a fall from rest: gravity, a square in time.
        QCOMPARE(Scene::ease("bounce(0)", .5), .25);

        // An arc rises on y only: a parabola between the keys, its height at the middle.
        Scene scene;
        Layer layer;
        layer.keys["y"] = {Key{0, 500, "arc(200)"}, Key{2, 500, "linear"}};
        layer.keys["x"] = {Key{0, 0, "arc(200)"}, Key{2, 100, "linear"}};
        scene.layers.append(layer);
        QCOMPARE(scene.value(scene.layers[0], "y", 1), 300.0);
        QCOMPARE(scene.value(scene.layers[0], "y", .5), 500.0 - 150);
        QCOMPARE(scene.value(scene.layers[0], "x", 1), 50.0);
        QCOMPARE(scene.value(scene.layers[0], "y", 2), 500.0);
    }

    void springyLinksLagOvershootAndSettle() {
        QString error;
        const Scene scene = Scene::fromJson(QJsonDocument::fromJson(R"({
            "duration": 6, "fps": 30,
            "layers": [
                {"name": "wheels", "source": "w.png",
                 "keys": {"y": [{"t": 0, "v": 0, "ease": "hold"}, {"t": 1, "v": 100}]}},
                {"name": "body", "source": "b.png",
                 "links": {"y": {"from": "wheels.y", "spring": 2, "damping": 0.3}}},
                {"name": "stiff", "source": "b.png",
                 "links": {"y": {"from": "wheels.y", "spring": 2, "damping": 1.5}}}
            ]
        })").object(), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const Layer &body = scene.layers[1], &stiff = scene.layers[2];
        // Before the jump it rests where it's pulled; just after, it lags.
        QCOMPARE(scene.value(body, "y", .9), 0.0);
        QVERIFY(scene.value(body, "y", 1.05) < 50);
        double highest = 0, stiffest = 0;
        for (double t = 1; t < 3; t += 1 / 60.0) {
            highest = std::max(highest, scene.value(body, "y", t));
            stiffest = std::max(stiffest, scene.value(stiff, "y", t));
        }
        QVERIFY(highest > 120);        // Light damping overshoots...
        QVERIFY(stiffest < 100.5);     // ...heavy damping doesn't.
        QVERIFY(std::abs(scene.value(body, "y", 5) - 100) < 1);  // Both settle.
        QVERIFY(std::abs(scene.value(stiff, "y", 5) - 100) < 1);

        Link link;
        QVERIFY(Scene::parseLink("wheels.y, spring 2, damping 0.3", &link, &error));
        QCOMPARE(link.spring, 2.0);
        QCOMPARE(link.damping, .3);
        QCOMPARE(Scene::linkText(link), QString("wheels.y, spring 2, damping 0.3"));
        QCOMPARE(Scene::linkToJson(link)["spring"].toDouble(), 2.0);
        QVERIFY(!Scene::parseLink("wheels.y, damping 0", &link, &error));
    }

    void linksAddTheirSourceToTheirOwnValue() {
        QString error;
        const Scene scene = Scene::fromJson(QJsonDocument::fromJson(R"({
            "duration": 3,
            "layers": [
                {"name": "car", "source": "car.png",
                 "keys": {"x": [{"t": 0, "v": 0}, {"t": 3, "v": 300}]}},
                {"name": "reversing", "source": "car.png",
                 "keys": {"x": [{"t": 0, "v": 300}, {"t": 3, "v": 0}]}},
                {"name": "wheel", "source": "wheel.png", "rotation": 10,
                 "links": {"rotation": {"from": "car.x", "times": 2},
                           "x": {"from": "car.x", "of": "speed"},
                           "y": {"from": "car.x", "of": "acceleration"},
                           "scale": {"from": "car.x", "lag": 1, "times": 0.01},
                           "opacity": {"from": "reversing.x", "of": "speed", "abs": true, "max": 50},
                           "blur": {"from": "time", "times": 90}}}
            ],
            "camera": {"links": {"x": {"from": "car.x"}}}
        })").object(), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const Layer &wheel = scene.layers[2];
        QCOMPARE(scene.value(wheel, "rotation", 1.5), 10.0 + 300);
        QCOMPARE(scene.ownValue(wheel, "rotation", 1.5), 10.0);
        // 100 px a second all the way, the first and last frames included.
        QCOMPARE(scene.value(wheel, "x", 1.5), 100.0);
        QCOMPARE(scene.value(wheel, "x", 0), 100.0);
        QCOMPARE(scene.value(wheel, "x", 3), 100.0);
        QVERIFY(std::abs(scene.value(wheel, "y", 1.5)) < 1e-6);
        QVERIFY(std::abs(scene.value(wheel, "y", 0)) < 1e-6);  // No jolt at the start.
        // A second behind: at 2 s it reads where the car was at 1 s.
        QCOMPARE(scene.value(wheel, "scale", 2), 1.0 + 1);
        // Backwards at 100 px a second counts as 100, held to the max.
        QCOMPARE(scene.value(wheel, "opacity", 1.5), 1.0 + 50);
        QCOMPARE(scene.value(wheel, "blur", 2), 180.0);
        // The camera starts at the canvas centre and is carried along.
        QCOMPARE(scene.value(scene.camera, "x", 1.5), 640.0 + 150);
    }

    void linksGoThroughJsonAndKeepUnknownFields() {
        QString error;
        const QJsonObject json = QJsonDocument::fromJson(R"({
            "layers": [
                {"name": "car", "source": "car.png"},
                {"name": "wheel", "source": "wheel.png",
                 "links": {"rotation": {"from": "car.x", "of": "speed", "times": 0.5, "note": "spins"}}}
            ]
        })").object();
        Scene scene = Scene::fromJson(json, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const QJsonObject link =
            scene.toJson()["layers"].toArray()[1].toObject()["links"].toObject()["rotation"].toObject();
        QCOMPARE(link["from"].toString(), QString("car.x"));
        QCOMPARE(link["of"].toString(), QString("speed"));
        QCOMPARE(link["times"].toDouble(), .5);
        QCOMPARE(link["note"].toString(), QString("spins"));
        QVERIFY(!link.contains("lag") && !link.contains("abs") && !link.contains("min"));
        // Unlinked layers don't grow an empty "links".
        QVERIFY(!scene.toJson()["layers"].toArray()[0].toObject().contains("links"));
        QVERIFY(!scene.toJson().contains("camera"));
    }

    void brokenLinksAreRefused() {
        auto load = [](const QByteArray &links) {
            QString error;
            Scene::fromJson(QJsonDocument::fromJson(R"({"layers": [{"name": "a", "source": "a.png", "links": )" + links +
                                                    R"(}, {"name": "b", "source": "b.png", "links": {"x": {"from": "a.y"}}}]})")
                                .object(),
                            &error);
            return error;
        };
        QVERIFY(load(R"({"x": {"from": "car.x"}})").contains("isn't there"));
        QVERIFY(load(R"({"x": {"from": "b.nonsense"}})").contains("isn't there"));
        QVERIFY(load(R"({"nonsense": {"from": "b.x"}})").contains("isn't one of its properties"));
        QVERIFY(load(R"({"x": {"from": "b.x", "of": "jerk"}})").contains("\"of\""));
        QVERIFY(load(R"({"x": {"from": "b.x", "min": 2, "max": 1}})").contains("min"));
        QVERIFY(load(R"({"x": {"times": 2}})").contains("where from"));
        // a.y is read by b.x; a.y reading b.x closes the loop.
        QVERIFY(load(R"({"y": {"from": "b.x"}})").contains("loop"));
        QVERIFY(load(R"({"x": {"from": "a.x"}})").contains("loop"));
        QVERIFY(load(R"({"x": {"from": "b.x"}})").isEmpty());

        // Made one at a time, a loop is refused and leaves the scene as it was.
        QString error;
        Scene scene = Scene::fromJson(QJsonDocument::fromJson(
            R"({"layers": [{"name": "a", "source": "a.png"}, {"name": "b", "source": "b.png"}]})").object(), &error);
        QVERIFY(scene.setLink(1, "x", Link{"a.x"}, &error));
        QVERIFY(!scene.setLink(0, "x", Link{"b.x"}, &error));
        QVERIFY(error.contains("loop"));
        QVERIFY(scene.layers[0].links.isEmpty());
        QVERIFY(!scene.setLink(0, "x", Link{"c.x"}, &error));
        QVERIFY(scene.setLink(1, "x", Link{}, &error));
        QVERIFY(scene.layers[1].links.isEmpty());
    }

    void linksReadAndWriteAsText() {
        Link link;
        QString error;
        const QString text = "acceleration of car body.x × -0.5, lag 0.1, smooth 0.2, abs, min -3, max 3";
        QVERIFY2(Scene::parseLink(text, &link, &error), qPrintable(error));
        QCOMPARE(link.from, QString("car body.x"));
        QCOMPARE(link.of, QString("acceleration"));
        QCOMPARE(link.times, -.5);
        QCOMPARE(link.lag, .1);
        QCOMPARE(link.smooth, .2);
        QVERIFY(link.abs);
        QCOMPARE(link.min, -3.0);
        QCOMPARE(link.max, 3.0);
        QCOMPARE(Scene::linkText(link), text);
        // * works as well as ×, and a plain source multiplies by 1.
        QVERIFY(Scene::parseLink("time * 90", &link, &error));
        QCOMPARE(link.from, QString("time"));
        QCOMPARE(link.times, 90.0);
        QVERIFY(Scene::parseLink("car.x", &link, &error));
        QCOMPARE(link.times, 1.0);
        QCOMPARE(Scene::linkText(link), QString("car.x"));
        QVERIFY(!Scene::parseLink("car.x, wobble 3", &link, &error));
        QVERIFY(error.contains("wobble"));
        QVERIFY(!Scene::parseLink("car.x, min 2, max 1", &link, &error));
    }

    void backendLinksWithoutAJump() {
        QTemporaryDir dir;
        Backend backend;
        backend.open(writePicture(dir.filePath("p.png")));
        const QString source = backend.scene().layers[0].name;
        backend.select(0);
        backend.duplicateLayer();
        QCOMPARE(backend.selected(), 1);
        backend.setProperty("rotation", 5);
        // Linking leaves the property where it was; the source moving moves it.
        QVERIFY(backend.setLink("rotation", source + ".x × 0.1"));
        QCOMPARE(backend.values()["rotation"].toDouble(), 5.0);
        QCOMPARE(backend.values()["links"].toMap()["rotation"].toString(), source + ".x × 0.1");
        backend.select(0);
        backend.adjust("x", 50);
        backend.select(1);
        QCOMPARE(backend.values()["rotation"].toDouble(), 10.0);
        // Typing a value shows that value, link and all.
        backend.setProperty("rotation", 30);
        QCOMPARE(backend.values()["rotation"].toDouble(), 30.0);
        // Bad links are refused with a reason.
        QVERIFY(!backend.setLink("rotation", "nobody.x"));
        QVERIFY(backend.error().contains("nobody.x"));
        // Renaming the source keeps the link; deleting it lets go.
        QVERIFY(backend.renameLayer(0, "car"));
        QCOMPARE(backend.scene().layers[1].links["rotation"].from, QString("car.x"));
        backend.select(0);
        backend.deleteLayer();
        QVERIFY(backend.scene().layers[0].links.isEmpty());
        QVERIFY(backend.notice().contains("unlinked"));
        backend.undo();
        QCOMPARE(backend.scene().layers[1].links["rotation"].from, QString("car.x"));
        // An empty link unlinks, again without a jump.
        backend.select(1);
        const double before = backend.values()["rotation"].toDouble();
        QVERIFY(backend.setLink("rotation", ""));
        QVERIFY(backend.scene().layers[1].links.isEmpty());
        QCOMPARE(backend.values()["rotation"].toDouble(), before);
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

    void shutterBlursWhatMovesAsEveryMomentWould() {
        // Drawing what holds still once and only what moves at each moment
        // comes out as averaging whole frames across the shutter does.
        QTemporaryDir dir;
        AssetCache assets;
        Scene scene = twoSquares(dir, assets);
        const QImage still = renderFrame(scene, assets, .5);
        scene.raw["shutter"] = 180;
        QCOMPARE(renderFrame(scene, assets, .5), still);

        for (const double t : {0.0, 1.0}) {
            scene.keyProperty(1, "x", t);
            scene.setValue(1, "x", t, 100 * t);
        }
        auto everyMoment = [&] {
            Scene plain = scene;
            plain.raw.remove("shutter");
            std::vector<int> sum(100 * 100 * 4, 0);
            for (int i = 0; i < 12; ++i) {
                const QImage moment = renderFrame(plain, assets, .5 + 1.0 / 60 * (i / 11.0 - .5));
                for (int y = 0; y < 100; ++y)
                    for (int x = 0; x < 400; ++x)
                        sum[y * 400 + x] += moment.constScanLine(y)[x];
            }
            QImage average(100, 100, QImage::Format_ARGB32_Premultiplied);
            for (int y = 0; y < 100; ++y)
                for (int x = 0; x < 400; ++x)
                    average.scanLine(y)[x] = uchar((2 * sum[y * 400 + x] + 12) / 24);
            return average;
        };
        auto mostApart = [](const QImage &a, const QImage &b) {
            int most = 0;
            for (int y = 0; y < a.height(); ++y)
                for (int x = 0; x < a.width() * 4; ++x)
                    most = std::max(most, std::abs(a.constScanLine(y)[x] - b.constScanLine(y)[x]));
            return most;
        };
        const QImage blurred = renderFrame(scene, assets, .5);
        QVERIFY(blurred != still);
        QVERIFY(mostApart(blurred, everyMoment()) <= 1);
        // Blends mix with what's below, so then whole moments are drawn.
        scene.layers[1].blend = "add";
        QVERIFY(mostApart(renderFrame(scene, assets, .5), everyMoment()) <= 1);
        // And so is the bottom layer moving, over a see-through background.
        scene.layers[1].blend.clear();
        scene.background = QColor(0, 0, 0, 100);
        std::swap(scene.layers[0], scene.layers[1]);
        QVERIFY(mostApart(renderFrame(scene, assets, .5), everyMoment()) <= 1);
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

    void oddSizedLooksDrawSmallInPlace() {
        // An odd width halves to a rounded size; a look made from that copy
        // once came out of the full picture and was drawn twice as big.
        QTemporaryDir dir;
        const QString project = Project::open(writePicture(dir.filePath("p.png"), {801, 401}), nullptr);
        Scene scene = Project::load(project, nullptr);
        AssetCache assets(project);
        double factor = 0, again = 0;
        assets.mip("assets/p.png", .3, &factor);
        QVERIFY(factor < 1);
        assets.mip("assets/p.png", factor, &again);
        QCOMPARE(again, factor);
        scene.layers[0].base["scale"] = .3;
        const QImage plain = renderFrame(scene, assets, 0);
        scene.layers[0].base["blur"] = .5;
        const QImage blurred = renderFrame(scene, assets, 0);
        // A touch of blur moves edges a pixel, not the whole picture.
        QVERIFY(differences(plain, blurred) < plain.width() * plain.height() / 50);
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

    void piecesCutBeforeASplitGoWithTheirDepth() {
        QTemporaryDir dir;
        QImage depth;
        const QString path = bandedPicture(dir, &depth);
        // Something standing in the middle band.
        QImage picture(path);
        QPainter painter(&picture);
        painter.fillRect(140, 60, 20, 30, QColor(250, 250, 80));
        painter.end();
        picture.save(path);
        const QString project = Project::open(path, nullptr);
        Scene scene = Project::load(project, nullptr);
        AssetCache assets(project);
        const QImage before = renderFrame(scene, assets, 0);
        QString error;
        QCOMPARE(Project::cutOut(scene, assets, 0, {{138, 58}, {162, 58}, {162, 92}, {138, 92}}, 0, true, &error, "post", 0, true), 1);
        scene.keyAll(1, 0);  // It has its own animation, which should survive.
        const QList<int> created = Project::splitByDepth(scene, assets, 0, depth, 3, 0, &error);
        QVERIFY2(created.size() == 2, qPrintable(error));
        const int post = scene.indexOf("post");
        QCOMPARE(scene.layers[post].parent, QString("middle ground"));
        QCOMPARE(post, scene.indexOf("middle ground") + 1);  // Right above its layer.
        QVERIFY(scene.indexOf("foreground") > post);
        QVERIFY(differences(renderFrame(scene, assets, 0), before) < 300 * 8);
        // Still where it was, and it moves with its layer.
        const QPointF at = layerTransform(scene, assets, post, 0).map(QPointF(0, 0));
        QVERIFY2(QLineF(at, QPointF(138, 58)).length() < 2, qPrintable(QString("%1,%2").arg(at.x()).arg(at.y())));
        QVERIFY(Project::animateParallax(scene, assets, 0, 30, &error));
        // Seen from its layer, it doesn't move: the camera carries both.
        auto onLayer = [&](double t) {
            return layerTransform(scene, assets, scene.indexOf("middle ground"), t).inverted()
                .map(layerTransform(scene, assets, post, t).map(QPointF(0, 0)));
        };
        QVERIFY(QLineF(onLayer(0), onLayer(scene.duration / 2)).length() < .01);
        // While the layers in front move further than it does.
        auto canvasX = [&](int layer, double t) { return layerTransform(scene, assets, layer, t).map(QPointF(150, 0)).x(); };
        const int near = scene.indexOf("foreground");
        QVERIFY(canvasX(near, scene.duration / 2) - canvasX(near, 0) < canvasX(post, scene.duration / 2) - canvasX(post, 0));
        QVERIFY(!scene.layers[post].keys.value("x").isEmpty());
    }

    void splittingKeepsPieceFamiliesTogether() {
        QTemporaryDir dir;
        QImage depth;
        const QString project = Project::open(bandedPicture(dir, &depth), nullptr);
        Scene scene = Project::load(project, nullptr);
        AssetCache assets(project);
        QString error;
        QCOMPARE(Project::cutOut(scene, assets, 0, {{138, 58}, {162, 58}, {162, 92}, {138, 92}}, 0, true, &error, "post", 0, true), 1);
        QVERIFY(Project::cutOut(scene, assets, 1, {{140, 60}, {150, 60}, {150, 70}, {140, 70}}, 0, true, &error, "sign", 0, true) > 0);
        // The post sits below its source in the stack, under something else too.
        Layer post = scene.layers.takeAt(scene.indexOf("post"));
        scene.layers.prepend(post);
        Layer other = scene.layers.at(scene.indexOf("bands"));
        other.name = "other";
        other.raw = QJsonObject();
        other.source = "assets/bands.png";
        scene.layers.append(other);
        const QImage before = renderFrame(scene, assets, 0);
        QVERIFY2(!Project::splitByDepth(scene, assets, scene.indexOf("bands"), depth, 3, 0, &error).isEmpty(), qPrintable(error));
        QCOMPARE(scene.layers[scene.indexOf("post")].parent, QString("middle ground"));
        QCOMPARE(scene.indexOf("post"), scene.indexOf("middle ground") + 1);
        QCOMPARE(scene.indexOf("sign"), scene.indexOf("post") + 1);  // Its own piece came along.
        QVERIFY(scene.indexOf("other") > scene.indexOf("foreground"));
        QVERIFY(differences(renderFrame(scene, assets, 0), before) < 300 * 8);
    }

    void lengtheningForParallaxStretchesLoops() {
        QTemporaryDir dir;
        QImage depth;
        const QString project = Project::open(bandedPicture(dir, &depth), nullptr);
        Scene scene = Project::load(project, nullptr);
        AssetCache assets(project);
        QString error;
        Project::cutOut(scene, assets, 0, {{138, 58}, {162, 58}, {162, 92}, {138, 92}}, 0, true, &error, "post", 0, true);
        QCOMPARE(scene.duration, 4.0);
        QVERIFY(Project::applyPreset(scene, assets, 1, "sway", 0, 1, &error));
        Project::splitByDepth(scene, assets, 0, depth, 3, 0, &error);
        QVERIFY(Project::animateParallax(scene, assets, 0, 30, &error));
        QCOMPARE(scene.duration, 10.0);
        const Layer &swaying = scene.layers[scene.indexOf("post")];
        QCOMPARE(swaying.keys["rotation"].last().t, 10.0);  // Still swaying to the end...
        QVERIFY(qAbs(scene.value(swaying, "rotation", 2.5)) > 3);  // ...at the stretched quarter.
    }

    void theCameraMovesWhatFollowsIt() {
        QTemporaryDir dir;
        AssetCache assets;
        Scene scene = twoSquares(dir, assets);  // 100×100 canvas, squares 40×40 at the centre.
        QCOMPARE(scene.toJson().contains("camera"), false);  // At rest it isn't written.
        scene.camera.base["x"] = 60;  // Look 10 px right of centre...
        scene.camera.base["zoom"] = 2;  // ...twice as close.
        scene.layers[0].follow = 0;     // The bottom one is stuck to the screen.
        scene.layers[1].follow = .5;
        const QPointF fixed = layerTransform(scene, assets, 0, 0).map(QPointF(20, 20));
        QCOMPARE(fixed, QPointF(50, 50));  // Didn't move.
        // Half-following: half the pan (5) and half the extra zoom (1.5).
        const QPointF half = layerTransform(scene, assets, 1, 0).map(QPointF(20, 20));
        QVERIFY(QLineF(half, QPointF(50 + (50 - 55) * 1.5, 50)).length() < 1e-9);
        // An unset follow is the parent's, and a full one the whole camera.
        scene.layers[1].follow = std::numeric_limits<double>::quiet_NaN();
        scene.layers[1].parent = "under";
        QCOMPARE(followOf(scene, 1), 0.0);
        scene.layers[1].parent.clear();
        QCOMPARE(followOf(scene, 1), 1.0);
        QCOMPARE(layerTransform(scene, assets, 1, 0).map(QPointF(20, 20)), QPointF(50 + (50 - 60) * 2, 50));

        // Through JSON and back, with keys.
        scene.camera.keys["rotation"] = {Key{0, 0, "inOut"}, Key{2, 30, "out"}};
        QString error;
        const Scene again = Scene::fromJson(scene.toJson(), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(again.camera.base["zoom"], 2.0);
        QCOMPARE(again.camera.keys["rotation"].size(), 2);
        QCOMPARE(again.layers[0].follow, 0.0);
        QVERIFY(std::isnan(again.layers[1].follow));
        QJsonObject broken = scene.toJson();
        broken["camera"] = QJsonObject{{"keys", QJsonObject{{"scale", QJsonArray{}}}}};
        Scene::fromJson(broken, &error);
        QVERIFY(error.contains("camera"));
    }

    void particlesLoopAndKeepTheirSettings() {
        QTemporaryDir dir;
        const QString project = Project::open(writePicture(dir.filePath("p.png")), nullptr);
        Scene scene = Project::load(project, nullptr);
        AssetCache assets(project);
        QString error;
        for (const QString &kind : Scene::particleKinds())
            QVERIFY2(Project::addParticles(scene, kind, {{"count", 60}}, &error) > 0, qPrintable(error));
        QVERIFY(Project::addParticles(scene, "hail", {}, &error) < 0);
        // Exactly the same at the end as at the start, and moving between.
        const QImage start = renderFrame(scene, assets, 0), end = renderFrame(scene, assets, scene.duration);
        QCOMPARE(differences(start, end), 0);
        QVERIFY(differences(start, renderFrame(scene, assets, scene.duration / 3)) > 50);
        // The same seed draws the same; another seed, others.
        QCOMPARE(differences(renderFrame(scene, assets, 1), renderFrame(scene, assets, 1)), 0);
        Scene reseeded = scene;
        reseeded.layers[1].particles["seed"] = 7;
        QVERIFY(differences(renderFrame(reseeded, assets, 1), renderFrame(scene, assets, 1)) > 20);
        // Through JSON and back: no source needed, settings kept.
        const Scene again = Scene::fromJson(scene.toJson(), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(again.layers[1].particles["kind"].toString(), QString("snow"));
        QCOMPARE(again.layers[1].particles["count"].toInt(), 60);
        QVERIFY(!scene.toJson()["layers"].toArray()[1].toObject().contains("source"));
        QCOMPARE(scene.layers[scene.indexOf("fireflies")].blend, QString("add"));
        QJsonObject bad = scene.toJson();
        QJsonArray layers = bad["layers"].toArray();
        QJsonObject odd = layers[1].toObject();
        odd["particles"] = QJsonObject{{"kind", "hail"}};
        layers[1] = odd;
        bad["layers"] = layers;
        Scene::fromJson(bad, &error);
        QVERIFY(error.contains("hail"));
        // Nothing to click on: they don't catch the mouse.
        Scene onlySnow = scene;
        onlySnow.layers.resize(2);
        onlySnow.layers[0].visible = false;
        QCOMPARE(layerAt(onlySnow, assets, 0, QPointF(100, 50)), -1);
    }

    void pinsBendRigidly() {
        Scene scene;
        Layer layer;
        layer.pins = {QPointF(0, 0), QPointF(0, 100)};
        scene.layers.append(layer);
        // Unmoved pins leave everything where it is.
        QVERIFY(QLineF(bend(scene, layer, QPointF(30, 40), 0), QPointF(30, 40)).length() < 1e-6);
        QVERIFY(!isBent(scene, layer, 0));
        // Both pins moved together carry the image along.
        layer.base["pin0x"] = 10;
        layer.base["pin1x"] = 10;
        QVERIFY(QLineF(bend(scene, layer, QPointF(30, 40), 0), QPointF(40, 40)).length() < 1e-6);
        // Swing the bottom pin round a quarter turn: the image turns, and
        // keeps its size (a point 50 below the top stays 50 from it).
        layer.base["pin0x"] = 0;
        layer.base["pin1x"] = 100;
        layer.base["pin1y"] = -100;
        QVERIFY(isBent(scene, layer, 0));
        const QPointF middle = bend(scene, layer, QPointF(0, 50), 0);
        QVERIFY2(qAbs(QLineF(middle, QPointF(0, 0)).length() - 50) < 1, qPrintable(QString("%1,%2").arg(middle.x()).arg(middle.y())));
        QCOMPARE(bend(scene, layer, QPointF(0, 100), 0), QPointF(100, 0));  // A pin goes exactly where it's put.
        // One pin just carries it.
        Layer single;
        single.pins = {QPointF(5, 5)};
        single.base["pin0y"] = 7;
        QCOMPARE(bend(scene, single, QPointF(50, 50), 0), QPointF(50, 57));
    }

    void pinsGoThroughJson() {
        QString error;
        const Scene scene = Scene::fromJson(QJsonDocument::fromJson(R"({"layers": [{"name": "arm", "source": "a.png",
            "pins": [[0, 0], [0, 100]], "pin1x": 20, "keys": {"pin1y": [{"t": 0, "v": 0}, {"t": 1, "v": -30}]}}]})").object(), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(scene.layers[0].pins.size(), 2);
        QCOMPARE(scene.value(scene.layers[0], "pin1x", 0), 20.0);
        QCOMPARE(scene.value(scene.layers[0], "pin1y", 1), -30.0);
        const QJsonObject out = scene.toJson()["layers"].toArray()[0].toObject();
        QCOMPARE(out["pin1x"].toDouble(), 20.0);
        QVERIFY(!out.contains("pin0x"));  // Zero offsets aren't written.
        Scene::fromJson(QJsonDocument::fromJson(R"({"layers": [{"name": "arm", "source": "a.png", "pins": [[0, 0]],
            "keys": {"pin3x": [{"t": 0, "v": 1}]}}]})").object(), &error);
        QVERIFY(error.contains("pin3x"));
    }

    void bentLayersLookWhole() {
        QTemporaryDir dir;
        AssetCache assets;
        Scene scene = twoSquares(dir, assets);  // A red square over a blue one, 40×40, on a 100×100 canvas.
        scene.layers.removeFirst();
        Layer &red = scene.layers[0];
        red.base["opacity"] = .5;
        red.pins = {QPointF(0, 20), QPointF(40, 20)};
        red.base["pin1y"] = 3;  // A slight bend.
        const QImage bent = renderFrame(scene, assets, 0);
        // Half see-through all over: no darker seams where triangles meet.
        QColor reference = bent.pixelColor(50, 50);
        int seams = 0;
        for (int y = 36; y < 62; ++y)
            for (int x = 36; x < 62; ++x)
                seams += std::abs(bent.pixelColor(x, y).red() - reference.red()) > 6;
        QCOMPARE(seams, 0);
        // A glow still has its halo when bent.
        red.base["opacity"] = 1;
        red.base["glow"] = 8;
        red.base["pin1y"] = 0;
        const QImage straight = renderFrame(scene, assets, 0);
        red.base["pin1y"] = .01;
        const QImage barely = renderFrame(scene, assets, 0);
        QVERIFY2(differences(straight, barely) < 40, qPrintable(QString::number(differences(straight, barely))));
        QVERIFY(barely.pixelColor(27, 50).red() > 10);  // Halo outside the square.
        // Clicks find it where it's bent to.
        red.base["pin1y"] = 25;
        QCOMPARE(layerAt(scene, assets, 0, QPointF(68, 70)), 0);
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

    void lensComesBackFromShiftedPoints() {
        // A made-up scene seen through an 800 px lens, handed over as the
        // model would: shifted along the view and at a third of its size.
        const QSize picture(960, 540);
        const double focal = 800, shift = 2.5, scale = 3;
        Ai::Geometry g;
        g.width = 96;
        g.height = 54;
        const double diagonal = std::hypot(picture.width(), picture.height());
        const double aspect = double(g.width) / g.height;
        const double sx = aspect / std::sqrt(1 + aspect * aspect), sy = 1 / std::sqrt(1 + aspect * aspect);
        const double f = 2 * focal / diagonal;
        std::vector<double> truth;
        for (int row = 0; row < g.height; ++row)
            for (int col = 0; col < g.width; ++col) {
                const double u = sx * (2 * (col + .5) / g.width - 1), v = sy * (2 * (row + .5) / g.height - 1);
                const double z = 5 + 3 * std::sin(col / 10.0) + row * .1;
                const double point[3] = {u / f * z, v / f * z, z};
                for (int c = 0; c < 3; ++c) {
                    truth.push_back(point[c]);
                    g.points.push_back(float((point[c] - (c == 2 ? shift : 0)) / scale));
                }
                g.normals.insert(g.normals.end(), {0, 0, -1});
                g.valid.push_back(row > 3);  // A strip of sky at the top.
            }
        QVERIFY(Ai::solveLens(g, scale, picture));
        QVERIFY2(std::abs(g.focal - focal) < 2, qPrintable(QString::number(g.focal)));
        for (size_t i = 0; i < truth.size(); i += 97)
            QVERIFY(std::abs(g.points[i] - truth[i]) < .01);
        // Nothing valid, nothing to solve.
        std::fill(g.valid.begin(), g.valid.end(), 0);
        QVERIFY(!Ai::solveLens(g, scale, picture));
    }

    void depthCodesKeepCentimetresAndKilometres() {
        for (const double meters : {.05, .5, 4.37, 61.0, 1500.0}) {
            const double back = World::decodeDepth(World::encodeDepth(meters));
            QVERIFY2(std::abs(back - meters) / meters < 2e-4, qPrintable(QString::number(meters)));
        }
        QCOMPARE(World::encodeDepth(1e9), World::Sky);
        QCOMPARE(World::decodeDepth(World::Sky), World::Far);
        // Holes fill from around them: between 4 m and 8 m, in between.
        QImage depth(40, 10, QImage::Format_Grayscale16), hole(40, 10, QImage::Format_Grayscale8);
        hole.fill(0);
        for (int y = 0; y < 10; ++y)
            for (int x = 0; x < 40; ++x) {
                reinterpret_cast<quint16 *>(depth.scanLine(y))[x] = World::encodeDepth(x < 20 ? 4 : 8);
                if (x >= 15 && x < 25)
                    hole.scanLine(y)[x] = 255;
            }
        World::fillDepth(depth, hole);
        const double middle = World::decodeDepth(reinterpret_cast<const quint16 *>(depth.constScanLine(5))[20]);
        QVERIFY2(middle > 4.2 && middle < 7.8, qPrintable(QString::number(middle)));
        QCOMPARE(World::decodeDepth(reinterpret_cast<const quint16 *>(depth.constScanLine(5))[2]), World::decodeDepth(World::encodeDepth(4)));
    }

    void worldAtRestLooksLikeThePicture() {
        // A picture, a piece cut from it, and a made-up world: every mesh
        // point, seen through the world's camera at rest, lands back on its
        // own pixel of the picture.
        QTemporaryDir dir;
        const QString project = Project::open(writePicture(dir.filePath("p.png"), {400, 200}), nullptr);
        Scene scene = Project::load(project, nullptr);
        AssetCache assets(project);
        QString error;
        QVERIFY(Project::cutOut(scene, assets, 0, QPolygonF({QPointF(150, 50), QPointF(260, 50), QPointF(260, 140), QPointF(150, 140)}),
                                0, true, &error, "piece", 0, true, false) >= 0);
        QImage depth(40, 20, QImage::Format_Grayscale16);
        for (int y = 0; y < 20; ++y)
            for (int x = 0; x < 40; ++x)
                reinterpret_cast<quint16 *>(depth.scanLine(y))[x] = y < 5 ? World::Sky : World::encodeDepth(3 + y * .4 + x * .05);
        QDir(project).mkpath("assets/world");
        QVERIFY(depth.save(QDir(project).filePath("assets/world/d.png")));
        scene.raw["world"] = QJsonObject{{"picture", scene.layers[0].name}, {"depth", "assets/world/d.png"},
                                         {"behind", "assets/world/d.png"}, {"focal", 350},
                                         {"ground", QJsonObject{{"up", QJsonArray{.05, -.98, .15}}, {"height", 1.2}}}};
        QVERIFY(World::info(scene).valid);
        QCOMPARE(World::layers(scene).size(), 2);
        const World::Frame frame = World::frame(scene, assets, 0);
        QMatrix4x4 view;
        view.rotate(frame.cameraRotation.conjugated());
        view.translate(-frame.cameraPosition);
        const double focal = 200 / 2 / std::tan(frame.fov / 2 * M_PI / 180);  // The picture is 200 tall.
        for (const int layer : World::layers(scene)) {
            const World::Mesh mesh = World::mesh(scene, assets, layer);
            QVERIFY(!mesh.isNull());
            const QPointF offset = World::offsetInPicture(scene, layer);
            const QSizeF size = assets.size(scene.layers[layer].source);
            const float *v = reinterpret_cast<const float *>(mesh.vertices.constData());
            const int count = int(mesh.vertices.size() / (5 * sizeof(float)));
            for (int i = 0; i < count; i += 37, v += 5 * 37) {
                const QVector3D seen = view.map(QVector3D(v[0], v[1], v[2]));
                if (-seen.z() > 1000)
                    continue;  // The sky: there, but too far to measure.
                const QPointF pixel(200 + focal * seen.x() / -seen.z(), 100 - focal * seen.y() / -seen.z());
                const QPointF expected = offset + QPointF(v[3] * size.width(), (1 - v[4]) * size.height());
                QVERIFY2(QLineF(pixel, expected).length() < .5,
                         qPrintable(QString("%1,%2 vs %3,%4").arg(pixel.x()).arg(pixel.y()).arg(expected.x()).arg(expected.y())));
            }
        }
        // The camera stands at the ground's height; moving it moves the frame.
        QVERIFY(std::abs(frame.cameraPosition.y() - 1.2f) < 1e-4);
        scene.camera.base["worldZ"] = 2;
        QVERIFY(std::abs(World::frame(scene, assets, 0).cameraPosition.z() + 2) < 1e-4);
        // Moving a piece in the world moves it, turning turns it about its pivot.
        const int piece = scene.indexOf("piece");
        scene.layers[piece].base["worldX"] = 1;
        scene.layers[piece].base["yaw"] = 90;
        const World::Frame moved = World::frame(scene, assets, 0);
        const World::Placement &place = moved.layers.at(World::layers(scene).indexOf(piece));
        const QVector3D pivot = World::pivot(scene, assets, piece);
        QVERIFY((place.position + place.rotation.rotatedVector(pivot) - (pivot + QVector3D(1, 0, 0))).length() < 1e-4);
        QVERIFY(std::abs(place.rotation.rotatedVector(QVector3D(1, 0, 0)).z() - 1) < 1e-4);
        // A world that holds still doesn't move on screen; a moving camera
        // moves it, further the longer it moves.
        QCOMPARE(World::motion(scene, assets, 0, .1, QSize(400, 200)), 0.0);
        for (const double t : {0.0, 1.0}) {
            scene.keyProperty(Scene::CameraIndex, "worldX", t);
            scene.setValue(Scene::CameraIndex, "worldX", t, t);
        }
        const double brief = World::motion(scene, assets, .5, .51, QSize(400, 200));
        QVERIFY(brief > .5);
        QVERIFY(World::motion(scene, assets, .5, .53, QSize(400, 200)) > 2 * brief);
    }

    void piecesArePickedWhereTheyAreInTheWorld() {
        QTemporaryDir dir;
        const QString project = madeUpWorld(dir);
        Scene scene = Project::load(project, nullptr);
        AssetCache assets(project);
        const int piece = scene.indexOf("piece");
        QVERIFY(piece > 0);
        const QSizeF size(400, 200);
        const World::View shot = World::cameraView(World::frame(scene, assets, 0));
        // A ray through a point of the view comes back to it.
        QVector3D origin, direction;
        World::ray(shot, size, QPointF(123, 45), &origin, &direction);
        QPointF back;
        QVERIFY(World::project(shot, size, origin + direction * 7, &back));
        QVERIFY(QLineF(back, QPointF(123, 45)).length() < 1e-3);
        QVERIFY(!World::project(shot, size, origin - direction * 7, &back));
        // At rest the world is the picture: the piece where it was cut, the
        // picture around it.
        QCOMPARE(World::pick(scene, assets, 0, shot, size, QPointF(200, 100)), piece);
        QCOMPARE(World::pick(scene, assets, 0, shot, size, QPointF(60, 150)), 0);
        // Moved a meter and a half to the right, it's picked there, and the
        // filled-in picture where it was.
        scene.layers[piece].base["worldX"] = 1.5;
        QList<QVector3D> corners;
        QVector3D pivot;
        QVERIFY(World::box(scene, assets, piece, 0, &corners, &pivot));
        QCOMPARE(corners.size(), 8);
        QPointF at;
        QVERIFY(World::project(shot, size, pivot, &at));
        QVERIFY(at.x() > 260);
        QCOMPARE(World::pick(scene, assets, 0, shot, size, at), piece);
        QCOMPARE(World::pick(scene, assets, 0, shot, size, QPointF(170, 100)), 0);
    }

    void handlesMovePiecesAlongAnAxisAndTurnThem() {
        QTemporaryDir dir;
        Backend backend;
        QVERIFY(backend.open(madeUpWorld(dir)));
        const int piece = backend.scene().indexOf("piece");
        // Clicking it in the view picks it, in 3D.
        backend.pickAt(200, 100);
        QCOMPARE(backend.selected(), piece);
        const Backend::Gizmo gizmo = backend.gizmo();
        QCOMPARE(gizmo.corners.size(), 8);
        QCOMPARE(gizmo.arrows.size(), 3);
        // The x arrow's tip is its handle; dragging along it moves only x.
        const QPointF tip = gizmo.arrows.at(0);
        QCOMPARE(backend.handleAt(tip.x(), tip.y(), 6), 1);
        backend.beginHandle(1, tip.x(), tip.y());
        const QPointF along = tip - gizmo.pivot;
        backend.handleTo(tip.x() + along.x(), tip.y() + along.y() + 30);
        backend.endHandle(true);
        const Layer &layer = backend.scene().layers.at(piece);
        QVERIFY(layer.base.value("worldX") > .05);
        QCOMPARE(layer.base.value("worldY"), 0.0);
        QCOMPARE(layer.base.value("worldZ"), 0.0);
        QVERIFY(backend.canUndo());
        // A grab kept to y moves only y; cancelled, it puts it back with
        // nothing to undo.
        const double x = layer.base.value("worldX");
        backend.undo();
        QVERIFY(backend.scene().layers.at(piece).base.value("worldX") < 1e-9);
        backend.redo();
        backend.beginHandle(0, 200, 100);
        backend.setHandleAxis(2);
        QCOMPARE(backend.handle(), 2);
        backend.handleTo(240, 60);
        QVERIFY(backend.scene().layers.at(piece).base.value("worldY") > .05);
        QCOMPARE(backend.scene().layers.at(piece).base.value("worldX"), x);
        backend.endHandle(false);
        QCOMPARE(backend.scene().layers.at(piece).base.value("worldY"), 0.0);
        QCOMPARE(backend.handle(), -1);
        // The ring turns it.
        const Backend::Gizmo again = backend.gizmo();
        const QPointF ring = again.ring.at(again.ring.size() / 4);
        backend.beginHandle(4, ring.x(), ring.y());
        backend.handleTo(again.pivot.x() + (ring - again.pivot).y(), again.pivot.y() - (ring - again.pivot).x());
        backend.endHandle(true);
        QVERIFY(std::abs(backend.scene().layers.at(piece).base.value("yaw")) > 10);
    }

    void theFreeViewLooksAroundWithoutMovingTheShot() {
        QTemporaryDir dir;
        Backend backend;
        QVERIFY(backend.open(madeUpWorld(dir)));
        const QJsonObject before = backend.scene().toJson();
        const World::View shot = backend.view();
        backend.setViewportSize(800, 400);
        backend.toggleFreeView();
        QVERIFY(backend.freeView());
        QCOMPARE(backend.viewSize(), QSizeF(800, 400));
        // It starts looking the way the camera does, from where it is.
        const World::View free = backend.view();
        QVERIFY((free.position - shot.position).length() < 1e-3);
        const QVector3D ahead = shot.rotation.rotatedVector(QVector3D(0, 0, -1));
        QVERIFY(QVector3D::dotProduct(free.rotation.rotatedVector(QVector3D(0, 0, -1)), ahead) > .999);
        // Orbiting, panning and coming closer move only the view.
        backend.orbitView(120, -40);
        backend.panView(30, 10);
        backend.zoomView(.5);
        QVERIFY((backend.view().position - shot.position).length() > .5);
        QCOMPARE(backend.scene().toJson(), before);
        QVERIFY(!backend.canUndo());
        // The shot's camera shows in it, and can be selected and moved.
        QCOMPARE(backend.gizmo().camera.size(), 5);
        backend.selectCamera();
        QCOMPARE(backend.gizmo().arrows.size(), 3);
        backend.toggleFreeView();
        QVERIFY(!backend.freeView());
        QVERIFY((backend.view().position - shot.position).length() < 1e-3);
    }

    void outlinerGroupsTheWorldAndItsPieces() {
        QTemporaryDir dir;
        Backend backend;
        QVERIFY(backend.open(madeUpWorld(dir)));
        backend.addParticles("snow");
        QStringList seen;
        for (const QVariant &row : backend.outliner()) {
            const QVariantMap map = row.toMap();
            seen << QString("%1 %2 %3").arg(map["kind"].toString(), map["name"].toString()).arg(map["depth"].toInt());
        }
        const QString picture = backend.scene().layers.at(0).name;
        QCOMPARE(seen, QStringList({"camera Camera 0", "world World 0", "picture " + picture + " 1", "piece piece 2",
                                    "particles " + backend.scene().layers.last().name + " 0"}));
        // The world's look is set from there, and undone like any edit.
        QCOMPARE(backend.worldInfo()["shutter"].toDouble(), 180.0);
        backend.setWorldValue("shutter", 90);
        backend.setWorldValue("hazeColor", "#102030");
        backend.setWorldValue("dust", "not a colour");
        QVERIFY(!backend.error().isEmpty());
        QCOMPARE(backend.worldInfo()["shutter"].toDouble(), 90.0);
        QCOMPARE(World::info(backend.scene()).haze, QColor("#102030"));
        backend.undo();
        QCOMPARE(backend.worldInfo()["hazeColor"].toString(), World::Info().haze.name());
        QCOMPARE(backend.worldInfo()["shutter"].toDouble(), 90.0);
    }

    void aiStepsKnowWhenTheyreOutOfDate() {
        QTemporaryDir dir;
        const QString project = madeUpWorld(dir);
        Scene scene = Project::load(project, nullptr);
        AssetCache assets(project);
        const int piece = scene.indexOf("piece"), root = 0;
        auto names = [&](int index) {
            QStringList list;
            for (const Steps::Step &step : Steps::of(scene, assets, index))
                list << step.name + (step.stale ? "!" : "");
            return list;
        };
        // A world made before worlds kept track counts as up to date.
        QCOMPARE(names(piece), QStringList({"cut", "fill", "world", "shape"}));
        QCOMPARE(names(root), QStringList({"world"}));
        QVERIFY(Steps::stale(scene, assets).isEmpty());
        // One that knows what it was made from, and a shape fitted in it.
        QJsonObject world = scene.raw["world"].toObject();
        world["pieces"] = World::madeFrom(scene, root);
        world["made"] = 1;
        scene.raw["world"] = world;
        scene.layers[piece].raw["shape"] = QJsonObject{{"file", "x"}, {"from", World::cutCode(scene.layers[piece])}, {"world", 1}};
        QVERIFY(Steps::stale(scene, assets).isEmpty());
        // Filling the hole another way changes neither.
        QString error;
        QVERIFY2(Steps::run(scene, assets, piece, "fill", "none", &error), qPrintable(error));
        QCOMPARE(scene.layers[piece].raw["cut"].toObject()["fill"].toBool(), false);
        QVERIFY(Steps::stale(scene, assets).isEmpty());
        QVERIFY(Steps::run(scene, assets, piece, "fill", "smooth", &error));
        QCOMPARE(scene.layers[piece].raw["cut"].toObject()["fillWith"].toString(), QString("smooth"));
        QVERIFY(!Steps::run(scene, assets, piece, "fill", "paint", &error));
        // Reshaping the piece puts the world out of date, and the shape too,
        // in that order.
        QVERIFY(Project::reshapeCut(scene, assets, piece, QPolygonF({QPointF(150, 50), QPointF(240, 50), QPointF(240, 140), QPointF(150, 140)}),
                                    0, 0, &error));
        QCOMPARE(names(piece), QStringList({"cut", "fill", "world!", "shape!"}));
        const auto todo = Steps::stale(scene, assets);
        QCOMPARE(todo.size(), 2);
        QCOMPARE(todo.at(0), qMakePair(root, QString("world")));
        QCOMPARE(todo.at(1), qMakePair(piece, QString("shape")));
        // A picture with no world yet offers to make one.
        scene.raw.remove("world");
        QCOMPARE(names(root), QStringList({"world"}));
        QVERIFY(!Steps::of(scene, assets, root).first().done);
        QCOMPARE(Steps::of(scene, assets, root).first().actions, QStringList({"make"}));
    }

    void graphEditorShowsCurvesAndMovesKeys() {
        QTemporaryDir dir;
        Backend backend;
        QVERIFY(backend.open(writePicture(dir.filePath("p.png"))));
        backend.select(0);
        QVERIFY(backend.curves().isEmpty());
        backend.keyProperty("x");
        backend.seek(1);
        backend.setProperty("x", 300);
        backend.keyProperty("opacity");
        QCOMPARE(backend.curveNames(), QStringList({"x", "opacity"}));
        const Backend::Curve x = backend.curves(10).first();
        QCOMPARE(x.keys.size(), 2);
        QCOMPARE(x.samples.size(), 11);
        QCOMPARE(x.high, 300.0);
        // A key dragged later and higher, as one undo step; it can't pass
        // its neighbour.
        backend.beginGesture();
        backend.moveKey("x", 0, .5, 50);
        backend.moveKey("x", 0, 2.5, 80);
        backend.endGesture();
        const QList<Key> keys = backend.scene().layers.at(0).keys.value("x");
        QCOMPARE(keys.at(0).v, 80.0);
        QVERIFY(keys.at(0).t < keys.at(1).t);
        QVERIFY(std::abs(keys.at(0).t - (1 - 1.0 / 30)) < 1e-9);
        backend.undo();
        QCOMPARE(backend.scene().layers.at(0).keys.value("x").at(0).t, 0.0);
        QCOMPARE(backend.scene().layers.at(0).keys.value("x").at(0).v, backend.scene().layers.at(0).base.value("x"));
        // The camera's curves, when it's selected.
        backend.selectCamera();
        QVERIFY(backend.curveNames().isEmpty());
    }

    void changesAreDescribedInWords() {
        QTemporaryDir dir;
        AssetCache assets;
        Scene before = twoSquares(dir, assets);
        Scene after = before;
        after.layers[1].keys["x"] = {Key{0, 10}, Key{1, 90}};
        after.layers[0].blend = "screen";
        after.camera.base["zoom"] = 2;
        after.duration = 6;
        after.layers.removeAt(0);
        Layer extra;
        extra.name = "rain";
        after.layers << extra;
        const QStringList lines = Project::describeChanges(before, after);
        QVERIFY2(lines.contains("added rain"), qPrintable(lines.join(" | ")));
        QVERIFY(lines.contains("took away under"));
        QVERIFY(lines.contains("over: keys on x"));
        QVERIFY(lines.contains("camera: zoom"));
        QVERIFY(lines.contains("scene: duration"));
        QVERIFY(Project::describeChanges(before, before).isEmpty());
    }

    void searchRanksOperatorsByWhatsTyped() {
        const QStringList names{"Export a video", "Free view", "Cut tool", "Fill the hole by the AI", "Keyframe everything", "Undo"};
        QCOMPARE(Project::rankMatches("cut", names), QList<int>({2}));
        QCOMPARE(Project::rankMatches("fr vi", names), QList<int>({1}));
        // Starting the name counts most, then starting a word, then inside one.
        QCOMPARE(Project::rankMatches("e", names).first(), 0);
        QCOMPARE(Project::rankMatches("ai", names), QList<int>({3}));
        QVERIFY(Project::rankMatches("zebra", names).isEmpty());
        QCOMPARE(Project::rankMatches("", names).size(), names.size());
    }

    void anAgentsChangesAreOneStepToKeepOrTakeBack() {
        QTemporaryDir dir;
        // An agent that slows the scene down by editing scene.json, as one
        // would, and says so; another that fails halfway.
        const QString agent = dir.filePath("agent.sh");
        writeText(agent, "#!/bin/sh\n[ \"$1\" = -p ] || exit 3\nsed -i 's/\"fps\": 30/\"fps\": 24/' scene.json\necho 'Made it 24 fps.'\n");
        const QString failing = dir.filePath("failing.sh");
        writeText(failing, "#!/bin/sh\nsed -i 's/\"fps\": 30/\"fps\": 12/' scene.json\necho 'no credit' >&2\nexit 1\n");
        QFile::setPermissions(agent, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        QFile::setPermissions(failing, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        qputenv("OMOTION_AGENT", agent.toUtf8());
        Backend backend;
        QVERIFY(backend.agentAvailable());
        QVERIFY(backend.open(writePicture(dir.filePath("p.png"))));
        backend.select(0);
        backend.adjust("x", 5);  // Something to undo from before.
        backend.askAgent("make it slower");
        QVERIFY(backend.agentBusy());
        QVERIFY(!backend.busy().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!backend.agentBusy(), 10000);
        QCOMPARE(backend.scene().fps, 24.0);
        QVERIFY(backend.agentReview());
        QCOMPARE(backend.agentReply(), QString("Made it 24 fps."));
        QVERIFY2(backend.agentChanges().contains("scene: fps"), qPrintable(backend.agentChanges().join(" | ")));
        backend.takeBackAgent();
        QCOMPARE(backend.scene().fps, 30.0);
        QVERIFY(!backend.agentReview());
        // One undo took it all back; the edit before it is still there.
        QCOMPARE(backend.values()["x"].toDouble(), backend.scene().layers.at(0).base.value("x"));
        QVERIFY(backend.canUndo());
        // An agent that fails leaves nothing behind, and says why.
        qputenv("OMOTION_AGENT", failing.toUtf8());
        backend.askAgent("break it");
        QTRY_VERIFY_WITH_TIMEOUT(!backend.agentBusy(), 10000);
        QCOMPARE(backend.scene().fps, 30.0);
        QVERIFY(!backend.agentReview());
        QVERIFY(backend.error().contains("no credit"));
        backend.flush();
        QCOMPARE(Project::load(backend.projectPath(), nullptr).fps, 30.0);
        qunsetenv("OMOTION_AGENT");
    }

    void scenesExportAsGltf() {
        auto read = [](const QString &path) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
                return QJsonObject();
            const QByteArray bytes = file.readAll();
            if (bytes.size() < 20 || bytes.left(4) != "glTF")
                return QJsonObject();
            const quint32 length = *reinterpret_cast<const quint32 *>(bytes.constData() + 12);
            return QJsonDocument::fromJson(bytes.mid(20, length)).object();
        };
        auto names = [](const QJsonObject &gltf) {
            QStringList list;
            for (const QJsonValue &node : gltf["nodes"].toArray())
                list << node.toObject()["name"].toString();
            return list;
        };
        QTemporaryDir dir;
        // A world: its picture and piece as meshes, the piece moving.
        const QString project = madeUpWorld(dir);
        Scene scene = Project::load(project, nullptr);
        AssetCache assets(project);
        const int piece = scene.indexOf("piece");
        scene.layers[piece].keys["worldX"] = {Key{0, 0}, Key{1, 2}};
        QString error;
        QVERIFY2(Gltf::write(scene, assets, dir.filePath("world.glb"), &error), qPrintable(error));
        QJsonObject gltf = read(dir.filePath("world.glb"));
        QCOMPARE(gltf["asset"].toObject()["version"].toString(), QString("2.0"));
        QCOMPARE(names(gltf), QStringList({scene.layers[0].name, "piece", "camera"}));
        QCOMPARE(gltf["meshes"].toArray().size(), 2);
        const QJsonArray channels = gltf["animations"].toArray().first().toObject()["channels"].toArray();
        QCOMPARE(channels.size(), 1);  // Only what moves.
        QCOMPARE(channels.first().toObject()["target"].toObject()["node"].toInt(), 1);
        QCOMPARE(channels.first().toObject()["target"].toObject()["path"].toString(), QString("translation"));
        // A flat scene: a card a layer, under the canvas, and the camera.
        AssetCache flatAssets;
        Scene flat = twoSquares(dir, flatAssets);
        QVERIFY(Gltf::write(flat, flatAssets, dir.filePath("flat.glb"), &error));
        gltf = read(dir.filePath("flat.glb"));
        QCOMPARE(names(gltf), QStringList({"canvas", "under", "over", "camera"}));
        QVERIFY(!gltf.contains("animations"));
    }

    void rigsReachLookRollAndFollow() {
        QTemporaryDir dir;
        // An arm: an upper bone 100 px long pointing right, its pivot at its
        // left end, and a forearm as its child, 80 px long.
        auto bar = [&](const QString &name, int length) {
            QImage image(length, 10, QImage::Format_ARGB32);
            image.fill(Qt::white);
            image.save(dir.filePath(name));
        };
        bar("upper.png", 100);
        bar("lower.png", 80);
        QImage wheel(40, 40, QImage::Format_ARGB32);
        wheel.fill(Qt::black);
        wheel.save(dir.filePath("wheel.png"));
        Scene scene = Scene::fromJson(QJsonDocument::fromJson(R"({"width": 400, "height": 400, "layers": [
            {"name": "upper", "source": "upper.png", "x": 100, "y": 200, "anchor": [0, 0.5]},
            {"name": "lower", "source": "lower.png", "parent": "upper", "x": 100, "y": 5, "anchor": [0, 0.5]},
            {"name": "body", "source": "wheel.png", "x": 100, "y": 300},
            {"name": "wheel", "source": "wheel.png", "parent": "body", "x": 20, "y": 40},
            {"name": "eye", "source": "wheel.png", "x": 300, "y": 100}]})").object());
        AssetCache assets(dir.path());
        QString error;
        auto tipAt = [&](double t) {
            return layerTransform(scene, assets, scene.indexOf("lower"), t).map(QPointF(79.5, 5.5));
        };
        const QPointF tip = tipAt(0);
        QVERIFY(QLineF(tip, QPointF(279.5, 200.5)).length() < 1.5);
        // IK: the target starts at the tip, so nothing moves...
        QVERIFY2(Project::rig(scene, assets, 0, "ik", "lower", &error), qPrintable(error));
        const int target = scene.indexOf("lower target");
        QVERIFY(target >= 0);
        QVERIFY(scene.layers.at(target).raw["empty"].toBool());
        QVERIFY(QLineF(tipAt(0), tip).length() < 2);
        // ...and moved, the tip follows it, the elbow bending.
        scene.layers[target].base["x"] = 220;
        scene.layers[target].base["y"] = 260;
        QVERIFY2(QLineF(tipAt(0), QPointF(220, 260)).length() < 2, qPrintable(QString("%1,%2").arg(tipAt(0).x()).arg(tipAt(0).y())));
        QVERIFY(std::abs(scene.value(scene.layers.at(1), "rotation", 0)) > 5);
        // Out of reach, it points straight at it.
        scene.layers[target].base["x"] = 1000;
        scene.layers[target].base["y"] = 200;
        QVERIFY(std::abs(tipAt(0).y() - 200) < 2);
        // It goes through scene.json, as empties do.
        Scene again = Scene::fromJson(scene.toJson(), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(again.layers.at(0).raw["constraints"], scene.layers.at(0).raw["constraints"]);
        QVERIFY(Project::unrig(scene, 0));
        QCOMPARE(scene.value(scene.layers.at(1), "rotation", 0), 0.0);

        // Looking at: turned toward the eye as it moves, from how it faced.
        QVERIFY(Project::rig(scene, assets, 2, "lookAt", "eye", &error));
        QVERIFY(std::abs(scene.value(scene.layers.at(2), "rotation", 0)) < 1e-6);
        scene.layers[4].base["y"] = 300;
        QVERIFY(std::abs(scene.value(scene.layers.at(2), "rotation", 0) - std::atan2(-200, 200) * -180 / M_PI) < 1e-3);
        Project::unrig(scene, 2);

        // A wheel rolls a turn for each circumference its body goes.
        QVERIFY2(Project::rig(scene, assets, 3, "wheel", "body", &error), qPrintable(error));
        scene.layers[2].keys["x"] = {Key{0, 100}, Key{1, 100 + M_PI * 40}};
        QVERIFY(std::abs(scene.value(scene.layers.at(3), "rotation", 1) - scene.value(scene.layers.at(3), "rotation", 0) - 360) < 1);
        QVERIFY(!Project::rig(scene, assets, 4, "wheel", "lower", &error));

        // A path: along the points over the time, turning with the way.
        QVERIFY(Project::rig(scene, assets, 4, "path", {}, &error,
                             {{"points", QJsonArray{QJsonArray{0, 0}, QJsonArray{100, 0}, QJsonArray{100, 100}}}, {"start", 0}, {"end", 2}, {"ease", "linear"}, {"turn", true}}));
        QCOMPARE(scene.value(scene.layers.at(4), "x", 0), 0.0);
        QCOMPARE(scene.value(scene.layers.at(4), "x", 1), 100.0);
        QCOMPARE(scene.value(scene.layers.at(4), "y", 2), 100.0);
        QVERIFY(std::abs(scene.value(scene.layers.at(4), "rotation", 1.5) - 90) < 1e-6);
    }

    void modifiersAndClipsLayerOnTheKeys() {
        QTemporaryDir dir;
        AssetCache assets;
        Scene scene = twoSquares(dir, assets);
        scene.duration = 10;
        scene.layers[1].keys["x"] = {Key{0, 0}, Key{1, 100}};
        QString error;
        // Cycles: past the last key, round again.
        QVERIFY(Project::setModifier(scene, 1, "x", "cycle", 0, 0, &error));
        QCOMPARE(scene.value(scene.layers[1], "x", 2.5), 50.0);
        QCOMPARE(scene.value(scene.layers[1], "x", .5), 50.0);
        // Noise: smooth, small, the same every time, and gone when taken away.
        const Scene cycled = scene;
        QVERIFY(Project::setModifier(scene, 1, "x", "noise", 4, 3, &error));
        QCOMPARE(scene.layers[1].raw["modifiers"].toObject()["x"].toArray().size(), 2);
        double most = 0;
        for (int i = 0; i < 300; ++i) {
            const double t = i / 30.0;
            most = std::max(most, std::abs(scene.value(scene.layers[1], "x", t) - cycled.value(cycled.layers[1], "x", t)));
        }
        Scene plain = scene;
        Project::setModifier(plain, 1, "x", "none", 0, 0, &error);
        QVERIFY(most > .5 && most <= 4.0001);
        QCOMPARE(scene.value(scene.layers[1], "x", 3.3), scene.value(scene.layers[1], "x", 3.3));
        QVERIFY(std::abs(scene.value(scene.layers[1], "x", 3.3) - scene.value(scene.layers[1], "x", 3.3 + 1 / 300.0)) < 1);
        QCOMPARE(plain.value(plain.layers[1], "x", 2.5), 100.0);
        QVERIFY(!plain.layers[1].raw.contains("modifiers"));
        QVERIFY(!Project::setModifier(scene, 1, "colour", "cycle", 0, 0, &error));

        // A clip: a saved motion added on top from its start, keys untouched.
        Project::setMotionDirectory(dir.filePath("motions"));
        QVERIFY(Project::saveMotion("hop", {{"y", Key{0, 500}}, {"y", Key{.5, 450}}, {"y", Key{1, 500}}}, &error));
        QVERIFY2(Project::addClip(plain, 0, "hop", 2, 2, 1, 1, &error), qPrintable(error));
        const double rest = plain.value(plain.layers[0], "y", 0);
        QCOMPARE(plain.value(plain.layers[0], "y", 2.5), rest - 50);
        QCOMPARE(plain.value(plain.layers[0], "y", 3.5), rest - 50);  // Repeated.
        QCOMPARE(plain.value(plain.layers[0], "y", 4.5), rest);       // Done.
        QVERIFY(plain.layers[0].keys.isEmpty());
        // It goes with the project, through scene.json.
        Scene again = Scene::fromJson(plain.toJson(), &error);
        QCOMPARE(again.value(again.layers[0], "y", 2.5), rest - 50);
        QVERIFY(Project::removeClip(plain, 0, 0));
        QCOMPARE(plain.value(plain.layers[0], "y", 2.5), rest);
        // On the camera too.
        QVERIFY(Project::setModifier(plain, Scene::CameraIndex, "x", "noise", 10, 1, &error));
        again = Scene::fromJson(plain.toJson(), &error);
        QVERIFY(again.camera.raw.contains("modifiers"));
        Project::setMotionDirectory({});
    }

    void keysTakeCurvesFromTheirHandles() {
        QTemporaryDir dir;
        Backend backend;
        QVERIFY(backend.open(writePicture(dir.filePath("p.png"))));
        backend.select(0);
        backend.keyProperty("x");
        backend.seek(1);
        backend.setProperty("x", 300);
        backend.setKeyCurve("x", 0, .1, .9, .2, 1);
        QCOMPARE(backend.scene().layers.at(0).keys.value("x").at(0).ease, Scene::cubicEase(.1, .9, .2, 1));
        // Eased out fast: most of the way there by a quarter of the time.
        const double start = backend.scene().layers.at(0).keys.value("x").at(0).v;
        QVERIFY(backend.scene().value(backend.scene().layers.at(0), "x", .25) > start + (300 - start) * .6);
    }

    void severalPiecesMoveTogetherAndSnap() {
        QTemporaryDir dir;
        const QString project = madeUpWorld(dir);
        {
            // A second piece, cut beside the first.
            Scene scene = Project::load(project, nullptr);
            AssetCache assets(project);
            QString error;
            QVERIFY(Project::cutOut(scene, assets, 0, QPolygonF({QPointF(280, 120), QPointF(340, 120), QPointF(340, 170), QPointF(280, 170)}),
                                    0, true, &error, "rock", 0, true, false) >= 0);
            Project::save(project, scene, nullptr);
        }
        Backend backend;
        QVERIFY(backend.open(project));
        const int piece = backend.scene().indexOf("piece"), rock = backend.scene().indexOf("rock");
        backend.select(piece);
        backend.selectAlso(rock);
        QCOMPARE(backend.alsoSelected(), QVariantList({rock}));
        QCOMPARE(backend.gizmo().others.size(), 1);
        // A grab along x, snapped, moves both by the same tenths of a meter.
        const Backend::Gizmo g = backend.gizmo();
        backend.beginHandle(1, g.pivot.x(), g.pivot.y());
        backend.handleTo(g.arrows.at(0).x() + 13, g.arrows.at(0).y(), true);
        backend.endHandle(true);
        const double x = backend.scene().layers.at(piece).base.value("worldX");
        QVERIFY(x > .05);
        QCOMPARE(std::round(x * 10) / 10, x);
        QCOMPARE(backend.scene().layers.at(rock).base.value("worldX"), x);
        // Rings: pitch and roll turn them, snapped to 15°.
        QCOMPARE(backend.gizmo().ringPitch.size(), 49);
        for (const int ring : {5, 6}) {
            const Backend::Gizmo now = backend.gizmo();
            const QPolygonF &polygon = ring == 5 ? now.ringPitch : now.ringRoll;
            const QPointF from = polygon.at(6);
            backend.beginHandle(ring, from.x(), from.y());
            const QPointF swung = now.pivot + QTransform().rotate(40).map(from - now.pivot);
            backend.handleTo(swung.x(), swung.y(), true);
            backend.endHandle(true);
            const QString property = ring == 5 ? "pitch" : "roll";
            const double turned = backend.scene().layers.at(rock).base.value(property);
            QVERIFY2(std::abs(turned) >= 15, qPrintable(QString::number(turned)));
            QCOMPARE(std::fmod(std::abs(turned), 15.0), 0.0);
            QCOMPARE(backend.scene().layers.at(piece).base.value(property), turned);
        }
        // Ctrl+clicking the leader again hands the lead on; a plain click
        // starts over.
        backend.selectAlso(piece);
        QCOMPARE(backend.selected(), rock);
        QVERIFY(backend.alsoSelected().isEmpty());
        backend.select(piece);
        QVERIFY(backend.alsoSelected().isEmpty());
    }

    void bodiesFallLandAndStack() {
        QTemporaryDir dir;
        AssetCache assets;
        Scene scene = twoSquares(dir, assets);  // 40 px squares on a 100×100 canvas.
        scene.duration = 3;
        scene.layers[0].base["y"] = 30;
        scene.layers[1].base["y"] = -40;
        scene.layers[1].base["x"] = 55;
        QString error;
        QVERIFY(Physics::setBody(scene, 0, {{"bounce", 0}}, &error));
        QVERIFY(Physics::setBody(scene, 1, {{"bounce", 0}}, &error));
        QCOMPARE(Physics::bake(scene, assets, 0, 3, &error), 2);
        // The first lands on the canvas's bottom, the second on top of it.
        const double under = scene.value(scene.layers[0], "y", 3), over = scene.value(scene.layers[1], "y", 3);
        QVERIFY2(std::abs(under - 80) < 1.5, qPrintable(QString::number(under)));
        QVERIFY2(std::abs(over - 40) < 1.5, qPrintable(QString::number(over)));
        QCOMPARE(scene.value(scene.layers[0], "y", 0), 30.0);
        // Faster and faster as it falls, before it lands.
        const double a = scene.value(scene.layers[0], "y", .1) - 30, b = scene.value(scene.layers[0], "y", .2) - 30;
        QVERIFY(b > 3 * a);
        // A fixed body holds still and stops what falls on it.
        Scene fixed = twoSquares(dir, assets);
        fixed.duration = 2;
        fixed.layers[1].base["y"] = -40;
        QVERIFY(Physics::setBody(fixed, 0, {{"fixed", true}}, &error));
        QVERIFY(Physics::setBody(fixed, 1, {{"bounce", 0}}, &error));
        QCOMPARE(Physics::bake(fixed, assets, 0, 2, &error), 1);
        QCOMPARE(fixed.value(fixed.layers[0], "y", 2), 50.0);
        QVERIFY(std::abs(fixed.value(fixed.layers[1], "y", 2) - 10) < 1.5);
        QVERIFY(!Physics::setBody(fixed, 5, {{"mass", 1}}, &error));

        // Thrown sideways, a body rolls over as the floor catches it, and
        // comes to rest on a side; told not to tumble, it only slides.
        Scene thrown = twoSquares(dir, assets);
        thrown.duration = 3;
        thrown.layers.removeLast();
        thrown.layers[0].keys["x"] = {Key{0, 10, "linear"}, Key{.1, 40, "linear"}};
        QVERIFY(Physics::setBody(thrown, 0, {{"bounce", 0}, {"friction", .8}}, &error));
        Scene sliding = thrown;
        QCOMPARE(Physics::bake(thrown, assets, 0, 3, &error), 1);
        QVERIFY(thrown.layers[0].keys.contains("rotation"));
        double most = 0;
        for (double t = 0; t < 3; t += .05)
            most = std::max(most, std::abs(thrown.value(thrown.layers[0], "rotation", t)));
        QVERIFY2(most > 20, qPrintable(QString::number(most)));
        const double rest = thrown.value(thrown.layers[0], "rotation", 3);
        QVERIFY2(std::abs(rest - std::round(rest / 90) * 90) < 1, qPrintable(QString::number(rest)));
        sliding.layers[0].raw["body"] = QJsonObject{{"bounce", 0}, {"friction", .8}, {"tumble", false}};
        QCOMPARE(Physics::bake(sliding, assets, 0, 3, &error), 1);
        QVERIFY(!sliding.layers[0].keys.contains("rotation"));

        // In a world: dropped from up high, it lands on the ground there.
        QTemporaryDir worldDir;
        const QString project = madeUpWorld(worldDir);
        Scene world = Project::load(project, nullptr);
        AssetCache worldAssets(project);
        const int piece = world.indexOf("piece");
        world.layers[piece].base["worldY"] = 3;
        world.duration = 3;
        QVERIFY(Physics::setBody(world, piece, {{"bounce", .2}}, &error));
        QVERIFY2(Physics::bake(world, worldAssets, 0, 3, &error) == 1, qPrintable(error));
        QList<QVector3D> corners;
        QVector3D pivot;
        QVERIFY(World::box(world, worldAssets, piece, 3, &corners, &pivot));
        float lowest = 1e9f;
        QVector3D middle;
        for (const QVector3D &c : corners)
            lowest = std::min(lowest, c.y()), middle += c / 8;
        const double ground = World::groundAt(world, worldAssets, middle.x(), middle.z());
        QVERIFY2(std::abs(lowest - ground) < .05, qPrintable(QString("%1 vs %2").arg(lowest).arg(ground)));
    }

    void emittersTrailFromWhereTheLayerWas() {
        QTemporaryDir dir;
        AssetCache assets;
        Scene scene = twoSquares(dir, assets);  // 100×100, 40 px squares.
        scene.width = 400;
        scene.camera.base["x"] = 200;  // The camera looks at the wider canvas's middle.
        scene.duration = 2;
        scene.background = Qt::black;
        scene.layers.removeAt(1);
        scene.layers[0].keys["x"] = {Key{0, 40, "linear"}, Key{2, 360, "linear"}};
        QString error;
        const int dust = Project::addParticles(scene, "embers",
                                               {{"from", scene.layers[0].name}, {"rate", 60}, {"life", .6}, {"speed", 0},
                                                {"inherit", 0}, {"gravity", 0}, {"twinkle", 0}, {"color", "#00ff00"}}, &error);
        QVERIFY2(dust >= 0, qPrintable(error));
        QCOMPARE(scene.layers[dust].particles["from"].toString(), scene.layers[0].name);
        // At 1 s the square is at x 200: the trail is behind it, to the left,
        // as far as it's gone in the particles' life; nothing ahead.
        const QImage frame = renderFrame(scene, assets, 1);
        auto green = [&](int x) {
            int most = 0;
            for (int y = 0; y < 100; ++y) {
                const QColor c = frame.pixelColor(x, y);
                most = std::max(most, c.green() - c.red() - c.blue());
            }
            return most;
        };
        QVERIFY(green(130) > 5);     // 0.4 s ago, fading but alive.
        QVERIFY(green(170) > 20);
        QCOMPARE(green(300), 0);      // Ahead of it.
        QCOMPARE(green(60), 0);       // Too long ago: gone.
        // Before it started emitting there's nothing to draw.
        QVERIFY(!Project::particleSetting("rate", "fast", nullptr, &error));
        QJsonValue value;
        QVERIFY(Project::particleSetting("at", "12,30", &value, &error));
        QCOMPARE(value.toArray().at(1).toDouble(), 30.0);
    }

    void theLookGradesTheFrame() {
        QImage grey(100, 100, QImage::Format_ARGB32_Premultiplied);
        grey.fill(QColor(100, 100, 100));
        QImage frame = grey;
        applyLook(frame, {{"exposure", 1}});
        QCOMPARE(frame.pixelColor(50, 50).red(), 200);
        frame = grey;
        frame.setPixelColor(10, 10, QColor(200, 50, 50));
        applyLook(frame, {{"saturation", 0}});
        QCOMPARE(frame.pixelColor(10, 10).red(), frame.pixelColor(10, 10).blue());
        frame = grey;
        applyLook(frame, {{"temperature", 1}});
        QVERIFY(frame.pixelColor(50, 50).red() > frame.pixelColor(50, 50).blue());
        frame = grey;
        applyLook(frame, {{"vignette", 1}});
        QVERIFY(frame.pixelColor(0, 0).red() < 40);
        QCOMPARE(frame.pixelColor(50, 50).red(), 100);
        // Bloom: light spills from what's bright onto what's next to it.
        frame = grey;
        for (int y = 40; y < 60; ++y)
            for (int x = 40; x < 60; ++x)
                frame.setPixelColor(x, y, Qt::white);
        applyLook(frame, {{"bloom", 1}});
        QVERIFY(frame.pixelColor(64, 50).red() > 110);
        QCOMPARE(frame.pixelColor(5, 5).red(), 100);

        // Set through the scene: neutral settings go away.
        QTemporaryDir dir;
        AssetCache assets;
        Scene scene = twoSquares(dir, assets);
        QString error;
        QVERIFY(Project::setLook(scene, "exposure", 9, &error));
        QCOMPARE(scene.raw["look"].toObject()["exposure"].toDouble(), 5.0);
        QVERIFY(Project::setLook(scene, "exposure", 0, &error));
        QVERIFY(!scene.raw.contains("look"));
        QVERIFY(!Project::setLook(scene, "sparkle", 1, &error));
        // Depth of field: a layer set back from the focus blurs, one at it
        // stays sharp.
        const QImage sharp = renderFrame(scene, assets, 0);
        QVERIFY(Project::setLook(scene, "aperture", 5, &error));
        QVERIFY(Project::setLook(scene, "focus", 0, &error));
        QCOMPARE(renderFrame(scene, assets, 0), sharp);
        scene.layers[1].base["z"] = 300;
        const QImage back = renderFrame(scene, assets, 0);
        scene.raw["look"] = QJsonObject{};
        const QImage backSharp = renderFrame(scene, assets, 0);
        QVERIFY(differences(back, backSharp) > 20);
    }

    void filmsJoinShots() {
        QTemporaryDir dir;
        // Three one-colour shots: red, green and blue, two seconds each.
        auto shot = [&](const QString &name, const QColor &colour) {
            QImage image(64, 32, QImage::Format_RGB32);
            image.fill(colour);
            const QString picture = dir.filePath(name + ".png");
            image.save(picture);
            const QString project = Project::open(picture, nullptr);
            Scene scene = Project::load(project, nullptr);
            scene.duration = 2;
            Project::save(project, scene, nullptr);
            return QFileInfo(project).fileName();
        };
        const QString red = shot("red", Qt::red), green = shot("green", Qt::green), blue = shot("blue", Qt::blue);
        writeText(dir.filePath("film.json"), QJsonDocument(QJsonObject{{"fps", 10}, {"shots", QJsonArray{
            QJsonObject{{"project", red}},
            QJsonObject{{"project", green}, {"transition", "dissolve"}, {"length", 1}},
            QJsonObject{{"project", blue}, {"from", .5}, {"transition", "fade"}, {"length", 1}}}}}).toJson());
        Film film;
        QString error;
        QVERIFY2(film.load(dir.filePath("film.json"), &error), qPrintable(error));
        // 2 + 2 - 1 overlapped + 1.5.
        QCOMPARE(film.duration(), 4.5);
        QCOMPARE(film.frameCount(), 45);
        const QSize size = film.size();
        QCOMPARE(film.frame(.5, size).pixelColor(10, 10), QColor(Qt::red));
        // Half way through the dissolve, half and half.
        const QColor mixed = film.frame(1.5, size).pixelColor(10, 10);
        QVERIFY(std::abs(mixed.red() - 128) < 4 && std::abs(mixed.green() - 128) < 4);
        QCOMPARE(film.frame(2.5, size).pixelColor(10, 10), QColor(Qt::green));
        // Through black: green goes down to it, then blue comes up.
        QVERIFY(film.frame(2.95, size).pixelColor(10, 10).green() < 40);
        QVERIFY(film.frame(3.05, size).pixelColor(10, 10).blue() < 40);
        QCOMPARE(film.frame(4, size).pixelColor(10, 10), QColor(Qt::blue));
        // Written out as frames.
        const Exporter::Source source = [&film](int frame, const QSize &at) { return film.frame(frame / film.fps(), at); };
        QVERIFY2(Exporter::runFrames(source, film.frameCount(), film.fps(), size, dir.filePath("frames/"), &error), qPrintable(error));
        QVERIFY(QFile::exists(dir.filePath("frames/0045.png")));
        // The second shot's sound plays where it is in the film.
        if (!QStandardPaths::findExecutable("ffmpeg").isEmpty()) {
            QProcess tone;
            tone.start("ffmpeg", {"-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "sine=frequency=440:duration=3", dir.filePath("tone.wav")});
            QVERIFY(tone.waitForFinished(30000) && tone.exitCode() == 0);
            Scene withSound = Project::load(dir.filePath(green), nullptr);
            QVERIFY(Project::setAudio(withSound, dir.filePath(green), dir.filePath("tone.wav"), &error));
            Project::save(dir.filePath(green), withSound, nullptr);
            QVERIFY(film.load(dir.filePath("film.json"), &error));
            QVERIFY2(Exporter::runFrames(source, film.frameCount(), film.fps(), size, dir.filePath("film.mp4"), &error), qPrintable(error));
            QVERIFY2(film.addSound(dir.filePath("film.mp4"), &error), qPrintable(error));
            auto loudness = [&](double from) {
                QProcess probe;
                probe.start("ffmpeg", {"-hide_banner", "-ss", QString::number(from), "-t", "0.5", "-i", dir.filePath("film.mp4"),
                                       "-af", "volumedetect", "-vn", "-f", "null", "-"});
                probe.waitForFinished(30000);
                const QString log = QString::fromUtf8(probe.readAllStandardError());
                const int at = int(log.indexOf("mean_volume: "));
                return at < 0 ? -200.0 : log.mid(at + 13).section(' ', 0, 0).toDouble();
            };
            QVERIFY(loudness(.2) < -60);   // The red shot has none.
            QVERIFY(loudness(2.5) > -40);  // The green one's tone.
        }
        // Broken films say why.
        writeText(dir.filePath("bad.json"), R"({"shots": [{"project": "red.omotion", "transition": "spin"}]})");
        QVERIFY(!film.load(dir.filePath("bad.json"), &error));
        QVERIFY(error.contains("spin"));
    }

    void drawingsHoldUntilTheNextAndMoveWithTheirLayer() {
        QTemporaryDir dir;
        AssetCache assets;
        Scene scene = twoSquares(dir, assets);
        scene.background = Qt::black;
        scene.layers.clear();
        const int drawing = Project::addDrawing(scene, {});
        QString error;
        // A red stroke across the top at 0, a green one across the bottom at 1.
        QVERIFY2(Project::addStroke(scene, assets, drawing, 0, {{10, 20}, {50, 20}, {90, 20}}, Qt::red, 6, &error), qPrintable(error));
        QVERIFY(Project::addStroke(scene, assets, drawing, 1, {{10, 80}, {90, 80}}, Qt::green, 6, &error));
        QCOMPARE(scene.layers[drawing].raw["drawing"].toObject()["frames"].toArray().size(), 2);
        QCOMPARE(renderFrame(scene, assets, .5).pixelColor(50, 20), QColor(Qt::red));
        QCOMPARE(renderFrame(scene, assets, .5).pixelColor(50, 80), QColor(Qt::black));
        QCOMPARE(renderFrame(scene, assets, 1.5).pixelColor(50, 80), QColor(Qt::green));
        QCOMPARE(renderFrame(scene, assets, 1.5).pixelColor(50, 20), QColor(Qt::black));
        // A second stroke on the same frame joins it; taking one back.
        QVERIFY(Project::addStroke(scene, assets, drawing, 1.01, {{50, 0}, {50, 100}}, Qt::blue, 4, &error));
        QCOMPARE(scene.layers[drawing].raw["drawing"].toObject()["frames"].toArray().size(), 2);
        QCOMPARE(renderFrame(scene, assets, 1.5).pixelColor(50, 40), QColor(Qt::blue));
        QVERIFY(Project::removeStroke(scene, drawing, 1.5));
        QCOMPARE(renderFrame(scene, assets, 1.5).pixelColor(50, 40), QColor(Qt::black));
        // It moves as a layer does.
        scene.layers[drawing].base["y"] = 30;
        QCOMPARE(renderFrame(scene, assets, .5).pixelColor(50, 50), QColor(Qt::red));
        // Through scene.json.
        Scene again = Scene::fromJson(scene.toJson(), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(renderFrame(again, assets, .5).pixelColor(50, 50), QColor(Qt::red));
        QVERIFY(!Project::addStroke(scene, assets, 5, 0, {{1, 1}}, Qt::red, 2, &error));
    }

    void shapesSmoothAndSimplify() {
        QTemporaryDir dir;
        const QString project = madeUpWorld(dir);
        Scene scene = Project::load(project, nullptr);
        AssetCache assets(project);
        const int piece = scene.indexOf("piece");
        // A lumpy ball as its shape: rings of points, every other one out.
        QList<float> positions;
        QList<quint32> triangles;
        const int rings = 24, around = 32;
        for (int r = 0; r <= rings; ++r)
            for (int a = 0; a < around; ++a) {
                const double lat = M_PI * r / rings, lon = 2 * M_PI * a / around;
                const double radius = .5 * (1 + .15 * ((r + a) % 2));
                positions << float(radius * std::sin(lat) * std::cos(lon)) << float(radius * std::cos(lat)) << float(radius * std::sin(lat) * std::sin(lon));
            }
        for (int r = 0; r < rings; ++r)
            for (int a = 0; a < around; ++a) {
                const quint32 p = quint32(r * around + a), q = quint32(r * around + (a + 1) % around);
                triangles << p << p + around << q << q << p + around << q + around;
            }
        QByteArray bytes("OMSHAPE1");
        const quint32 v = quint32(positions.size() / 3), n = quint32(triangles.size());
        bytes.append(reinterpret_cast<const char *>(&v), 4).append(reinterpret_cast<const char *>(&n), 4);
        bytes.append(reinterpret_cast<const char *>(positions.constData()), positions.size() * 4);
        bytes.append(reinterpret_cast<const char *>(triangles.constData()), triangles.size() * 4);
        writeText(QDir(project).filePath("assets/world/ball.shape"), bytes);
        scene.layers[piece].raw["shape"] = QJsonObject{{"file", "assets/world/ball.shape"}, {"quarter", 0}};
        const World::Mesh lumpy = World::mesh(scene, assets, piece);
        QVERIFY(!lumpy.isNull());
        // Lumps: how much the distance from its middle varies, for its size.
        auto roughness = [](const World::Mesh &mesh) {
            const float *f = reinterpret_cast<const float *>(mesh.vertices.constData());
            const qsizetype count = mesh.vertices.size() / qsizetype(mesh.stride * sizeof(float));
            QVector3D middle;
            for (qsizetype k = 0; k < count; ++k)
                middle += QVector3D(f[k * mesh.stride], f[k * mesh.stride + 1], f[k * mesh.stride + 2]) / float(count);
            double sum = 0, squares = 0;
            for (qsizetype k = 0; k < count; ++k) {
                const double d = (QVector3D(f[k * mesh.stride], f[k * mesh.stride + 1], f[k * mesh.stride + 2]) - middle).length();
                sum += d, squares += d * d;
            }
            const double mean = sum / count;
            return std::sqrt(std::max(0.0, squares / count - mean * mean)) / mean;
        };
        QString error;
        QVERIFY2(World::setShapeModifier(scene, piece, "smooth", 3, &error), qPrintable(error));
        const World::Mesh smooth = World::mesh(scene, assets, piece);
        QVERIFY(roughness(smooth) < roughness(lumpy) * .8);
        QVERIFY(World::setShapeModifier(scene, piece, "simplify", 8, &error));
        const World::Mesh light = World::mesh(scene, assets, piece);
        QVERIFY(light.indices.size() < lumpy.indices.size() / 2);
        QCOMPARE(scene.layers[piece].raw["shape"].toObject()["modifiers"].toArray().size(), 2);
        QVERIFY(World::setShapeModifier(scene, piece, "none", 0, &error));
        QCOMPARE(World::mesh(scene, assets, piece).indices.size(), lumpy.indices.size());
        QVERIFY(!World::setShapeModifier(scene, 0, "smooth", 2, &error));
    }

    void lightsAndMaterialsInAWorld() {
        QTemporaryDir dir;
        Backend backend;
        QVERIFY(backend.open(madeUpWorld(dir)));
        const World::View shot = backend.view();
        backend.addLight();
        const int lamp = backend.selected();
        QVERIFY(backend.scene().layers.at(lamp).raw.contains("light"));
        World::Frame frame = World::frame(backend.scene(), backend.assets(), 0);
        QCOMPARE(frame.lights.size(), 1);
        // In front of the camera, a little up.
        const QVector3D ahead = shot.rotation.rotatedVector(QVector3D(0, 0, -1));
        QVERIFY(QVector3D::dotProduct(frame.lights.first().position - shot.position, ahead) > 3);
        // Its handles move it; its power and opacity set how bright.
        const Backend::Gizmo g = backend.gizmo();
        QCOMPARE(g.arrows.size(), 3);
        QCOMPARE(g.lights.size(), 1);
        backend.beginHandle(2, g.pivot.x(), g.pivot.y());
        backend.handleTo(g.pivot.x(), g.pivot.y() - 40);
        backend.endHandle(true);
        QVERIFY(World::frame(backend.scene(), backend.assets(), 0).lights.first().position.y() > frame.lights.first().position.y());
        backend.setLightValue("power", 2);
        backend.setLightValue("color", "#ff0000");
        frame = World::frame(backend.scene(), backend.assets(), 0);
        QCOMPARE(frame.lights.first().colour, QVector3D(2, 0, 0));
        backend.setProperty("opacity", .5);
        QCOMPARE(World::frame(backend.scene(), backend.assets(), 0).lights.first().colour, QVector3D(1, 0, 0));
        backend.setProperty("opacity", 0);
        QVERIFY(World::frame(backend.scene(), backend.assets(), 0).lights.isEmpty());
        QCOMPARE(backend.gizmo().lights.size(), 1);  // Still shown, to find.
        backend.setLightValue("power", -1);
        QVERIFY(!backend.error().isEmpty());
        // A piece's material reaches its placement.
        backend.select(backend.scene().indexOf("piece"));
        backend.setMaterial("shine", .7);
        backend.setMaterial("roughness", 2);
        for (const World::Placement &place : World::frame(backend.scene(), backend.assets(), 0).layers)
            if (place.layer == backend.selected()) {
                QCOMPARE(place.shine, .7f);
                QCOMPARE(place.roughness, 1.f);
            }
        // Grids carry smooth normals for the lights.
        const World::Mesh ground = World::mesh(backend.scene(), backend.assets(), 0);
        QCOMPARE(ground.normals.size() / qsizetype(3 * sizeof(float)), ground.vertices.size() / qsizetype(5 * sizeof(float)));
    }

    void videoLayersPlay() {
        if (QStandardPaths::findExecutable("ffmpeg").isEmpty())
            QSKIP("ffmpeg isn't installed");
        QTemporaryDir dir;
        // Ten frames: red for half a second, then blue.
        QProcess make;
        make.start("ffmpeg", {"-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "color=red:s=64x32:r=10:d=0.5",
                              "-f", "lavfi", "-i", "color=blue:s=64x32:r=10:d=0.5", "-filter_complex", "[0][1]concat=n=2",
                              dir.filePath("clip.mp4")});
        QVERIFY(make.waitForFinished(30000) && make.exitCode() == 0);
        const QString project = Project::open(writePicture(dir.filePath("p.png")), nullptr);
        Scene scene = Project::load(project, nullptr);
        scene.fps = 10;
        AssetCache assets(project);
        QString error;
        QVERIFY(Project::isVideo(dir.filePath("clip.mp4")));
        const int clip = Project::importVideo(scene, project, dir.filePath("clip.mp4"), &error);
        QVERIFY2(clip >= 0, qPrintable(error));
        QCOMPARE(scene.layers[clip].raw["video"].toObject()["count"].toInt(), 10);
        auto colourAt = [&](double t) { return renderFrame(scene, assets, t).pixelColor(100, 50); };
        QVERIFY(colourAt(.2).red() > 200);
        QVERIFY(colourAt(.7).blue() > 200);
        QVERIFY(colourAt(1.2).red() > 200);  // Looping.
        QVERIFY(Project::setVideo(scene, clip, "loop", false, &error));
        QVERIFY(colourAt(1.2).blue() > 200);  // Holding its last.
        QVERIFY(Project::setVideo(scene, clip, "start", .5, &error));
        QVERIFY(colourAt(.7).red() > 200);
        QVERIFY(!Project::setVideo(scene, 0, "speed", 2, &error));
        QCOMPARE(sourceAt(scene.layers[clip], .55), scene.layers[clip].source);
    }

    void trackingFollowsAPointInAVideo() {
        if (QStandardPaths::findExecutable("ffmpeg").isEmpty())
            QSKIP("ffmpeg isn't installed");
        QTemporaryDir dir;
        // A white square sliding right 60 px a second (6 a frame) over a
        // speckled ground, made as frames and encoded.
        QImage ground(200, 100, QImage::Format_RGB32);
        QRandomGenerator random(7);
        for (int y = 0; y < 100; ++y)
            for (int x = 0; x < 200; ++x) {
                const int v = 90 + int(random.bounded(60));
                ground.setPixelColor(x, y, QColor(v, v, v));
            }
        for (int f = 0; f < 10; ++f) {
            QImage frame = ground;
            QPainter painter(&frame);
            painter.fillRect(QRect(30 + 6 * f, 40, 14, 14), Qt::white);
            painter.end();
            frame.save(dir.filePath(QString("f%1.png").arg(f, 2, 10, QChar('0'))));
        }
        QProcess make;
        make.start("ffmpeg", {"-hide_banner", "-loglevel", "error", "-framerate", "10", "-i", dir.filePath("f%02d.png"),
                              "-pix_fmt", "yuv444p", "-crf", "5", dir.filePath("clip.mp4")});
        QVERIFY(make.waitForFinished(30000) && make.exitCode() == 0);
        const QString project = Project::open(writePicture(dir.filePath("p.png")), nullptr);  // 200×100.
        Scene scene = Project::load(project, nullptr);
        scene.fps = 10;
        scene.duration = 1;
        AssetCache assets(project);
        QString error;
        const int clip = Project::importVideo(scene, project, dir.filePath("clip.mp4"), &error);
        QVERIFY2(clip >= 0, qPrintable(error));
        QList<QPointF> path;
        const int follower = Project::track(scene, assets, clip, QPointF(37, 47), 0, .9, &error, {}, &path);
        QVERIFY2(follower >= 0, qPrintable(error));
        QCOMPARE(path.size(), 10);
        QVERIFY2(std::abs(path.last().x() - (37 + 54)) < 3, qPrintable(QString::number(path.last().x())));
        QVERIFY(std::abs(path.last().y() - 47) < 3);
        // The empty it made follows: where the square is, there it is.
        const Layer &empty = scene.layers.at(follower);
        QVERIFY(empty.raw["empty"].toBool());
        QVERIFY(std::abs(layerTransform(scene, assets, follower, .9).map(QPointF()).x() - path.last().x()) < .5);
        QVERIFY(Project::track(scene, assets, 0, QPointF(37, 47), 0, 1, &error) < 0);
    }

    void modelsStandInTheWorld() {
        QTemporaryDir dir;
        const QString project = madeUpWorld(dir);
        // A box 2 units across and 4 tall, standing on its origin.
        writeText(dir.filePath("box.obj"), "v -1 0 -1\nv 1 0 -1\nv 1 0 1\nv -1 0 1\nv -1 4 -1\nv 1 4 -1\nv 1 4 1\nv -1 4 1\n"
                                           "f 1 2 3 4\nf 5 6 7 8\nf 1 2 6 5\nf 2 3 7 6\nf 3 4 8 7\nf 4 1 5 8\n");
        QVector3D low, high;
        QVERIFY(World::modelBounds(dir.filePath("box.obj"), &low, &high));
        QCOMPARE(high - low, QVector3D(2, 4, 2));
        Scene scene = Project::load(project, nullptr);
        AssetCache assets(project);
        QString error;
        QVERIFY(Project::isModel(dir.filePath("box.obj")));
        const int box = Project::importModel(scene, project, dir.filePath("box.obj"), QVector3D(0, 0, 6), 2, &error);
        QVERIFY2(box >= 0, qPrintable(error));
        QVERIFY(QFile::exists(QDir(project).filePath(scene.layers[box].raw["model"].toObject()["file"].toString())));
        // 2 m tall, its bottom's middle on its point, 6 m ahead.
        QList<QVector3D> corners;
        QVector3D pivot;
        QVERIFY(World::box(scene, assets, box, 0, &corners, &pivot));
        QVector3D a = corners.first(), b = corners.first();
        for (const QVector3D &c : corners)
            for (int k = 0; k < 3; ++k)
                a[k] = std::min(a[k], c[k]), b[k] = std::max(b[k], c[k]);
        QVERIFY(std::abs((b - a).y() - 2) < 1e-4);
        const QVector3D eye = World::pictureToWorld(World::info(scene)).map(QVector3D());
        QVERIFY(std::abs(a.y() - eye.y()) < 1e-4);
        QVERIFY(std::abs((a + b).z() / 2 - (eye.z() - 6)) < 1e-4);
        // Picked where it stands, in front of the picture.
        const World::View shot = World::cameraView(World::frame(scene, assets, 0));
        QPointF at;
        QVERIFY(World::project(shot, QSizeF(400, 200), (a + b) / 2, &at));
        QCOMPARE(World::pick(scene, assets, 0, shot, QSizeF(400, 200), at), box);
        // It goes through scene.json, and is a model in the outliner.
        QVERIFY(Scene::fromJson(scene.toJson(), &error).layers.size() == scene.layers.size());
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(!Project::isModel(dir.filePath("box.png")));
    }

    void theWindowEditsTheFilmBesideIt() {
        QTemporaryDir dir;
        Backend backend;
        QVERIFY(backend.open(writePicture(dir.filePath("a.png"))));
        QVERIFY(backend.filmShots().isEmpty());
        backend.addToFilm();
        backend.addToFilm();
        QCOMPARE(backend.filmShots().size(), 2);
        QVERIFY(backend.filmShots().first().toMap()["this"].toBool());
        backend.setShot(1, "transition", "dissolve");
        backend.setShot(1, "length", .25);
        backend.setShot(1, "transition", "spin");
        QCOMPARE(backend.filmShots().at(1).toMap()["transition"].toString(), QString("dissolve"));
        QCOMPARE(backend.filmShots().at(1).toMap()["length"].toDouble(), .25);
        backend.moveShot(1, -1);
        QCOMPARE(backend.filmShots().first().toMap()["transition"].toString(), QString("dissolve"));
        backend.removeShot(0);
        QCOMPARE(backend.filmShots().size(), 1);
        Film film;
        QString error;
        QVERIFY2(film.load(backend.filmPath(), &error), qPrintable(error));
    }

    void layersBillowAndLoop() {
        // Stripes, pushed about: changed, changing over time, and the same
        // at the scene's end as at its start.
        QImage stripes(120, 80, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < 80; ++y)
            for (int x = 0; x < 120; ++x)
                stripes.setPixelColor(x, y, (x / 10) % 2 ? Qt::white : Qt::black);
        const QJsonObject warp{{"amount", 6}, {"scale", 30}, {"speed", .5}};
        const QImage a = warped(stripes, warp, 0, 1, 4), b = warped(stripes, warp, 1, 1, 4), c = warped(stripes, warp, 4, 1, 4);
        QVERIFY(differences(a, stripes) > 200);
        QVERIFY(differences(a, b) > 100);
        QVERIFY(differences(a, c) < 5);
        QCOMPARE(warped(stripes, {{"amount", 0}}, 1, 1, 4), stripes);
        QTemporaryDir dir;
        AssetCache assets;
        Scene scene = twoSquares(dir, assets);
        QString error;
        QVERIFY(Project::setWarp(scene, 1, "amount", 5, &error));
        QVERIFY(scene.layers[1].raw.contains("warp"));
        QVERIFY(Project::setWarp(scene, 1, "amount", 0, &error));
        QVERIFY(!scene.layers[1].raw.contains("warp"));
        QVERIFY(!Project::setWarp(scene, 1, "wobble", 1, &error));
    }

    void surfacesWrapSolidsFacingOut() {
        // A ball of radius 0.5: inside positive, as the shape model gives.
        const int n = 40;
        const double r = 1;
        std::vector<float> field(size_t(n) * n * n);
        for (int x = 0; x < n; ++x)
            for (int y = 0; y < n; ++y)
                for (int z = 0; z < n; ++z) {
                    const QVector3D p(-r + 2 * r * x / (n - 1), -r + 2 * r * y / (n - 1), -r + 2 * r * z / (n - 1));
                    field[(size_t(x) * n + y) * n + z] = .5f - p.length();
                }
        const Ai::Shape ball = Ai::surface(field, n, r);
        QVERIFY(ball.triangles.size() > 1000);
        for (size_t i = 0; i < ball.positions.size(); i += 3)
            QVERIFY(std::abs(QVector3D(ball.positions[i], ball.positions[i + 1], ball.positions[i + 2]).length() - .5f) < .03f);
        int outward = 0;
        for (size_t t = 0; t < ball.triangles.size(); t += 3) {
            QVector3D v[3];
            for (int k = 0; k < 3; ++k) {
                const size_t i = ball.triangles[t + k] * 3;
                v[k] = QVector3D(ball.positions[i], ball.positions[i + 1], ball.positions[i + 2]);
            }
            outward += QVector3D::dotProduct(QVector3D::crossProduct(v[1] - v[0], v[2] - v[0]), v[0] + v[1] + v[2]) > 0;
        }
        QCOMPARE(outward, int(ball.triangles.size() / 3));
    }

    void worldsTearAtJumpsOverTheLayerBehind() {
        // A wall at 3 m before a background at 30 m: with a layer behind, the
        // picture's mesh leaves out the triangles that would stretch between
        // them, and the layer behind has a mesh of its own.
        QTemporaryDir dir;
        const QString project = Project::open(writePicture(dir.filePath("p.png"), {400, 200}), nullptr);
        Scene scene = Project::load(project, nullptr);
        AssetCache assets(project);
        QImage depth(40, 20, QImage::Format_Grayscale16);
        for (int y = 0; y < 20; ++y)
            for (int x = 0; x < 40; ++x)
                reinterpret_cast<quint16 *>(depth.scanLine(y))[x] = World::encodeDepth(x < 20 ? 3 : 30);
        QDir(project).mkpath("assets/world");
        QVERIFY(depth.save(QDir(project).filePath("assets/world/d.png")));
        QJsonObject world{{"picture", scene.layers[0].name}, {"depth", "assets/world/d.png"}, {"behind", "assets/world/d.png"},
                          {"focal", 350}, {"ground", QJsonObject{{"up", QJsonArray{0, -1, 0}}, {"height", 1.5}}}};
        scene.raw["world"] = world;
        const int whole = int(World::mesh(scene, assets, 0).indices.size());
        QVERIFY(World::mesh(scene, assets, World::FarLayer).isNull());
        world["far"] = QJsonObject{{"depth", "assets/world/d.png"}, {"picture", "assets/p.png"}};
        scene.raw["world"] = world;
        const World::Mesh torn = World::mesh(scene, assets, 0);
        QVERIFY(!torn.isNull());
        QVERIFY(int(torn.indices.size()) < whole || torn.columns > 100);
        // Every triangle left stays on one side of the jump.
        const float *v = reinterpret_cast<const float *>(torn.vertices.constData());
        const quint32 *t = reinterpret_cast<const quint32 *>(torn.indices.constData());
        const QVector3D eye = World::frame(scene, assets, 0).cameraPosition;
        for (qsizetype i = 0; i < torn.indices.size() / qsizetype(sizeof(quint32)); i += 3) {
            float near = 1e9f, far = 0;
            for (int k = 0; k < 3; ++k) {
                const float d = (QVector3D(v[t[i + k] * 5], v[t[i + k] * 5 + 1], v[t[i + k] * 5 + 2]) - eye).length();
                near = std::min(near, d), far = std::max(far, d);
            }
            QVERIFY(far <= near * 1.15f + 1e-3f);
        }
        QVERIFY(!World::mesh(scene, assets, World::FarLayer).isNull());
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
        QVERIFY(splitter.scene().camera.keys.contains("x"));  // A moves the camera...
        QVERIFY(splitter.scene().layers[0].follow < 1);       // ...which the background follows less.
        splitter.undo();
        splitter.undo();
        QCOMPARE(splitter.scene().layers.size(), 1);
    }

    void splittingAPieceMeansItsPicture() {
        if (!Ai::instance().problem().isEmpty())
            QSKIP(qPrintable("The AI isn't set up here: " + Ai::instance().problem()));
        QTemporaryDir dir;
        Backend backend;
        backend.open(ballPicture(dir));
        backend.select(0);
        backend.setTool("cut");
        for (const QPointF p : {QPointF(150, 150), QPointF(250, 150), QPointF(250, 250), QPointF(150, 250)})
            backend.addCutPoint(p.x(), p.y());
        backend.finishCut(true);
        QCOMPARE(backend.selected(), 1);  // The new piece.
        backend.splitByDepth(3);
        backend.waitForJobs();
        QCOMPARE(backend.error(), QString());
        QVERIFY(backend.scene().layers.size() >= 3);
        QCOMPARE(backend.scene().layers[0].name, QString("ball"));
        QVERIFY(backend.scene().layers[0].raw.contains("depth"));  // The picture was split, not the piece.
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
        QVERIFY(backend.busy().startsWith("Cutting"));
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

    void pendingSavesDontOverwriteOutsideEdits() {
        QTemporaryDir dir;
        Backend backend;
        backend.open(writePicture(dir.filePath("p.png")));
        backend.select(0);
        backend.setProperty("x", 10);  // A save is now waiting.
        const QString file = Project::sceneFile(backend.projectPath());
        QJsonObject json = QJsonDocument::fromJson([&] { QFile f(file); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }()).object();
        json["duration"] = 9;
        writeText(file, QJsonDocument(json).toJson());  // An agent's edit, before the reload notices.
        backend.flush();
        QCOMPARE(backend.sceneInfo()["duration"].toDouble(), 9.0);
        QFile disk(file);
        QVERIFY(disk.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(disk.readAll()).object()["duration"].toDouble(), 9.0);
        // Ours is a step back.
        backend.undo();
        QCOMPARE(backend.values()["x"].toDouble(), 10.0);
    }

    void deletedLayersLetGoOfTheirPieces() {
        QTemporaryDir dir;
        Backend backend;
        backend.open(writePicture(dir.filePath("p.png")));
        backend.select(0);
        backend.setTool("cut");
        for (const QPointF p : {QPointF(5, 5), QPointF(80, 5), QPointF(80, 60), QPointF(5, 60)})
            backend.addCutPoint(p.x(), p.y());
        backend.finishCut(true);
        backend.select(0);
        backend.deleteLayer();
        QCOMPARE(backend.scene().layers.size(), 1);
        QVERIFY(!backend.scene().layers[0].raw.contains("cut"));
        QVERIFY(backend.notice().contains("unlinked"));
    }

    void cuttingForgetsPickedKeyframes() {
        QTemporaryDir dir;
        Backend backend;
        backend.open(writePicture(dir.filePath("p.png")));
        backend.select(0);
        backend.keyAll();
        backend.selectKey(0, 0, false);
        backend.setTool("cut");
        for (const QPointF p : {QPointF(5, 5), QPointF(80, 5), QPointF(80, 60), QPointF(5, 60)})
            backend.addCutPoint(p.x(), p.y());
        backend.finishCut(true);
        QVERIFY(backend.keySelection().isEmpty());
    }

    void clicksThatMoveNothingKeepRedo() {
        QTemporaryDir dir;
        Backend backend;
        backend.open(writePicture(dir.filePath("p.png")));
        backend.select(0);
        backend.setProperty("x", 10);
        backend.undo();
        QVERIFY(backend.canRedo());
        backend.beginDrag(50, 50);
        backend.endDrag();
        backend.keyAll();
        backend.undo();
        backend.selectKey(0, 0, false);
        backend.beginKeyDrag();
        backend.endKeyDrag();
        QVERIFY(backend.canRedo());
        // A drag that moves makes exactly one step.
        backend.beginDrag(50, 50);
        backend.dragTo(60, 50);
        backend.dragTo(70, 50);
        backend.endDrag();
        backend.undo();
        QCOMPARE(backend.values()["x"].toDouble(), 100.0);
    }

    void staleAiResultsAreDropped() {
        if (!Ai::instance().problem().isEmpty())
            QSKIP(qPrintable("The AI isn't set up here: " + Ai::instance().problem()));
        QTemporaryDir dir;
        Backend backend;
        backend.open(ballPicture(dir));
        backend.select(0);
        backend.duplicateLayer();
        backend.select(0);
        backend.setTool("wand");
        backend.smartSelect(200, 200, false);
        backend.select(1);  // Another layer, while it thinks.
        backend.waitForJobs();
        QVERIFY(backend.cutPoints().isEmpty());
    }

    void presetsLoopWithTheScene() {
        QTemporaryDir dir;
        const QString project = Project::open(writePicture(dir.filePath("p.png")), nullptr);
        Scene scene = Project::load(project, nullptr);
        AssetCache assets(project);
        scene.duration = 10;
        QString error;
        for (const QString &preset : Project::presets()) {
            Scene moved = scene;
            QVERIFY2(Project::applyPreset(moved, assets, 0, preset, 2, 1, &error), qPrintable(error));
            const Layer &layer = moved.layers[0];
            QVERIFY(!layer.keys.isEmpty());
            // The looping ones end where they start.
            if (preset != "fade" && preset != "spin")
                for (auto it = layer.keys.begin(); it != layer.keys.end(); ++it)
                    QCOMPARE(moved.value(layer, it.key(), 10), moved.value(layer, it.key(), 0));
        }
        Scene swaying = scene;
        Project::applyPreset(swaying, assets, 0, "sway", 0, 1, &error);
        double most = 0;
        for (double t = 0; t <= 10; t += .05)
            most = std::max(most, std::abs(swaying.value(swaying.layers[0], "rotation", t)));
        QVERIFY(qAbs(most - 4) < .01);
        Scene spinning = scene;
        Project::applyPreset(spinning, assets, 0, "spin", 0, 1, &error);
        QCOMPARE(spinning.value(spinning.layers[0], "rotation", 10), 360.0);
        Scene fading = scene;
        Project::applyPreset(fading, assets, 0, "fade", 3, 1, &error);
        QCOMPARE(fading.value(fading.layers[0], "opacity", 3), 0.0);
        QCOMPARE(fading.value(fading.layers[0], "opacity", 4), 1.0);
        QVERIFY(!Project::applyPreset(scene, assets, 0, "wobble", 0, 1, &error));
        QVERIFY(error.contains("sway"));
    }

    void aiFillsHolesWithTexture() {
        if (!Ai::instance().canInpaint())
            QSKIP("The AI's fill isn't set up here.");
        QTemporaryDir dir;
        // Stripes, with a block over them: the fill should carry the stripes on.
        QImage picture(240, 160, QImage::Format_RGB32);
        for (int y = 0; y < 160; ++y)
            for (int x = 0; x < 240; ++x)
                picture.setPixelColor(x, y, (x / 10) % 2 ? QColor(230, 220, 120) : QColor(40, 90, 200));
        QPainter block(&picture);
        block.fillRect(95, 55, 50, 50, QColor(200, 30, 30));
        block.end();
        picture.save(dir.filePath("stripes.png"));
        const QString project = Project::open(dir.filePath("stripes.png"), nullptr);
        Scene scene = Project::load(project, nullptr);
        AssetCache assets(project);
        QString error;
        QCOMPARE(Project::cutOut(scene, assets, 0, {{92, 52}, {148, 52}, {148, 108}, {92, 108}}, 0, true, &error, "block", 1,
                                 true, true), 1);
        QCOMPARE(scene.layers[1].raw["cut"].toObject()["fillWith"].toString(), QString("ai"));
        scene.layers[1].visible = false;
        const QImage filled = renderFrame(scene, assets, 0);
        // No red left, and stripes rather than a smudge: the middle row
        // still swings between the two colours.
        int reds = 0, swings = 0;
        for (int x = 100; x < 140; ++x) {
            const QColor c = filled.pixelColor(x, 80);
            reds += c.red() > 150 && c.green() < 80;
            if (x > 100 && std::abs(c.blue() - filled.pixelColor(x - 1, 80).blue()) > 40)
                ++swings;
        }
        QCOMPARE(reds, 0);
        QVERIFY2(swings >= 2, qPrintable(QString::number(swings)));
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

    void backendShapesCurvesInOneStep() {
        QTemporaryDir dir;
        Backend backend;
        backend.open(writePicture(dir.filePath("p.png")));
        backend.select(0);
        backend.keyAll();
        QCOMPARE(backend.values()["keyCurve"].toList().size(), 4);
        backend.beginGesture();
        backend.setCurve(.2, .9, .4, 1);
        backend.setCurve(.3, 1.4, .5, 1);
        backend.endGesture();
        QCOMPARE(backend.scene().layers[0].keys["x"][0].ease, QString("cubic(0.3,1.4,0.5,1)"));
        QCOMPARE(backend.scene().layers[0].keys["rotation"][0].ease, QString("cubic(0.3,1.4,0.5,1)"));
        backend.undo();  // One drag, one step.
        QCOMPARE(backend.scene().layers[0].keys["x"][0].ease, QString("inOut"));
    }

    void brushingReshapesAPiece() {
        QTemporaryDir dir;
        Backend backend;
        backend.open(writePicture(dir.filePath("p.png")));  // 200×100.
        backend.select(0);
        backend.setTool("cut");
        for (const QPointF p : {QPointF(10, 10), QPointF(50, 10), QPointF(50, 40), QPointF(10, 40)})
            backend.addCutPoint(p.x(), p.y());
        backend.finishCut(true);
        QCOMPARE(backend.selected(), 1);
        const QSizeF before = backend.assets().size(backend.scene().layers[1].source);

        // Esc leaves it as it was.
        backend.startBrush();
        QCOMPARE(backend.tool(), QString("brush"));
        backend.beginBrush(80, 25, false);
        backend.endBrush();
        backend.setTool("move");
        QVERIFY(backend.scene().layers[1].raw["cut"].toObject().contains("polygon"));

        // Paint more on to the right, rub some out at the top left, apply.
        backend.startBrush();
        backend.adjustBrush(.5);  // 12 px.
        backend.beginBrush(50, 25, false);
        backend.brushTo(90, 25);
        backend.endBrush();
        backend.beginBrush(12, 12, true);
        backend.endBrush();
        backend.finishBrush();
        backend.waitForJobs();
        QCOMPARE(backend.error(), QString());
        const Layer &piece = backend.scene().layers[1];
        const QJsonObject cut = piece.raw["cut"].toObject();
        QVERIFY(cut.contains("mask") && !cut.contains("polygon"));  // Painted, it's a mask now.
        QVERIFY(backend.assets().size(piece.source).width() > before.width() + 30);
        const QJsonArray origin = cut["origin"].toArray();
        const QImage image = backend.assets().image(piece.source);
        QCOMPARE(qAlpha(image.pixel(12 - origin[0].toInt(), 12 - origin[1].toInt())), 0);  // Rubbed out.
        QCOMPARE(qAlpha(image.pixel(85 - origin[0].toInt(), 25 - origin[1].toInt())), 255);  // Painted in.
        QCOMPARE(backend.tool(), QString("move"));
        backend.undo();
        QVERIFY(backend.scene().layers[1].raw["cut"].toObject().contains("polygon"));
    }

    void particleSettingsAreChecked() {
        Scene scene;
        QString error;
        QVERIFY(Project::addParticles(scene, "snow", {{"kind", "blah"}}, &error) < 0);
        QVERIFY(Project::addParticles(scene, "snow", {{"count", "lots"}}, &error) < 0);
        QVERIFY(error.contains("number"));
        QVERIFY(Project::addParticles(scene, "snow", {{"color", 123}}, &error) < 0);
        QVERIFY(Project::addParticles(scene, "snow", {{"spedd", 3}}, &error) < 0);
        QVERIFY(error.contains("spedd"));
        QCOMPARE(Project::addParticles(scene, "snow", {{"count", 12}, {"color", "#ff0000"}, {"twinkle", 5}}, &error), 0);
        QCOMPARE(scene.layers[0].particles["twinkle"].toDouble(), 1.0);
    }

    void slowParticlesStillMoveAndLoop() {
        QTemporaryDir dir;
        const QString project = Project::open(writePicture(dir.filePath("p.png"), {800, 400}), nullptr);
        Scene scene = Project::load(project, nullptr);
        AssetCache assets(project);
        QString error;
        // Snow at its slow default, no sway: it falls, and still loops.
        Project::addParticles(scene, "snow", {{"sway", 0}, {"count", 80}}, &error);
        QVERIFY(differences(renderFrame(scene, assets, 0), renderFrame(scene, assets, .5)) > 20);
        QCOMPARE(differences(renderFrame(scene, assets, 0), renderFrame(scene, assets, scene.duration)), 0);
        // Panned far over, there's still snow where the camera looks.
        scene.layers[0].visible = false;
        scene.camera.keys["x"] = {Key{0, 400, "linear"}, Key{scene.duration, 1000, "linear"}};
        const QImage panned = renderFrame(scene, assets, scene.duration * .9);
        int bright = 0;
        for (int y = 0; y < panned.height(); y += 2)
            for (int x = 600; x < 800; x += 2)
                bright += qGray(panned.pixel(x, y)) > 128;
        QVERIFY2(bright > 10, qPrintable(QString::number(bright)));
    }

    void backendAddsAndTunesParticles() {
        QTemporaryDir dir;
        Backend backend;
        backend.open(writePicture(dir.filePath("p.png")));
        backend.addParticles("rain");
        QCOMPARE(backend.selected(), 1);
        QCOMPARE(backend.values()["particles"].toMap()["kind"].toString(), QString("rain"));
        backend.setParticle("count", 50);
        backend.setParticle("color", "#ff0000");
        backend.setParticle("color", "not a colour");
        QVERIFY(backend.error().contains("colour"));
        // No picture to cut or split.
        backend.setTool("cut");
        QCOMPARE(backend.tool(), QString("move"));
        QVERIFY(backend.notice().contains("particle"));
        backend.splitByDepth(3);
        QVERIFY(backend.notice().contains("particle"));
        QCOMPARE(backend.scene().layers[1].particles["count"].toInt(), 50);
        QCOMPARE(backend.scene().layers[1].particles["color"].toString(), QString("#ff0000"));
        QVERIFY(!backend.error().contains("Can't find"));  // No picture isn't a missing picture.
        backend.undo();
        QCOMPARE(backend.scene().layers[1].particles["color"].toString(), QString("#c8d8ff"));
    }

    void soundPlaysAlongAndGoesIntoVideo() {
        if (QStandardPaths::findExecutable("ffmpeg").isEmpty())
            QSKIP("ffmpeg isn't installed here.");
        QTemporaryDir dir;
        // Half a second of beep every second, for six seconds.
        QProcess make;
        make.start("ffmpeg", {"-loglevel", "error", "-f", "lavfi", "-i", "aevalsrc='sin(440*2*PI*t)*lt(mod(t,1),0.5)':s=22050:d=6",
                              dir.filePath("beep.wav")});
        QVERIFY(make.waitForFinished(20000));
        Backend backend;
        backend.open(writePicture(dir.filePath("p.png")));
        QVERIFY(!backend.setAudioFile(dir.filePath("p.png")));  // Not a sound.
        QVERIFY(backend.setAudioFile(dir.filePath("beep.wav")));
        QCOMPARE(backend.audio()["source"].toString(), QString("assets/beep.wav"));
        QTRY_VERIFY(backend.audio()["peaks"].toList().size() == 600);
        const QVariantList peaks = backend.audio()["peaks"].toList();
        QVERIFY(peaks.at(50).toDouble() > .5);   // 0.33 s in: beeping.
        QVERIFY(peaks.at(110).toDouble() < .05); // 0.73 s in: quiet.
        backend.setAudioValue("offset", .5);
        QCOMPARE(backend.audio()["offset"].toDouble(), .5);
        QTRY_VERIFY(backend.audio()["peaks"].toList().size() == 600 && backend.audio()["peaks"].toList().at(50).toDouble() < .05);
        backend.flush();

        // Into an MP4 with the scene's length of sound; a GIF has none.
        const Scene scene = backend.scene();
        QString error;
        QVERIFY2(Exporter::run(scene, backend.projectPath(), dir.filePath("out.mp4"), &error, .5), qPrintable(error));
        QProcess probe;
        probe.start("ffprobe", {"-v", "error", "-show_entries", "stream=codec_type", "-of", "csv=p=0", dir.filePath("out.mp4")});
        QVERIFY(probe.waitForFinished(20000));
        QCOMPARE(QString::fromUtf8(probe.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts), (QStringList{"video", "audio"}));

        // A sound shorter than the scene doesn't cut the video short.
        Scene longer = scene;
        longer.duration = 8;  // The beep (from .5 s in) runs out at 5.5.
        QVERIFY2(Exporter::run(longer, backend.projectPath(), dir.filePath("long.mp4"), &error, .25), qPrintable(error));
        QProcess length;
        length.start("ffprobe", {"-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", dir.filePath("long.mp4")});
        QVERIFY(length.waitForFinished(20000));
        QVERIFY2(qAbs(QString::fromUtf8(length.readAllStandardOutput()).trimmed().toDouble() - 8) < .2, "the video kept its length");

        backend.removeAudio();
        QVERIFY(backend.audio().isEmpty());
        backend.undo();
        QCOMPARE(backend.audio()["offset"].toDouble(), .5);
    }

    void motionsSaveAndGoOnAnyLayer() {
        QTemporaryDir motions, dir;
        Project::setMotionDirectory(motions.path());
        Backend backend;
        backend.open(writePicture(dir.filePath("p.png")));
        backend.select(0);
        Project::applyPreset(const_cast<Scene &>(backend.scene()), backend.assets(), 0, "bounce", 0, 1, nullptr);
        backend.saveMotion("hop");
        QCOMPARE(backend.error(), QString());
        backend.saveMotion("bad/name");
        QVERIFY(!backend.error().isEmpty());
        QCOMPARE(backend.savedMotions(), QStringList{"hop"});
        // On the camera, from a second in: only what a camera has comes along.
        backend.selectCamera();
        backend.seek(1);
        backend.applyMotion("hop");
        QVERIFY(backend.scene().camera.keys.contains("y"));
        QCOMPARE(backend.scene().camera.keys["y"].first().t, 1.0);
        backend.applyMotion("nothing");
        QVERIFY(backend.error().contains("nothing"));
        Project::setMotionDirectory({});
    }

    void backendPinsAndBends() {
        QTemporaryDir dir;
        Backend backend;
        backend.open(writePicture(dir.filePath("p.png")));  // 200×100 at the canvas's own size.
        backend.select(0);
        backend.setTool("pin");
        QCOMPARE(backend.addPinAt(20, 50), 0);
        QCOMPARE(backend.addPinAt(180, 50), 1);
        QCOMPARE(backend.addPinAt(500, 500), -1);  // Off the layer.
        QCOMPARE(backend.pinPoints().size(), 2);
        QCOMPARE(backend.pinNear(181, 51, 5), 1);
        const QImage flat = renderFrame(backend.scene(), backend.assets(), 0);
        backend.keyAll();  // Pins keyframe too.
        QVERIFY(backend.scene().layers[0].keys.contains("pin1y"));
        backend.seek(1);
        backend.beginPinDrag(1, 180, 50);
        backend.pinDragTo(180, 20);
        backend.endPinDrag();
        QCOMPARE(backend.scene().value(backend.scene().layers[0], "pin1y", 1), -30.0);
        QCOMPARE(backend.scene().value(backend.scene().layers[0], "pin1y", 0), 0.0);
        QVERIFY(differences(renderFrame(backend.scene(), backend.assets(), 1), flat) > 200);
        QCOMPARE(differences(renderFrame(backend.scene(), backend.assets(), 0), flat), 0);  // Unbent, it's the same.
        // Take the first pin away: the second becomes pin 0, keys and all.
        backend.removePin(0);
        QCOMPARE(backend.scene().layers[0].pins.size(), 1);
        QCOMPARE(backend.scene().value(backend.scene().layers[0], "pin0y", 1), -30.0);
        QVERIFY(!backend.scene().layers[0].keys.contains("pin1y"));
        backend.undo();
        QCOMPARE(backend.scene().layers[0].pins.size(), 2);
    }

    void pastingKeepsTheSceneLoadable() {
        QTemporaryDir motions, dir;
        Project::setMotionDirectory(motions.path());
        Backend backend;
        backend.open(writePicture(dir.filePath("p.png")));
        backend.selectCamera();
        backend.keyAll();
        backend.saveMotion("zoomy");
        backend.select(0);
        backend.applyMotion("zoomy");  // A camera's zoom goes on as the layer's scale.
        QVERIFY(backend.scene().layers[0].keys.contains("scale"));
        QVERIFY(!backend.scene().layers[0].keys.contains("zoom"));
        // Pin keys on a layer without those pins are left out.
        backend.setTool("pin");
        backend.addPinAt(50, 50);
        backend.setTool("move");
        backend.keyAll();
        backend.copyKeys();
        backend.duplicateLayer();
        backend.setTool("pin");
        backend.removePin(0);
        backend.setTool("move");
        backend.pasteKeys();
        QVERIFY(!backend.scene().layers[1].keys.contains("pin0x"));
        backend.flush();
        QString error;
        Project::load(backend.projectPath(), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        // Only pin keys: not a motion.
        backend.select(0);
        backend.clearKeySelection();
        QVERIFY(!Project::saveMotion("pins", {KeyClip{"pin0x", Key{0, 1}}}, &error));
        QVERIFY(error.contains("pin"));
        Project::setMotionDirectory({});
    }

    void addingAPinToABentLayerKeepsItsPoint() {
        QTemporaryDir dir;
        Backend backend;
        backend.open(writePicture(dir.filePath("p.png")));
        backend.select(0);
        backend.setTool("pin");
        backend.addPinAt(20, 50);
        backend.addPinAt(180, 50);
        backend.beginPinDrag(1, 180, 50);
        backend.pinDragTo(180, 20);
        backend.endPinDrag();
        const QImage before = renderFrame(backend.scene(), backend.assets(), 0);
        QCOMPARE(backend.addPinAt(120, 40), 2);
        // The pinned point stays put; the rest can settle a little, as every
        // pin has a say in the bend (here about 1% of the picture moves).
        const int changed = differences(renderFrame(backend.scene(), backend.assets(), 0), before);
        QVERIFY2(changed < 200 * 100 / 30, qPrintable(QString::number(changed)));
        QVERIFY(QLineF(backend.pinPoints().at(2).toPointF(), QPointF(120, 40)).length() < .5);
    }

    void backendEditsTheCamera() {
        QTemporaryDir dir;
        Backend backend;
        backend.open(writePicture(dir.filePath("p.png")));  // 200×100.
        backend.selectCamera();
        QVERIFY(backend.cameraSelected());
        QCOMPARE(backend.selected(), -1);
        QCOMPARE(backend.values()["name"].toString(), QString("Camera"));
        backend.multiply("scale", 2);  // Scale means zoom for the camera.
        QCOMPARE(backend.values()["zoom"].toDouble(), 2.0);
        // Dragging pans: the picture follows the hand, at the zoom.
        backend.beginDrag(100, 50);
        backend.dragTo(120, 50);
        backend.endDrag();
        QCOMPARE(backend.values()["x"].toDouble(), 90.0);
        backend.keyAll();
        QCOMPARE(backend.sceneInfo()["cameraKeys"].toList().size(), 1);
        backend.seek(1);
        backend.adjust("rotation", 10);
        QCOMPARE(backend.scene().camera.keys["rotation"].size(), 2);
        // Its keyframes retime like a layer's.
        backend.selectKey(Scene::CameraIndex, 1, false);
        backend.beginKeyDrag();
        backend.dragKeys(.5);
        backend.endKeyDrag();
        QCOMPARE(backend.scene().camera.keys["rotation"].last().t, 1.5);
        backend.undo();
        QCOMPARE(backend.scene().camera.keys["rotation"].last().t, 1.0);
        // < and > walk the camera's keys when it's selected.
        backend.seek(0);
        backend.jumpKey(1);
        QCOMPARE(backend.time(), 1.0);
        // Selecting a layer leaves the camera.
        backend.select(0);
        QVERIFY(!backend.cameraSelected());
        backend.setFollow(.25);
        QCOMPARE(backend.values()["follow"].toDouble(), .25);
        QVERIFY(backend.values()["ownFollow"].toBool());
        backend.setFollow(-1);
        QCOMPARE(backend.values()["follow"].toDouble(), 1.0);
    }

    void cuttingThroughTheCameraLandsOnThePicture() {
        QTemporaryDir dir;
        const QString project = Project::open(writePicture(dir.filePath("p.png")), nullptr);
        Scene scene = Project::load(project, nullptr);
        AssetCache assets(project);
        scene.camera.base["zoom"] = 2;  // Zoomed into the centre (100, 50).
        QString error;
        // The red square (10..70, 10..50 in the picture) at 2× around the
        // centre is canvas -80..40, -30..50; cut a bit of it on screen.
        QCOMPARE(Project::cutOut(scene, assets, 0, {{0, 0}, {20, 0}, {20, 20}, {0, 20}}, 0, true, &error, "bit", 0, false), 1);
        const QRectF piece = QPolygonF(Project::cutOutline(scene, assets, 1, 0)).boundingRect();
        QVERIFY(qAbs(piece.width() - 20) < .01);
        const QJsonArray origin = scene.layers[1].raw["cut"].toObject()["origin"].toArray();
        QCOMPARE(origin[0].toInt(), 50);  // Canvas 0 is picture 50 at 2× around 100.
        QCOMPARE(origin[1].toInt(), 25);
    }

    void depthShrinksTowardTheVanishingPoint() {
        QTemporaryDir dir;
        const QString project = Project::open(writePicture(dir.filePath("p.png")), nullptr);  // 200×100.
        Scene scene = Project::load(project, nullptr);
        AssetCache assets(project);
        const QTransform flat = layerTransform(scene, assets, 0, 0);
        scene.layers[0].base["x"] = 160;  // To the right of the vanishing point, the canvas centre.
        const QTransform placed = layerTransform(scene, assets, 0, 0);
        QCOMPARE(placed.map(QPointF(0, 0)), flat.map(QPointF(0, 0)) + QPointF(60, 0));
        // As far back as the focal distance (the canvas width), it's half
        // size, halfway to the vanishing point.
        scene.layers[0].base["z"] = 200;
        const QTransform far = layerTransform(scene, assets, 0, 0);
        QCOMPARE(std::hypot(far.m11(), far.m12()), std::hypot(placed.m11(), placed.m12()) / 2);
        const QPointF pivot(100, 50);  // The image centre, its anchor.
        QCOMPARE(placed.map(pivot), QPointF(160, 50));
        QCOMPARE(far.map(pivot), QPointF(130, 50));
        // Children are as far back as their parent.
        Layer child;
        child.name = "child";
        child.source = scene.layers[0].source;
        child.parent = scene.layers[0].name;
        scene.layers.append(child);
        QCOMPARE(depthOf(scene, 1, 0), 200.0);
        // Perspective goes through scene.json; z is left out until it's used.
        scene.vanish = QPointF(150, 30);
        scene.distance = 900;
        const QJsonObject json = scene.toJson();
        QCOMPARE(json["perspective"].toObject()["distance"].toDouble(), 900.0);
        QString error;
        const Scene again = Scene::fromJson(json, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(again.vanishPoint(), QPointF(150, 30));
        QCOMPARE(again.focalDistance(), 900.0);
        QVERIFY(!json["layers"].toArray()[1].toObject().contains("z"));
        QVERIFY(!Scene().toJson().contains("perspective"));
        Scene keyed = again;
        keyed.keyAll(1, 0);
        QVERIFY(!keyed.layers[1].keys.contains("z"));
        keyed.keyAll(0, 0);  // Set back, so it's in use.
        QVERIFY(keyed.layers[0].keys.contains("z"));
    }

    void backendDragsLayersSetBack() {
        QTemporaryDir dir;
        Backend backend;
        backend.open(writePicture(dir.filePath("p.png")));  // 200×100, centred.
        backend.select(0);
        backend.setProperty("z", 200);  // Half size.
        const double x = backend.values()["x"].toDouble();
        backend.beginDrag(100, 50);
        backend.dragTo(110, 50);
        backend.endDrag();
        // It stays under the hand: 10 canvas pixels are 20 that far back.
        QCOMPARE(backend.values()["x"].toDouble(), x + 20);
        backend.setSceneValue("distance", 400);
        QCOMPARE(backend.sceneInfo()["distance"].toDouble(), 400.0);
        backend.setSceneValue("vanishY", 10);
        QCOMPARE(backend.scene().vanishPoint(), QPointF(100, 10));
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
        QVERIFY(Exporter::run(scene, project, dir.filePath("small"), &error, .5));
        QCOMPARE(QImage(dir.filePath("small/0001.png")).size(), QSize(100, 50));
        QVERIFY(!Exporter::run(scene, project, dir.filePath("none"), &error, 0));
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
