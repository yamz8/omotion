#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QProcess>
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
