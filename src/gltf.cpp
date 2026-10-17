#include "gltf.h"

#include <QBuffer>
#include <QDir>
#include <QFileInfo>
#include <QUrl>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QVector3D>
#include <cmath>

#include "modelfile.h"
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
    // A plain colour, lit: an OBJ model's material without a picture.
    int colour(const QColor &c, const QString &name) {
        materials.append(QJsonObject{
            {"name", name},
            {"pbrMetallicRoughness", QJsonObject{{"baseColorFactor", QJsonArray{c.redF(), c.greenF(), c.blueF(), 1}},
                                                 {"metallicFactor", 0}, {"roughnessFactor", .8}}},
            {"doubleSided", true}});
        return int(materials.size()) - 1;
    }
    // A picture as an OBJ model's material, lit as models are.
    int litPicture(const QImage &picture, const QString &name) {
        const int unlit = material(picture, name);
        QJsonObject m = materials.at(unlit).toObject();
        m.remove("extensions");
        QJsonObject pbr = m.value("pbrMetallicRoughness").toObject();
        pbr["roughnessFactor"] = .8;
        m["pbrMetallicRoughness"] = pbr;
        materials[unlit] = m;
        return unlit;
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

// Every texture a material names ("baseColorTexture", "normalTexture", and
// those in its extensions), moved along by where the model's textures start.
QJsonValue moveTextures(const QJsonValue &value, int by) {
    if (value.isArray()) {
        QJsonArray list;
        for (const QJsonValue &v : value.toArray())
            list.append(moveTextures(v, by));
        return list;
    }
    if (!value.isObject())
        return value;
    QJsonObject object = value.toObject();
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (it.key().endsWith("Texture") && it.value().isObject()) {
            QJsonObject texture = moveTextures(it.value(), by).toObject();
            texture["index"] = texture.value("index").toInt() + by;
            it.value() = texture;
        } else
            it.value() = moveTextures(it.value(), by);
    }
    return object;
}

// An imported glTF or GLB copied in: its buffers into ours, and every index
// into its lists moved along by where they start in ours. Its skins (it
// stands in its rest pose), cameras and animations stay behind. Adds the
// roots of its scene to roots.
bool copyGltf(Builder &b, const QString &path, QJsonArray *roots, QStringList *extensions, QString *error) {
    auto fail = [error, path](const QString &why) {
        if (error)
            *error = QString("%1: %2").arg(QFileInfo(path).fileName(), why);
        return false;
    };
    const ModelFile::Gltf file = ModelFile::readGltf(path);
    if (file.json.isEmpty() || !file.error.isEmpty())
        return fail(file.error);
    const QJsonObject &gltf = file.json;
    for (const QJsonValue &required : gltf.value("extensionsRequired").toArray())
        if (!required.toString().startsWith("KHR_materials_") && required.toString() != "KHR_texture_transform" &&
            required.toString() != "KHR_mesh_quantization")
            return fail(QString("it's packed with %1, which omotion can't copy").arg(required.toString()));
    const QDir beside = QFileInfo(path).absoluteDir();
    auto load = [&](const QString &uri, QByteArray *out) {
        if (uri.startsWith("data:")) {
            *out = QByteArray::fromBase64(uri.mid(uri.indexOf(',') + 1).toLatin1());
            return true;
        }
        QFile f(beside.filePath(QUrl::fromPercentEncoding(uri.toUtf8())));
        if (!f.open(QIODevice::ReadOnly))
            return false;
        *out = f.readAll();
        return true;
    };
    const QList<QByteArray> &data = file.buffers;
    const int views = int(b.views.size()), accessors = int(b.accessors.size()), images = int(b.images.size()),
              samplers = int(b.samplers.size()), textures = int(b.textures.size()), materials = int(b.materials.size()),
              meshes = int(b.meshes.size()), nodes = int(b.nodes.size());
    for (const QJsonValue &v : gltf.value("bufferViews").toArray()) {
        const QJsonObject view = v.toObject();
        const QByteArray &from = data.value(view.value("buffer").toInt());
        const qsizetype offset = view.value("byteOffset").toInteger(), length = view.value("byteLength").toInteger();
        if (offset < 0 || length < 0 || offset + length > from.size())
            return fail("its buffers are shorter than it says");
        b.view(from.mid(offset, length), view.value("target").toInt());
        if (view.contains("byteStride")) {
            QJsonObject copy = b.views.last().toObject();
            copy["byteStride"] = view.value("byteStride");
            b.views.last() = copy;
        }
    }
    for (const QJsonValue &v : gltf.value("accessors").toArray()) {
        QJsonObject accessor = v.toObject();
        if (accessor.contains("bufferView"))
            accessor["bufferView"] = accessor.value("bufferView").toInt() + views;
        if (accessor.contains("sparse")) {
            QJsonObject sparse = accessor.value("sparse").toObject();
            for (const QString &part : {"indices", "values"}) {
                QJsonObject p = sparse.value(part).toObject();
                p["bufferView"] = p.value("bufferView").toInt() + views;
                sparse[part] = p;
            }
            accessor["sparse"] = sparse;
        }
        b.accessors.append(accessor);
    }
    for (const QJsonValue &v : gltf.value("images").toArray()) {
        QJsonObject image = v.toObject();
        if (image.contains("bufferView"))
            image["bufferView"] = image.value("bufferView").toInt() + views;
        else {
            // Pictures beside it go inside the GLB.
            const QString uri = image.value("uri").toString();
            QByteArray picture;
            if (!load(uri, &picture))
                return fail(QString("couldn't read its picture %1").arg(uri));
            const QString suffix = QFileInfo(uri).suffix().toLower();
            image.remove("uri");
            if (!image.contains("mimeType"))
                image["mimeType"] = uri.startsWith("data:") ? uri.mid(5, uri.indexOf(';') - 5)
                                    : suffix == "png"       ? QString("image/png")
                                    : suffix == "webp"      ? QString("image/webp")
                                                            : QString("image/jpeg");
            image["bufferView"] = b.view(picture);
        }
        b.images.append(image);
    }
    for (const QJsonValue &v : gltf.value("samplers").toArray())
        b.samplers.append(v);
    for (const QJsonValue &v : gltf.value("textures").toArray()) {
        QJsonObject texture = v.toObject();
        if (texture.contains("source"))
            texture["source"] = texture.value("source").toInt() + images;
        if (texture.contains("sampler"))
            texture["sampler"] = texture.value("sampler").toInt() + samplers;
        // Pictures in other formats (WebP, Basis) name theirs in an extension.
        QJsonObject more = texture.value("extensions").toObject();
        for (auto it = more.begin(); it != more.end(); ++it) {
            QJsonObject e = it.value().toObject();
            if (e.contains("source"))
                e["source"] = e.value("source").toInt() + images;
            it.value() = e;
        }
        if (!more.isEmpty())
            texture["extensions"] = more;
        b.textures.append(texture);
    }
    for (const QJsonValue &v : gltf.value("materials").toArray())
        b.materials.append(moveTextures(v, textures));
    for (const QJsonValue &v : gltf.value("meshes").toArray()) {
        QJsonObject mesh = v.toObject();
        QJsonArray primitives;
        for (const QJsonValue &p : mesh.value("primitives").toArray()) {
            QJsonObject primitive = p.toObject();
            QJsonObject attributes;
            const QJsonObject from = primitive.value("attributes").toObject();
            for (auto it = from.begin(); it != from.end(); ++it)
                if (!it.key().startsWith("JOINTS_") && !it.key().startsWith("WEIGHTS_"))
                    attributes[it.key()] = it.value().toInt() + accessors;
            primitive["attributes"] = attributes;
            if (primitive.contains("indices"))
                primitive["indices"] = primitive.value("indices").toInt() + accessors;
            if (primitive.contains("material"))
                primitive["material"] = primitive.value("material").toInt() + materials;
            if (primitive.contains("targets")) {
                QJsonArray targets;
                for (const QJsonValue &t : primitive.value("targets").toArray()) {
                    QJsonObject target = t.toObject();
                    for (auto it = target.begin(); it != target.end(); ++it)
                        it.value() = it.value().toInt() + accessors;
                    targets.append(target);
                }
                primitive["targets"] = targets;
            }
            primitives.append(primitive);
        }
        mesh["primitives"] = primitives;
        b.meshes.append(mesh);
    }
    for (const QJsonValue &v : gltf.value("nodes").toArray()) {
        QJsonObject node = v.toObject();
        node.remove("skin");
        node.remove("camera");
        node.remove("extensions");
        if (node.contains("mesh"))
            node["mesh"] = node.value("mesh").toInt() + meshes;
        if (node.contains("children")) {
            QJsonArray children;
            for (const QJsonValue &c : node.value("children").toArray())
                children.append(c.toInt() + nodes);
            node["children"] = children;
        }
        b.nodes.append(node);
    }
    const QJsonArray scenes = gltf.value("scenes").toArray();
    const QJsonArray own = scenes.at(gltf.value("scene").toInt(0)).toObject().value("nodes").toArray();
    for (const QJsonValue &root : own)
        roots->append(root.toInt() + nodes);
    for (const QJsonValue &e : gltf.value("extensionsUsed").toArray())
        if (!extensions->contains(e.toString()))
            *extensions << e.toString();
    return true;
}

// An OBJ model as a mesh a material: its corners (with their picture
// coordinates), and each material's colour or picture from its library.
bool copyObj(Builder &b, const QString &path, QJsonArray *roots, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QString("%1: couldn't read it").arg(QFileInfo(path).fileName());
        return false;
    }
    const QDir beside = QFileInfo(path).absoluteDir();
    QList<QVector3D> positions;
    QList<QPointF> coords;
    struct Part {
        QList<QVector3D> points;
        QList<QPointF> uv;
        QList<quint32> triangles;
        QHash<QPair<int, int>, quint32> corner;
    };
    QMap<QString, Part> parts;
    QHash<QString, QPair<QColor, QString>> library;  // A material's colour and picture.
    QString current;
    for (const QByteArray &raw : file.readAll().split('\n')) {
        const QList<QByteArray> words = raw.simplified().split(' ');
        const QByteArray &what = words.first();
        if (what == "v" && words.size() >= 4)
            positions << QVector3D(words.at(1).toFloat(), words.at(2).toFloat(), words.at(3).toFloat());
        else if (what == "vt" && words.size() >= 3)
            coords << QPointF(words.at(1).toDouble(), 1 - words.at(2).toDouble());  // OBJ counts up from the bottom.
        else if (what == "usemtl" && words.size() >= 2)
            current = QString::fromUtf8(words.at(1));
        else if (what == "mtllib" && words.size() >= 2) {
            QFile mtl(beside.filePath(QString::fromUtf8(raw.simplified().mid(7))));
            if (!mtl.open(QIODevice::ReadOnly))
                continue;
            QString name;
            for (const QByteArray &line : mtl.readAll().split('\n')) {
                const QList<QByteArray> m = line.simplified().split(' ');
                if (m.first() == "newmtl" && m.size() >= 2)
                    name = QString::fromUtf8(m.at(1)), library[name] = {QColor(200, 200, 200), QString()};
                else if (m.first() == "Kd" && m.size() >= 4 && !name.isEmpty())
                    library[name].first = QColor::fromRgbF(std::clamp(m.at(1).toFloat(), 0.f, 1.f), std::clamp(m.at(2).toFloat(), 0.f, 1.f),
                                                           std::clamp(m.at(3).toFloat(), 0.f, 1.f));
                else if (m.first() == "map_Kd" && m.size() >= 2 && !name.isEmpty())
                    library[name].second = beside.filePath(QString::fromUtf8(m.last()));
            }
        } else if (what == "f" && words.size() >= 4) {
            Part &part = parts[current];
            QList<quint32> face;
            for (int k = 1; k < words.size(); ++k) {
                const QList<QByteArray> ids = words.at(k).split('/');
                // Counted from 1, or back from the end when negative.
                auto at = [](const QByteArray &id, qsizetype count) {
                    const int n = id.toInt();
                    return n < 0 ? int(count) + n : n - 1;
                };
                const int v = at(ids.at(0), positions.size());
                const int t = ids.size() > 1 && !ids.at(1).isEmpty() ? at(ids.at(1), coords.size()) : -1;
                if (v < 0 || v >= positions.size())
                    continue;
                const auto key = qMakePair(v, t);
                if (!part.corner.contains(key)) {
                    part.corner.insert(key, quint32(part.points.size()));
                    part.points << positions.at(v);
                    part.uv << (t >= 0 && t < coords.size() ? coords.at(t) : QPointF());
                }
                face << part.corner.value(key);
            }
            // A fan across the face.
            for (int k = 2; k < face.size(); ++k)
                part.triangles << face.at(0) << face.at(k - 1) << face.at(k);
        }
    }
    const QString stem = QFileInfo(path).completeBaseName();
    for (auto it = parts.begin(); it != parts.end(); ++it) {
        if (it->triangles.isEmpty())
            continue;
        const auto look = library.value(it.key(), {QColor(200, 200, 200), QString()});
        const QImage picture = look.second.isEmpty() ? QImage() : QImage(look.second);
        const QString name = it.key().isEmpty() ? stem : it.key();
        const int material = picture.isNull() ? b.colour(look.first, name) : b.litPicture(picture, name);
        roots->append(b.node(QJsonObject{{"name", name}, {"mesh", b.mesh(it->points, it->uv, it->triangles, material, name)}}));
    }
    return true;
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

bool write(const Scene &scene, AssetCache &assets, const QString &path, QString *error, QStringList *skipped) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    Builder b;
    QJsonArray roots;
    QStringList extensions{"KHR_materials_unlit"};
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
        // Each model as it was made, under a node that stands it on its
        // point (its offset, fixed) and one that moves it as the scene does.
        const World::Frame start = World::frame(scene, assets, 0);
        QHash<int, int> modelOf;
        for (const World::Placement &model : start.models) {
            QJsonArray own;
            QString why;
            const bool obj = QFileInfo(model.model).suffix().toLower() == "obj";
            const int before[] = {int(b.views.size()), int(b.accessors.size()), int(b.images.size()), int(b.samplers.size()),
                                  int(b.textures.size()), int(b.materials.size()), int(b.meshes.size()), int(b.nodes.size())};
            const qsizetype bytes = b.bin.size();
            if (!(obj ? copyObj(b, model.model, &own, &why) : copyGltf(b, model.model, &own, &extensions, &why))) {
                // What it got halfway through copying goes again.
                QJsonArray *lists[] = {&b.views, &b.accessors, &b.images, &b.samplers, &b.textures, &b.materials, &b.meshes, &b.nodes};
                for (int k = 0; k < 8; ++k)
                    while (lists[k]->size() > before[k])
                        lists[k]->removeLast();
                b.bin.truncate(bytes);
                if (skipped)
                    *skipped << why;
                continue;
            }
            const int standing = b.node(QJsonObject{{"name", scene.layers.at(model.layer).name + " model"},
                                                    {"translation", json(model.offset)}, {"children", own}});
            const int node = b.node(QJsonObject{{"name", scene.layers.at(model.layer).name}, {"children", QJsonArray{standing}}});
            roots.append(node);
            nodeOf.insert(model.layer, node);
            modelOf.insert(model.layer, node);
            Track track;
            track.node = node;
            tracks << track;
        }
        for (int f = 0; f < frames; ++f) {
            const World::Frame now = World::frame(scene, assets, times.at(f));
            for (Track &track : tracks) {
                const int index = nodeOf.key(track.node, World::FarLayer - 1);
                World::Placement place;
                for (const World::Placement &p : modelOf.contains(index) ? now.models : now.layers)
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
                     {"extensionsUsed", QJsonArray::fromStringList(extensions)},
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
