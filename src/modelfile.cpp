#include "modelfile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQuaternion>
#include <QUrl>

namespace ModelFile {

Gltf readGltf(const QString &path) {
    Gltf file;
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly)) {
        file.error = "couldn't read it";
        return file;
    }
    const QByteArray bytes = in.readAll();
    QByteArray text = bytes, binary;
    if (bytes.startsWith("glTF")) {
        text.clear();
        // Chunks after the header: the JSON, then the binary.
        for (qsizetype at = 12; at + 8 <= bytes.size();) {
            const quint32 length = *reinterpret_cast<const quint32 *>(bytes.constData() + at);
            const quint32 type = *reinterpret_cast<const quint32 *>(bytes.constData() + at + 4);
            if (qsizetype(length) > bytes.size() - at - 8)
                break;
            if (type == 0x4E4F534A)
                text = bytes.mid(at + 8, length);
            else if (type == 0x004E4942)
                binary = bytes.mid(at + 8, length);
            at += 8 + qsizetype(length);
        }
    }
    file.json = QJsonDocument::fromJson(text).object();
    if (file.json.isEmpty()) {
        file.error = "it isn't glTF";
        return file;
    }
    const QDir beside = QFileInfo(path).absoluteDir();
    for (const QJsonValue &b : file.json.value("buffers").toArray()) {
        const QString uri = b.toObject().value("uri").toString();
        if (uri.isEmpty())
            file.buffers << binary;
        else if (uri.startsWith("data:"))
            file.buffers << QByteArray::fromBase64(uri.mid(uri.indexOf(',') + 1).toLatin1());
        else {
            QFile f(beside.filePath(QUrl::fromPercentEncoding(uri.toUtf8())));
            if (!f.open(QIODevice::ReadOnly)) {
                file.error = QString("couldn't read its %1").arg(uri);
                file.buffers << QByteArray();
            } else
                file.buffers << f.readAll();
        }
    }
    return file;
}

QMatrix4x4 nodeMatrix(const QJsonObject &node) {
    QMatrix4x4 local;
    const QJsonArray m = node.value("matrix").toArray();
    if (m.size() == 16) {
        float values[16];
        for (int k = 0; k < 16; ++k)
            values[k] = float(m.at(k).toDouble());
        return QMatrix4x4(values).transposed();  // glTF's are column by column.
    }
    const QJsonArray tr = node.value("translation").toArray(), r = node.value("rotation").toArray(), sc = node.value("scale").toArray();
    if (tr.size() == 3)
        local.translate(float(tr.at(0).toDouble()), float(tr.at(1).toDouble()), float(tr.at(2).toDouble()));
    if (r.size() == 4)
        local.rotate(QQuaternion(float(r.at(3).toDouble()), float(r.at(0).toDouble()), float(r.at(1).toDouble()), float(r.at(2).toDouble())));
    if (sc.size() == 3)
        local.scale(float(sc.at(0).toDouble()), float(sc.at(1).toDouble()), float(sc.at(2).toDouble()));
    return local;
}

void walk(const QJsonObject &gltf, const std::function<void(const QJsonObject &node, const QMatrix4x4 &world)> &visit) {
    const QJsonArray nodes = gltf.value("nodes").toArray();
    std::function<void(int, const QMatrix4x4 &, int)> down = [&](int index, const QMatrix4x4 &above, int depth) {
        // Kept from looping round a file that makes a node its own child.
        if (depth > 64 || index < 0 || index >= nodes.size())
            return;
        const QJsonObject node = nodes.at(index).toObject();
        const QMatrix4x4 world = above * nodeMatrix(node);
        visit(node, world);
        for (const QJsonValue &child : node.value("children").toArray())
            down(child.toInt(), world, depth + 1);
    };
    const QJsonArray scenes = gltf.value("scenes").toArray();
    for (const QJsonValue &root : scenes.at(gltf.value("scene").toInt(0)).toObject().value("nodes").toArray())
        down(root.toInt(), QMatrix4x4(), 0);
}

bool read(const Gltf &file, int index, int width, QList<double> *out) {
    const QJsonArray accessors = file.json.value("accessors").toArray(), views = file.json.value("bufferViews").toArray();
    if (index < 0 || index >= accessors.size())
        return false;
    const QJsonObject a = accessors.at(index).toObject();
    if (!a.contains("bufferView"))
        return false;
    const QJsonObject view = views.at(a.value("bufferView").toInt()).toObject();
    const QByteArray &buffer = file.buffers.value(view.value("buffer").toInt());
    const int type = a.value("componentType").toInt();
    const int size = type == 5126 || type == 5125 ? 4 : type == 5123 || type == 5122 ? 2 : 1;
    const qsizetype stride = view.value("byteStride").toInt(size * width);
    const qsizetype start = view.value("byteOffset").toInteger() + a.value("byteOffset").toInteger();
    const qsizetype count = a.value("count").toInteger();
    // Nothing outside its buffer, either way, whatever the file says.
    if (start < 0 || stride < size * width || count <= 0 || count > buffer.size() || start + (count - 1) * stride + size * width > buffer.size())
        return false;
    for (qsizetype i = 0; i < count; ++i)
        for (int k = 0; k < width; ++k) {
            const char *p = buffer.constData() + start + i * stride + k * size;
            double value = 0;
            if (type == 5126)
                value = *reinterpret_cast<const float *>(p);
            else if (type == 5125)
                value = *reinterpret_cast<const quint32 *>(p);
            else if (type == 5123)
                value = *reinterpret_cast<const quint16 *>(p);
            else if (type == 5121)
                value = *reinterpret_cast<const quint8 *>(p);
            else
                return false;
            *out << value;
        }
    return true;
}

QList<QVector3D> objPoints(const QByteArray &text) {
    QList<QVector3D> points;
    for (const QByteArray &raw : text.split('\n')) {
        const QList<QByteArray> words = raw.simplified().split(' ');
        if (words.first() == "v" && words.size() >= 4)
            points << QVector3D(words.at(1).toFloat(), words.at(2).toFloat(), words.at(3).toFloat());
    }
    return points;
}

QList<QVector3D> objTriangles(const QByteArray &text) {
    QList<QVector3D> points, corners;
    for (const QByteArray &raw : text.split('\n')) {
        const QList<QByteArray> words = raw.simplified().split(' ');
        if (words.first() == "v" && words.size() >= 4)
            points << QVector3D(words.at(1).toFloat(), words.at(2).toFloat(), words.at(3).toFloat());
        else if (words.first() == "f" && words.size() >= 4) {
            // Counted from 1, or back from the end when negative; a corner
            // naming no point (0, too far, not a number) leaves out the
            // triangles it's in.
            QList<int> face;
            for (int k = 1; k < words.size(); ++k) {
                const int id = words.at(k).split('/').first().toInt();
                face << (id < 0 ? int(points.size()) + id : id - 1);
            }
            auto known = [&](int i) { return i >= 0 && i < points.size(); };
            for (int k = 2; k < face.size(); ++k)
                if (known(face.at(0)) && known(face.at(k - 1)) && known(face.at(k)))
                    corners << points.at(face.at(0)) << points.at(face.at(k - 1)) << points.at(face.at(k));
        }
    }
    return corners;
}

QList<QVector3D> triangles(const QString &path) {
    if (QFileInfo(path).suffix().toLower() == "obj") {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? objTriangles(file.readAll()) : QList<QVector3D>();
    }
    QList<QVector3D> corners;
    const Gltf file = readGltf(path);
    const QJsonArray meshes = file.json.value("meshes").toArray();
    walk(file.json, [&](const QJsonObject &node, const QMatrix4x4 &world) {
        if (!node.contains("mesh"))
            return;
        for (const QJsonValue &p : meshes.at(node.value("mesh").toInt()).toObject().value("primitives").toArray()) {
            const QJsonObject primitive = p.toObject();
            if (primitive.value("mode").toInt(4) != 4)
                continue;
            QList<double> positions, order;
            if (!read(file, primitive.value("attributes").toObject().value("POSITION").toInt(-1), 3, &positions))
                continue;
            if (primitive.contains("indices"))
                read(file, primitive.value("indices").toInt(), 1, &order);
            else
                for (qsizetype k = 0; k < positions.size() / 3; ++k)
                    order << double(k);
            // Whole triangles only, so one bad index can't put every later
            // one out of step.
            for (qsizetype t = 0; t + 2 < order.size(); t += 3) {
                QVector3D triangle[3];
                bool whole = true;
                for (int c = 0; c < 3 && whole; ++c) {
                    const qsizetype at = qsizetype(order.at(t + c)) * 3;
                    whole = at >= 0 && at + 2 < positions.size();
                    if (whole)
                        triangle[c] = world.map(QVector3D(float(positions.at(at)), float(positions.at(at + 1)), float(positions.at(at + 2))));
                }
                if (whole)
                    corners << triangle[0] << triangle[1] << triangle[2];
            }
        }
    });
    return corners;
}

}
