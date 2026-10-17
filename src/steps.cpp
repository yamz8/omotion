#include "steps.h"

#include <QJsonArray>
#include <QJsonObject>

#include "project.h"
#include "world.h"

namespace Steps {

bool Step::slow(const QString &step, const QString &action) {
    return step == "world" || step == "shape" || (step == "fill" && action == "ai");
}

namespace {
QString fillWords(const QJsonObject &cut) {
    if (!cut.value("punch").toBool(true))
        return "copied out: no hole";
    if (!cut.value("fill").toBool(true))
        return "the hole left open";
    return cut.value("fillWith").toString() == "ai" ? "the hole painted in by the AI" : "the hole filled smoothly";
}

// Why a world is out of date, or empty when it isn't (or can't be told:
// worlds made before they kept track count as up to date).
QString worldStale(const Scene &scene) {
    const QJsonObject world = scene.raw.value("world").toObject();
    if (!World::info(scene).valid || !world.contains("pieces"))
        return {};
    const int root = scene.indexOf(World::info(scene).picture);
    return World::madeFrom(scene, root) != world.value("pieces").toArray()
               ? QString("pieces were cut or reshaped since, so what's behind them needs filling in again")
               : QString();
}

QString shapeStale(const Scene &scene, int index) {
    const QJsonObject shape = scene.layers.at(index).raw.value("shape").toObject();
    if (shape.isEmpty())
        return {};
    if (shape.contains("from") && shape.value("from").toString() != World::cutCode(scene.layers.at(index)))
        return "the piece was reshaped since";
    const QJsonValue made = scene.raw.value("world").toObject().value("made");
    if (shape.contains("world") && !shape.value("world").isNull() && shape.value("world") != made)
        return "the world was made again since";
    return {};
}
}

QList<Step> of(const Scene &scene, AssetCache &assets, int index) {
    Q_UNUSED(assets)
    QList<Step> steps;
    if (index < 0 || index >= scene.layers.size())
        return steps;
    const Layer &layer = scene.layers.at(index);
    if (!layer.particles.isEmpty())
        return steps;
    const QJsonObject cut = layer.raw.value("cut").toObject();
    if (!cut.isEmpty()) {
        Step step;
        step.name = "cut";
        step.title = "Cut from " + cut.value("from").toString();
        const bool painted = cut.contains("mask");
        step.detail = painted ? QString("a painted shape") : QString("%1 points").arg(cut.value("polygon").toArray().size());
        if (cut.value("feather").toDouble() > 0)
            step.detail += QString(", soft %1 px").arg(cut.value("feather").toDouble());
        step.actions << "reshape";
        steps << step;

        Step fill;
        fill.name = "fill";
        fill.title = "Behind it, in " + cut.value("from").toString();
        fill.detail = fillWords(cut);
        // A painted shape is redrawn by the brush, which keeps its fill.
        if (cut.value("punch").toBool(true) && !painted && Project::isCut(scene, index))
            fill.actions << "ai" << "smooth" << "none";
        steps << fill;
    }

    const World::Info world = World::info(scene);
    const bool inWorld = world.valid && World::layers(scene).contains(index);
    const bool root = layer.name == world.picture;
    if (inWorld) {
        Step step;
        step.name = "world";
        step.title = "World from " + world.picture;
        step.detail = "distances, lens and ground read by the AI";
        step.why = worldStale(scene);
        step.stale = !step.why.isEmpty();
        step.actions << "again";
        steps << step;
    } else if (!world.valid && cut.isEmpty() && !layer.source.isEmpty()) {
        Step step;
        step.name = "world";
        step.title = "Not a world yet";
        step.detail = "the AI can read this picture in 3D";
        step.done = false;
        step.actions << "make";
        steps << step;
    }
    if (inWorld && !root) {
        Step step;
        step.name = "shape";
        const bool shaped = !layer.raw.value("shape").toObject().isEmpty();
        step.done = shaped;
        step.title = shaped ? "A 3D shape" : "Flat in the world";
        step.detail = shaped ? "made by the AI from the cut-out, fitted to the picture" : "the AI can give it a shape that turns";
        step.why = shapeStale(scene, index);
        step.stale = !step.why.isEmpty();
        step.actions << (shaped ? "again" : "make");
        steps << step;
    }
    return steps;
}

bool run(Scene &scene, AssetCache &assets, int index, const QString &step, const QString &action, QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (index < 0 || index >= scene.layers.size())
        return fail("There's no such layer.");
    if (step == "fill") {
        const int fill = action == "ai" ? 2 : action == "smooth" ? 1 : action == "none" ? 0 : -1;
        if (fill < 0)
            return fail("Fill it with ai, smooth or none.");
        if (!Project::isCut(scene, index) || scene.layers.at(index).raw.value("cut").toObject().contains("mask"))
            return fail(QString("%1 wasn't cut by an outline, so there's no hole to fill again.").arg(scene.layers.at(index).name));
        double feather = 0;
        const QPolygonF outline = Project::cutOutline(scene, assets, index, 0, &feather);
        return Project::reshapeCut(scene, assets, index, outline, 0, feather, error, fill);
    }
    if (step == "world") {
        const QJsonObject world = scene.raw.value("world").toObject();
        const double horizon = world.value("horizon").toDouble(-1);
        const int root = World::info(scene).valid ? scene.indexOf(World::info(scene).picture) : index;
        return World::build(scene, assets, root, error, horizon);
    }
    if (step == "shape")
        return World::shapeObject(scene, assets, index, error);
    if (step == "cut")
        return fail("A cut is reshaped in the window (R), or with omotion cut.");
    return fail(QString("There's no step called %1.").arg(step));
}

QList<QPair<int, QString>> stale(const Scene &scene, AssetCache &assets) {
    QList<QPair<int, QString>> list;
    const World::Info world = World::info(scene);
    if (!world.valid)
        return list;
    const int root = scene.indexOf(world.picture);
    const bool remake = !worldStale(scene).isEmpty();
    if (remake)
        list << qMakePair(root, QString("world"));
    for (const int index : World::layers(scene)) {
        if (index == root || scene.layers.at(index).raw.value("shape").toObject().isEmpty())
            continue;
        // Made again, the world is new to every shape in it.
        if (remake || !shapeStale(scene, index).isEmpty())
            list << qMakePair(index, QString("shape"));
    }
    Q_UNUSED(assets)
    return list;
}

bool redoStale(Scene &scene, AssetCache &assets, QString *error, QStringList *done) {
    for (const auto &[index, step] : stale(scene, assets)) {
        if (!run(scene, assets, index, step, "again", error))
            return false;
        if (done)
            *done << (step == "world" ? QString("the world") : QString("%1's shape").arg(scene.layers.at(index).name));
    }
    return true;
}

}
