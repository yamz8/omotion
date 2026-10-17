#include "gltf.h"

#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QVector3D>
#include <cmath>

#include "world.h"

namespace Gltf {

namespace {

// The binary buffer and the JSON that describes it, built up together.
struct Builder {
    QByteArray bin;
    QJsonArray views, accessors, meshes, materials, textures, images, samplers, nodes, animations;

    void align() {
        while (bin.size() % 4)
            bin.append('\0');
    }
    int view(const QByteArray &bytes, int target = 0) {
        align();
        QJsonObject v{{"buffer", 0}, {"byteOffset", bin.size()}, {"byteLength", bytes.size()}};
        if (target)
            v["target"] = target;
        bin.append(bytes);
        views.append(v);
        return int(views.size()) - 1;
    }
    int accessor(const QByteArray &bytes, int componentType, int count, const QString &type, int target = 0,
                 const QJsonArray &min = {}, const QJsonArray &max = {}) {
        QJsonObject a{{"bufferView", view(bytes, target)}, {"componentType", componentType}, {"count", count}, {"type", type}};
        if (!min.isEmpty())
            a["min"] = min;
        if (!max.isEmpty())
            a["max"] = max;
        accessors.append(a);
        return int(accessors.size()) - 1;
    }
    // An unlit material wearing a picture: the picture already has its light.
    int material(const QImage &picture, const QString &name) {
        // See-through pictures as PNG; solid ones as JPEG, a tenth the size.
        const bool clear = picture.hasAlphaChannel() && !picture.convertToFormat(QImage::Format_RGB32).isNull() && [&] {
            const QImage argb = picture.convertToFormat(QImage::Format_ARGB32);
            for (int y = 0; y < argb.height(); y += 4)
                for (int x = 0; x < argb.width(); x += 4)
                    if (qAlpha(argb.pixel(x, y)) < 255)
                        return true;
            return false;
        }();
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        picture.save(&buffer, clear ? "PNG" : "JPG", clear ? -1 : 92);
        images.append(QJsonObject{{"bufferView", view(bytes)}, {"mimeType", clear ? "image/png" : "image/jpeg"}, {"name", name}});
        if (samplers.isEmpty())
            samplers.append(QJsonObject{{"magFilter", 9729}, {"minFilter", 9987}, {"wrapS", 33071}, {"wrapT", 33071}});
        textures.append(QJsonObject{{"source", images.size() - 1}, {"sampler", 0}});
        materials.append(QJsonObject{
            {"name", name},
            {"pbrMetallicRoughness", QJsonObject{{"baseColorTexture", QJsonObject{{"index", textures.size() - 1}}},
                                                 {"metallicFactor", 0}, {"roughnessFactor", 1}}},
            {"alphaMode", clear ? "BLEND" : "OPAQUE"},
            {"doubleSided", true},
            {"extensions", QJsonObject{{"KHR_materials_unlit", QJsonObject{}}}}});
        return int(materials.size()) - 1;
    }
    // A mesh from positions, texture coordinates (top-left origin) and
    // triangles.
    int mesh(const QList<QVector3D> &points, const QList<QPointF> &uv, const QList<quint32> &triangles, int material,
             const QString &name) {
        QByteArray p, t, i;
        QVector3D low(1e30f, 1e30f, 1e30f), high(-1e30f, -1e30f, -1e30f);
        for (const QVector3D &point : points) {
            for (int k = 0; k < 3; ++k) {
                const float v = point[k];
                p.append(reinterpret_cast<const char *>(&v), 4);
                low[k] = std::min(low[k], v);
                high[k] = std::max(high[k], v);
            }
        }
        for (const QPointF &coord : uv) {
            const float u = float(coord.x()), v = float(coord.y());
            t.append(reinterpret_cast<const char *>(&u), 4);
            t.append(reinterpret_cast<const char *>(&v), 4);
        }
        for (const quint32 index : triangles)
            i.append(reinterpret_cast<const char *>(&index), 4);
        const int position = accessor(p, 5126, int(points.size()), "VEC3", 34962, {low.x(), low.y(), low.z()}, {high.x(), high.y(), high.z()});
        const int coords = accessor(t, 5126, int(uv.size()), "VEC2", 34962);
        const int indices = accessor(i, 5125, int(triangles.size()), "SCALAR", 34963);
        meshes.append(QJsonObject{{"name", name},
                                  {"primitives", QJsonArray{QJsonObject{{"attributes", QJsonObject{{"POSITION", position}, {"TEXCOORD_0", coords}}},
                                                                        {"indices", indices},
                                                                        {"material", material}}}}});
        return int(meshes.size()) - 1;
    }
    int node(const QJsonObject &node) {
        nodes.append(node);
        return int(nodes.size()) - 1;
    }
};

QByteArray floats(const QList<float> &values) {
    return QByteArray(reinterpret_cast<const char *>(values.constData()), values.size() * qsizetype(sizeof(float)));
}

QJsonArray json(const QVector3D &v) { return {v.x(), v.y(), v.z()}; }
QJsonArray json(const QQuaternion &q) { return {q.x(), q.y(), q.z(), q.scalar()}; }

// Animation channels from per-frame samples of a node's place, kept only
// where it moves.
struct Track {
    int node = -1;
    QList<QVector3D> translation;
    QList<QQuaternion> rotation;
    QList<QVector3D> scale;
};

void animate(Builder &b, const QList<Track> &tracks, const QList<float> &times, const QString &name) {
    QJsonArray channels, samplers;
    int input = -1;
    auto moves = [](const auto &list) {
        for (const auto &v : list)
            if (!qFuzzyCompare(QVector4D(v) + QVector4D(1, 1, 1, 1), QVector4D(list.first()) + QVector4D(1, 1, 1, 1)))
                return true;
        return false;
    };
    auto add = [&](int node, const QString &path, const QByteArray &values, const QString &type, int count) {
        if (input < 0)
            input = b.accessor(floats(times), 5126, int(times.size()), "SCALAR", 0, {times.first()}, {times.last()});
        const int output = b.accessor(values, 5126, count, type);
        samplers.append(QJsonObject{{"input", input}, {"output", output}, {"interpolation", "LINEAR"}});
        channels.append(QJsonObject{{"sampler", samplers.size() - 1}, {"target", QJsonObject{{"node", node}, {"path", path}}}});
    };
    for (const Track &track : tracks) {
        if (moves(track.translation)) {
            QList<float> v;
            for (const QVector3D &p : track.translation)
                v << p.x() << p.y() << p.z();
            add(track.node, "translation", floats(v), "VEC3", int(track.translation.size()));
        }
        if (!track.rotation.isEmpty()) {
            bool turns = false;
            for (const QQuaternion &q : track.rotation)
                turns |= !qFuzzyCompare(q, track.rotation.first());
            if (turns) {
                QList<float> v;
                QQuaternion last = track.rotation.first();
                for (QQuaternion q : track.rotation) {
                    // The short way round, so it doesn't spin between samples.
                    if (QQuaternion::dotProduct(q, last) < 0)
                        q = -q;
                    last = q;
                    v << q.x() << q.y() << q.z() << q.scalar();
                }
                add(track.node, "rotation", floats(v), "VEC4", int(track.rotation.size()));
            }
        }
        if (!track.scale.isEmpty() && moves(track.scale)) {
            QList<float> v;
            for (const QVector3D &s : track.scale)
                v << s.x() << s.y() << s.z();
            add(track.node, "scale", floats(v), "VEC3", int(track.scale.size()));
        }
    }
    if (!channels.isEmpty())
        b.animations.append(QJsonObject{{"name", name}, {"channels", channels}, {"samplers", samplers}});
}

// A world mesh as glTF wants it.
int worldMesh(Builder &b, const World::Mesh &mesh, int material, const QString &name) {
    const float *v = reinterpret_cast<const float *>(mesh.vertices.constData());
    const qsizetype count = mesh.vertices.size() / qsizetype(mesh.stride * sizeof(float));
    QList<QVector3D> points;
    QList<QPointF> uv;
    for (qsizetype k = 0; k < count; ++k) {
        points << QVector3D(v[k * mesh.stride], v[k * mesh.stride + 1], v[k * mesh.stride + 2]);
        // Meshes count v up from the bottom; glTF down from the top.
        uv << QPointF(v[k * mesh.stride + 3], 1 - v[k * mesh.stride + 4]);
    }
    const quint32 *i = reinterpret_cast<const quint32 *>(mesh.indices.constData());
    return b.mesh(points, uv, QList<quint32>(i, i + mesh.indices.size() / 4), material, name);
}

}

bool write(const Scene &scene, AssetCache &assets, const QString &path, QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    Builder b;
    QJsonArray roots;
    QList<Track> tracks;
    QList<float> times;
    const int frames = std::max(1, scene.frameCount());
    for (int f = 0; f < frames; ++f)
        times << float(scene.frameTime(f));
    QJsonObject camera;
    int cameraNode = -1;
    Track cameraTrack;

    const World::Info world = World::info(scene);
    if (world.valid) {
        // Each piece of the world at rest, moved by its node.
        QList<int> indices = World::layers(scene);
        if (!world.farDepth.isEmpty())
            indices.prepend(World::FarLayer);
        QHash<int, int> nodeOf;
        for (const int index : indices) {
            const World::Mesh mesh = World::mesh(scene, assets, index);
            if (mesh.isNull())
                continue;
            const QString name = index == World::FarLayer ? QString("behind") : scene.layers.at(index).name;
            const QString picture = index == World::FarLayer ? world.farPicture : World::texture(scene, assets, index);
            const QImage image = assets.image(picture);
            if (image.isNull())
                return fail(QString("Couldn't read %1.").arg(picture));
            const int material = b.material(image, name);
            const int node = b.node(QJsonObject{{"name", name}, {"mesh", worldMesh(b, mesh, material, name)}});
            roots.append(node);
            nodeOf.insert(index, node);
            Track track;
            track.node = node;
            tracks << track;
        }
        for (int f = 0; f < frames; ++f) {
            const World::Frame now = World::frame(scene, assets, times.at(f));
            for (Track &track : tracks) {
                const int index = nodeOf.key(track.node, World::FarLayer - 1);
                World::Placement place;
                for (const World::Placement &p : now.layers)
                    if (p.layer == index)
                        place = p;
                track.translation << place.position;
                track.rotation << place.rotation;
                track.scale << QVector3D(place.scale, place.scale, place.scale);
            }
            cameraTrack.translation << now.cameraPosition;
            cameraTrack.rotation << now.cameraRotation;
        }
        const World::Frame first = World::frame(scene, assets, 0);
        camera = QJsonObject{{"type", "perspective"},
                             {"perspective", QJsonObject{{"yfov", first.fov * M_PI / 180}, {"aspectRatio", double(scene.width) / scene.height},
                                                         {"znear", .05}, {"zfar", 5000}}}};
    } else {
        // A flat scene: each layer a card in the canvas's plane, a pixel a
        // centimetre, y up; the top layer nearest.
        const int flip = b.node(QJsonObject{{"name", "canvas"}, {"scale", QJsonArray{.01, -.01, .01}}});
        roots.append(flip);
        QJsonArray children;
        for (int i = 0; i < scene.layers.size(); ++i) {
            const Layer &layer = scene.layers.at(i);
            if (!layer.particles.isEmpty() || !layer.visible)
                continue;
            const QImage image = assets.image(layer.source);
            if (image.isNull())
                continue;
            const QSizeF size = assets.size(layer.source);
            const QList<QVector3D> corners{{0, 0, 0}, {float(size.width()), 0, 0}, {float(size.width()), float(size.height()), 0}, {0, float(size.height()), 0}};
            const int material = b.material(image.convertToFormat(QImage::Format_ARGB32), layer.name);
            const int mesh = b.mesh(corners, {{0, 0}, {1, 0}, {1, 1}, {0, 1}}, {0, 1, 2, 0, 2, 3}, material, layer.name);
            const int node = b.node(QJsonObject{{"name", layer.name}, {"mesh", mesh}});
            children.append(node);
            Track track;
            track.node = node;
            for (int f = 0; f < frames; ++f) {
                // The layer's place on the canvas, through the camera, as a
                // move, a turn and a stretch.
                const QTransform m = layerTransform(scene, assets, i, times.at(f));
                const double sx = std::hypot(m.m11(), m.m12());
                const double sy = sx > 0 ? (m.m11() * m.m22() - m.m12() * m.m21()) / sx : 0;
                const double angle = std::atan2(m.m12(), m.m11()) * 180 / M_PI;
                track.translation << QVector3D(float(m.dx()), float(m.dy()), float(i));
                track.rotation << QQuaternion::fromAxisAndAngle(0, 0, 1, float(angle));
                track.scale << QVector3D(float(sx), float(sy), 1);
            }
            tracks << track;
        }
        QJsonObject canvas = b.nodes.at(flip).toObject();
        canvas["children"] = children;
        b.nodes[flip] = canvas;
        const double yfov = 2 * std::atan(1.0 / 2);  // The canvas's height at its own height away.
        const QVector3D eye(float(scene.width / 2.0 * .01), float(-scene.height / 2.0 * .01), float(scene.height * .01));
        for (int f = 0; f < frames; ++f) {
            cameraTrack.translation << eye;
            cameraTrack.rotation << QQuaternion();
        }
        camera = QJsonObject{{"type", "perspective"},
                             {"perspective", QJsonObject{{"yfov", yfov}, {"aspectRatio", double(scene.width) / scene.height},
                                                         {"znear", .01}, {"zfar", 1000}}}};
    }
    // Every node starts where its first frame has it.
    for (const Track &track : tracks) {
        QJsonObject node = b.nodes.at(track.node).toObject();
        node["translation"] = json(track.translation.first());
        node["rotation"] = json(track.rotation.first());
        if (!track.scale.isEmpty())
            node["scale"] = json(track.scale.first());
        b.nodes[track.node] = node;
    }
    cameraNode = b.node(QJsonObject{{"name", "camera"}, {"camera", 0},
                                    {"translation", json(cameraTrack.translation.first())},
                                    {"rotation", json(cameraTrack.rotation.first())}});
    cameraTrack.node = cameraNode;
    roots.append(cameraNode);
    tracks << cameraTrack;
    if (frames > 1)
        animate(b, tracks, times, "scene");

    b.align();
    QJsonObject gltf{{"asset", QJsonObject{{"version", "2.0"}, {"generator", "omotion"}}},
                     {"extensionsUsed", QJsonArray{"KHR_materials_unlit"}},
                     {"scene", 0},
                     {"scenes", QJsonArray{QJsonObject{{"name", "omotion"}, {"nodes", roots}}}},
                     {"nodes", b.nodes},
                     {"cameras", QJsonArray{camera}},
                     {"buffers", QJsonArray{QJsonObject{{"byteLength", b.bin.size()}}}},
                     {"bufferViews", b.views},
                     {"accessors", b.accessors}};
    if (!b.meshes.isEmpty()) {
        gltf["meshes"] = b.meshes;
        gltf["materials"] = b.materials;
        gltf["textures"] = b.textures;
        gltf["images"] = b.images;
        gltf["samplers"] = b.samplers;
    }
    if (!b.animations.isEmpty())
        gltf["animations"] = b.animations;
    QByteArray text = QJsonDocument(gltf).toJson(QJsonDocument::Compact);
    while (text.size() % 4)
        text.append(' ');

    // GLB: a header, then the JSON and the binary, each a chunk.
    QByteArray glb;
    auto u32 = [&glb](quint32 v) { glb.append(reinterpret_cast<const char *>(&v), 4); };
    u32(0x46546C67);  // "glTF"
    u32(2);
    u32(quint32(12 + 8 + text.size() + 8 + b.bin.size()));
    u32(quint32(text.size()));
    u32(0x4E4F534A);  // "JSON"
    glb.append(text);
    u32(quint32(b.bin.size()));
    u32(0x004E4942);  // "BIN"
    glb.append(b.bin);
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly) || out.write(glb) != glb.size() || !out.commit())
        return fail(QString("Couldn't write %1.").arg(path));
    return true;
}

}
