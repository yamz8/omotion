#include "worldview.h"

#include <QCoreApplication>
#include <algorithm>
#include <optional>
#include <QDir>
#include <cstdio>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include <QtQml>
#include <rhi/qrhi.h>

void WorldSource::setScene(const Scene &scene, const QString &directory, double t) {
    m_time = t;
    if (m_assets->directory() != directory)
        m_assets->setDirectory(directory);
    m_scene = scene;
    m_info = World::info(scene);
    // Meshes only change with the world and the layers' pictures and pivots.
    QJsonObject key{{"world", scene.raw.value("world")}, {"directory", directory}};
    // The layer behind has triangles only where its picture shows, so a
    // picture made again under the same name means its mesh again too.
    if (m_info.valid && !m_info.farPicture.isEmpty())
        key["far"] = QString::number(QFileInfo(QDir(directory).filePath(m_info.farPicture)).lastModified().toMSecsSinceEpoch());
    QVariantList layers;
    QJsonArray parts;
    if (m_info.valid && !m_info.farDepth.isEmpty())
        layers.append(QVariantMap{{"layer", World::FarLayer},
                                  {"picture", QUrl::fromLocalFile(QDir(directory).filePath(m_info.farPicture))},
                                  {"shaped", false}});
    if (m_info.valid)
        for (const int index : World::layers(scene)) {
            const Layer &layer = scene.layers.at(index);
            const QString path = QDir(directory).filePath(layer.source);
            const QString picture = QDir(directory).filePath(World::texture(scene, *m_assets, index));
            layers.append(QVariantMap{{"layer", index},
                                      {"opaque", index == scene.indexOf(m_info.picture)},
                                      {"picture", QUrl::fromLocalFile(picture)},
                                      {"shaped", !layer.raw.value("shape").toObject().isEmpty()}});
            parts.append(QJsonArray{index, layer.source, layer.anchor.x(), layer.anchor.y(),
                                    QFileInfo(path).lastModified().toMSecsSinceEpoch(), layer.raw.value("shape")});
        }
    key["layers"] = parts;
    QVariantList models;
    if (m_info.valid)
        for (const int index : World::modelLayers(scene))
            models.append(QVariantMap{{"layer", index},
                                      {"source", QUrl::fromLocalFile(QDir(directory).filePath(
                                                     scene.layers.at(index).raw.value("model").toObject().value("file").toString()))}});
    const QByteArray meshKey = QJsonDocument(key).toJson(QJsonDocument::Compact);
    const bool rebuilt = meshKey != m_meshKey || layers != m_layers || models != m_models;
    if (rebuilt) {
        m_meshKey = meshKey;
        m_meshes.clear();
        m_layers = layers;
        m_models = models;
        ++m_revision;
        emit changed();
    }
    update();
}

void WorldSource::setTime(double t) {
    if (t == m_time)
        return;
    m_time = t;
    update();
}

void WorldSource::setView(bool free, const World::View &view) {
    if (free == m_free && (!free || (view.position == m_view.position && view.rotation == m_view.rotation && view.fov == m_view.fov)))
        return;
    m_free = free;
    m_view = view;
    update();
}

const World::Mesh &WorldSource::mesh(int layer) {
    auto found = m_meshes.find(layer);
    if (found == m_meshes.end())
        found = m_meshes.insert(layer, World::mesh(m_scene, *m_assets, layer));
    return *found;
}

void WorldSource::update() {
    QVariantMap frame;
    if (m_info.valid) {
        std::optional<World::ReadyOnly> readyOnly;
        if (m_live)
            readyOnly.emplace();
        const World::Frame f = World::frame(m_scene, *m_assets, m_time);
        readyOnly.reset();
        frame["cameraPosition"] = QVariant::fromValue(f.cameraPosition);
        frame["cameraRotation"] = QVariant::fromValue(f.cameraRotation);
        frame["fov"] = f.fov;
        // Depth of field, through the 3D view's own: its focus in meters.
        const QJsonObject look = m_scene.raw.value("look").toObject();
        frame["aperture"] = look.value("aperture").toDouble(0);
        frame["focus"] = look.value("focus").toDouble(10);
        if (m_free) {
            frame["viewPosition"] = QVariant::fromValue(m_view.position);
            frame["viewRotation"] = QVariant::fromValue(m_view.rotation);
            frame["viewFov"] = m_view.fov;
        }
        QVariantMap places;
        for (const World::Placement &place : f.layers)
            places.insert(QString::number(place.layer), QVariantMap{{"haze", place.haze},
                                                                     {"wrap", place.wrap},
                                                                     {"position", QVariant::fromValue(place.position)},
                                                                     {"rotation", QVariant::fromValue(place.rotation)},
                                                                     {"scale", place.scale},
                                                                     {"opacity", place.opacity},
                                                                     {"visible", place.visible},
                                                                     {"shine", place.shine},
                                                                     {"roughness", place.roughness},
                                                                     {"warpAmount", place.warpAmount},
                                                                     {"warpScale", QVariant::fromValue(place.warpScale)},
                                                                     {"warpDrift", QVariant::fromValue(place.warpDrift)}});
        for (const World::Placement &model : f.models)
            places.insert(QString::number(model.layer), QVariantMap{{"position", QVariant::fromValue(model.position)},
                                                                     {"rotation", QVariant::fromValue(model.rotation)},
                                                                     {"scale", model.scale},
                                                                     {"offset", QVariant::fromValue(model.offset)},
                                                                     {"opacity", model.opacity},
                                                                     {"visible", model.visible}});
        frame["places"] = places;
        // Up to four lights, for the shaders.
        QVariantList lights;
        for (const World::Light &light : f.lights.mid(0, 4))
            lights.append(QVariantMap{{"position", QVariant::fromValue(light.position)},
                                      {"colour", QVariant::fromValue(light.colour)},
                                      {"reach", light.reach}});
        frame["lights"] = lights;
        QVariantList shadows;
        for (const World::Shadow &shadow : f.shadows)
            shadows.append(QVariantMap{{"position", QVariant::fromValue(shadow.position)},
                                       {"width", shadow.width},
                                       {"length", shadow.length},
                                       {"yaw", shadow.yaw},
                                       {"opacity", shadow.opacity}});
        frame["shadows"] = shadows;
        QVariantList bursts;
        for (const World::Burst &burst : f.bursts)
            bursts.append(QVariantMap{{"position", QVariant::fromValue(burst.position)},
                                      {"away", QVariant::fromValue(burst.away)},
                                      {"time", burst.time},
                                      {"strength", burst.strength}});
        frame["bursts"] = bursts;
        frame["time"] = f.time;
        frame["haze"] = f.haze;
        frame["dustCount"] = f.puffs.size();
        // Smoke, fire and water from its pieces, on sheets among them.
        QVariantList sheets;
        for (const World::Sheet &sheet : World::sheets(m_scene, *m_assets, m_time)) {
            QQuick3DTextureData *texture = this->sheet(sheet.key);
            // Rows bottom first, as the rectangle's texture coordinates go.
            const QImage rgba = sheet.image.convertToFormat(QImage::Format_RGBA8888_Premultiplied).mirrored();
            texture->setSize(rgba.size());
            texture->setFormat(QQuick3DTextureData::RGBA8);
            texture->setHasTransparency(true);
            texture->setTextureData(QByteArray(reinterpret_cast<const char *>(rgba.constBits()), rgba.sizeInBytes()));
            sheets.append(QVariantMap{{"layer", sheet.layer},
                                      {"key", sheet.key},
                                      {"position", QVariant::fromValue(sheet.centre)},
                                      {"rotation", QVariant::fromValue(sheet.rotation)},
                                      {"width", sheet.size.width()},
                                      {"height", sheet.size.height()},
                                      {"opacity", sheet.opacity}});
        }
        frame["sheets"] = sheets;
        // Shadows from the lights: each caster's ellipsoid, and which one is
        // a layer's own.
        QByteArray packed(32 * 3 * 4 * int(sizeof(float)), '\0');
        float *o = reinterpret_cast<float *>(packed.data());
        for (int k = 0; k < f.occluders.size(); ++k) {
            const World::Occluder &c = f.occluders.at(k);
            float *row = o + k * 12;
            row[0] = c.centre.x(), row[1] = c.centre.y(), row[2] = c.centre.z();
            row[4] = c.half.x(), row[5] = c.half.y(), row[6] = c.half.z(), row[7] = float(c.layer);
            row[8] = c.rotation.x(), row[9] = c.rotation.y(), row[10] = c.rotation.z(), row[11] = c.rotation.scalar();
        }
        QQuick3DTextureData *texture = occluders();
        texture->setSize(QSize(3, 32));
        texture->setFormat(QQuick3DTextureData::RGBA32F);
        texture->setTextureData(packed);
        frame["occluderCount"] = f.occluders.size();
        constexpr int most = World::Frame::mostSolids;
        QByteArray boxes(most * 3 * 4 * int(sizeof(float)), '\0');
        float *boxRows = reinterpret_cast<float *>(boxes.data());
        for (int k = 0; k < f.solids.size() && k < most; ++k) {
            const World::Occluder &c = f.solids.at(k).box;
            float *row = boxRows + k * 12;
            row[0] = c.centre.x(), row[1] = c.centre.y(), row[2] = c.centre.z();
            row[4] = c.half.x(), row[5] = c.half.y(), row[6] = c.half.z(), row[7] = float(c.layer);
            row[8] = c.rotation.x(), row[9] = c.rotation.y(), row[10] = c.rotation.z(), row[11] = c.rotation.scalar();
            // Only sent to the GPU again when they're other cells: they're
            // cached, so the same shape keeps the same data.
            QQuick3DTextureData *cells = solidCells(k);
            if (cells->textureData().constData() != f.solids.at(k).cells.constData()) {
                cells->setSize(QSize(World::Frame::solid, World::Frame::solid));
                cells->setDepth(World::Frame::solid);
                cells->setFormat(QQuick3DTextureData::R8);
                cells->setTextureData(f.solids.at(k).cells);
            }
        }
        QQuick3DTextureData *solidBoxes = solids();
        solidBoxes->setSize(QSize(3, most));
        solidBoxes->setFormat(QQuick3DTextureData::RGBA32F);
        solidBoxes->setTextureData(boxes);
        frame["solidCount"] = std::min<qsizetype>(most, f.solids.size());
        // The world's own picture as the light all round, for imported
        // models' materials, so they take its colours.
        // A small copy of its own, in the cache: the picture's file is the
        // cards' texture too, and a probe made from it would take its place.
        const int root = m_scene.indexOf(m_info.picture);
        if (root >= 0 && !m_scene.layers.at(root).source.isEmpty()) {
            const QString file = QDir(m_assets->directory()).absoluteFilePath(m_scene.layers.at(root).source);
            const QDir cache(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/probes");
            const QString probe = cache.filePath(QString::number(qHash(file + QString::number(QFileInfo(file).lastModified().toMSecsSinceEpoch())), 16) + ".png");
            if (!QFileInfo::exists(probe)) {
                cache.mkpath(".");
                const QImage small = m_assets->image(m_scene.layers.at(root).source).scaled(512, 256, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                QSaveFile out(probe);
                if (!small.isNull() && out.open(QIODevice::WriteOnly) && small.save(&out, "PNG"))
                    out.commit();
            }
            if (QFileInfo::exists(probe))
                frame["probe"] = QUrl::fromLocalFile(probe).toString();
        }

        // Puffs face the camera and are drawn far to near, so they blend.
        QList<World::Puff> puffs = f.puffs;
        const QVector3D eye = f.cameraPosition;
        std::sort(puffs.begin(), puffs.end(), [&](const World::Puff &a, const World::Puff &b) {
            return (a.position - eye).lengthSquared() > (b.position - eye).lengthSquared();
        });
        const QVector3D right = f.cameraRotation.rotatedVector(QVector3D(1, 0, 0));
        const QVector3D up = f.cameraRotation.rotatedVector(QVector3D(0, 1, 0));
        m_dustVertices.resize(qsizetype(puffs.size()) * 4 * 9 * sizeof(float));
        m_dustIndices.resize(qsizetype(puffs.size()) * 6 * sizeof(quint32));
        float *v = reinterpret_cast<float *>(m_dustVertices.data());
        quint32 *i = reinterpret_cast<quint32 *>(m_dustIndices.data());
        const float r = float(f.dust.redF()), g = float(f.dust.greenF()), b = float(f.dust.blueF());
        for (int n = 0; n < puffs.size(); ++n) {
            const World::Puff &puff = puffs.at(n);
            const QVector3D half = right * (puff.size / 2), lift = up * (puff.size / 2);
            const QVector3D corners[4] = {puff.position - half - lift, puff.position + half - lift,
                                          puff.position + half + lift, puff.position - half + lift};
            const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
            for (int k = 0; k < 4; ++k) {
                *v++ = corners[k].x(), *v++ = corners[k].y(), *v++ = corners[k].z();
                *v++ = uv[k][0], *v++ = uv[k][1];
                if (puff.colour.isValid())
                    *v++ = float(puff.colour.redF()), *v++ = float(puff.colour.greenF()), *v++ = float(puff.colour.blueF());
                else
                    *v++ = r, *v++ = g, *v++ = b;
                *v++ = puff.alpha;
            }
            const quint32 base = quint32(n * 4);
            *i++ = base, *i++ = base + 1, *i++ = base + 2, *i++ = base, *i++ = base + 2, *i++ = base + 3;
        }
    }
    m_frame = frame;
    emit frameChanged();
}

QQuick3DTextureData *WorldSource::solids() {
    if (!m_solids) {
        m_solids = std::make_shared<QQuick3DTextureData>();
        QQmlEngine::setObjectOwnership(m_solids.get(), QQmlEngine::CppOwnership);
    }
    return m_solids.get();
}

QQuick3DTextureData *WorldSource::solidCells(int i) {
    std::shared_ptr<QQuick3DTextureData> &cells = m_solidCells[std::clamp(i, 0, World::Frame::mostSolids - 1)];
    if (!cells) {
        cells = std::make_shared<QQuick3DTextureData>();
        // Something to sample before there's a shape: one empty cell.
        cells->setSize(QSize(1, 1));
        cells->setDepth(1);
        cells->setFormat(QQuick3DTextureData::R8);
        cells->setTextureData(QByteArray(1, '\0'));
        QQmlEngine::setObjectOwnership(cells.get(), QQmlEngine::CppOwnership);
    }
    return cells.get();
}

QQuick3DTextureData *WorldSource::occluders() {
    if (!m_occluders) {
        m_occluders = std::make_shared<QQuick3DTextureData>();
        QQmlEngine::setObjectOwnership(m_occluders.get(), QQmlEngine::CppOwnership);
    }
    return m_occluders.get();
}

QQuick3DTextureData *WorldSource::sheet(int key) {
    std::shared_ptr<QQuick3DTextureData> &texture = m_sheets[key];
    if (!texture) {
        texture = std::make_shared<QQuick3DTextureData>();
        QQmlEngine::setObjectOwnership(texture.get(), QQmlEngine::CppOwnership);
    }
    return texture.get();
}

void WorldGeometry::setSource(WorldSource *source) {
    if (m_source == source)
        return;
    if (m_source)
        disconnect(m_source, nullptr, this, nullptr);
    m_source = source;
    if (m_source)
        connect(m_source, &WorldSource::changed, this, &WorldGeometry::rebuild);
    emit sourceChanged();
    rebuild();
}

void WorldGeometry::setLayer(int layer) {
    if (m_layer == layer)
        return;
    m_layer = layer;
    emit layerChanged();
    rebuild();
}

void WorldGeometry::rebuild() {
    clear();
    if (m_source && (m_layer >= 0 || m_layer == World::FarLayer)) {
        const World::Mesh &mesh = m_source->mesh(m_layer);
        if (!mesh.isNull() && !mesh.normals.isEmpty() && mesh.stride == 5) {
            // A grid's normals go in beside each vertex, for the GPU only.
            const qsizetype count = mesh.normals.size() / qsizetype(3 * sizeof(float));
            QByteArray interleaved(count * 8 * qsizetype(sizeof(float)), Qt::Uninitialized);
            const float *v = reinterpret_cast<const float *>(mesh.vertices.constData());
            const float *n = reinterpret_cast<const float *>(mesh.normals.constData());
            float *out = reinterpret_cast<float *>(interleaved.data());
            for (qsizetype i = 0; i < count; ++i) {
                std::copy(v + i * 5, v + i * 5 + 5, out + i * 8);
                std::copy(n + i * 3, n + i * 3 + 3, out + i * 8 + 5);
            }
            setVertexData(interleaved);
            setIndexData(mesh.indices);
            setStride(8 * sizeof(float));
            setPrimitiveType(PrimitiveType::Triangles);
            addAttribute(Attribute::PositionSemantic, 0, Attribute::F32Type);
            addAttribute(Attribute::TexCoord0Semantic, 3 * sizeof(float), Attribute::F32Type);
            addAttribute(Attribute::NormalSemantic, 5 * sizeof(float), Attribute::F32Type);
            addAttribute(Attribute::IndexSemantic, 0, Attribute::U32Type);
            setBounds(mesh.minimum, mesh.maximum);
        } else if (!mesh.isNull()) {
            setVertexData(mesh.vertices);
            setIndexData(mesh.indices);
            setStride(mesh.stride * sizeof(float));
            setPrimitiveType(PrimitiveType::Triangles);
            addAttribute(Attribute::PositionSemantic, 0, Attribute::F32Type);
            addAttribute(Attribute::TexCoord0Semantic, 3 * sizeof(float), Attribute::F32Type);
            if (mesh.stride >= 14) {
                addAttribute(Attribute::ColorSemantic, 5 * sizeof(float), Attribute::F32Type);
                addAttribute(Attribute::TexCoord1Semantic, 9 * sizeof(float), Attribute::F32Type);
                addAttribute(Attribute::NormalSemantic, 11 * sizeof(float), Attribute::F32Type);
            }
            addAttribute(Attribute::IndexSemantic, 0, Attribute::U32Type);
            setBounds(mesh.minimum, mesh.maximum);
        }
    }
    update();
}

void DustGeometry::setSource(WorldSource *source) {
    if (m_source == source)
        return;
    if (m_source)
        disconnect(m_source, nullptr, this, nullptr);
    m_source = source;
    if (m_source)
        connect(m_source, &WorldSource::frameChanged, this, &DustGeometry::rebuild);
    emit sourceChanged();
    rebuild();
}

void DustGeometry::rebuild() {
    clear();
    if (m_source && !m_source->dustVertices().isEmpty()) {
        setVertexData(m_source->dustVertices());
        setIndexData(m_source->dustIndices());
        setStride(9 * sizeof(float));
        setPrimitiveType(PrimitiveType::Triangles);
        addAttribute(Attribute::PositionSemantic, 0, Attribute::F32Type);
        addAttribute(Attribute::TexCoord0Semantic, 3 * sizeof(float), Attribute::F32Type);
        addAttribute(Attribute::ColorSemantic, 5 * sizeof(float), Attribute::F32Type);
        addAttribute(Attribute::IndexSemantic, 0, Attribute::U32Type);
        setBounds(QVector3D(-1e4f, -1e4f, -1e4f), QVector3D(1e4f, 1e4f, 1e4f));
    }
    update();
}

struct WorldRenderer::Private {
    QQuickRenderControl control;
    QQuickWindow window{&control};
    QQmlEngine engine;
    WorldSource source;
    QQuickItem *root = nullptr;
    QRhi *rhi = nullptr;
    std::unique_ptr<QRhiTexture> texture;
    std::unique_ptr<QRhiRenderBuffer> depthStencil;
    std::unique_ptr<QRhiTextureRenderTarget> target;
    std::unique_ptr<QRhiRenderPassDescriptor> pass;
    QSize size;
    bool failed = false;

    bool start() {
        engine.rootContext()->setContextProperty("worldSource", &source);
        QQmlComponent component(&engine, QUrl("qrc:/WorldView.qml"));
        root = qobject_cast<QQuickItem *>(component.create());
        if (!root) {
            qWarning() << "omotion: the world view didn't load:" << component.errors();
            return false;
        }
        root->setProperty("source", QVariant::fromValue(&source));
        root->setParentItem(window.contentItem());
        // What the world doesn't cover stays see-through, for the layers
        // and background drawn around it.
        window.setColor(Qt::transparent);
        if (!control.initialize())
            return false;
        rhi = control.rhi();
        return rhi != nullptr;
    }

    bool resize(const QSize &wanted) {
        if (wanted == size && target)
            return true;
        size = wanted;
        window.setGeometry(0, 0, size.width(), size.height());
        root->setSize(size);
        texture.reset(rhi->newTexture(QRhiTexture::RGBA8, size, 1,
                                      QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        depthStencil.reset(rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, size, 1));
        if (!texture->create() || !depthStencil->create())
            return false;
        QRhiTextureRenderTargetDescription description{QRhiColorAttachment(texture.get())};
        description.setDepthStencilBuffer(depthStencil.get());
        target.reset(rhi->newTextureRenderTarget(description));
        pass.reset(target->newCompatibleRenderPassDescriptor());
        target->setRenderPassDescriptor(pass.get());
        if (!target->create())
            return false;
        window.setRenderTarget(QQuickRenderTarget::fromRhiRenderTarget(target.get()));
        return true;
    }

    QImage draw() {
        QCoreApplication::processEvents();
        control.polishItems();
        control.beginFrame();
        control.sync();
        control.render();
        QRhiReadbackResult result;
        QRhiResourceUpdateBatch *batch = rhi->nextResourceUpdateBatch();
        batch->readBackTexture(texture.get(), &result);
        control.commandBuffer()->resourceUpdate(batch);
        control.endFrame();
        const QSize got = result.pixelSize;
        if (Q_BYTE_ORDER != Q_LITTLE_ENDIAN || result.data.size() != qsizetype(got.width()) * got.height() * 4) {
            QImage image(reinterpret_cast<const uchar *>(result.data.constData()), got.width(), got.height(),
                         QImage::Format_RGBA8888_Premultiplied);
            if (rhi->isYUpInFramebuffer())
                image = image.mirrored();
            return image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        }
        // Turned the right way up and its red and blue swapped in one pass,
        // rather than a copy for each: a shutter draws a frame many times.
        // (Rows padded out, which the size would show, take the way above.)
        QImage image(got, QImage::Format_ARGB32_Premultiplied);
        const bool flip = rhi->isYUpInFramebuffer();
        const quint32 *from = reinterpret_cast<const quint32 *>(result.data.constData());
        for (int y = 0; y < got.height(); ++y) {
            const quint32 *row = from + size_t(flip ? got.height() - 1 - y : y) * got.width();
            quint32 *out = reinterpret_cast<quint32 *>(image.scanLine(y));
            for (int x = 0; x < got.width(); ++x) {
                const quint32 v = row[x];
                out[x] = (v & 0xff00ff00u) | ((v & 0xffu) << 16) | ((v >> 16) & 0xffu);
            }
        }
        return image;
    }
};

WorldRenderer &WorldRenderer::instance() {
    static WorldRenderer renderer;
    return renderer;
}

WorldRenderer::WorldRenderer() : d(std::make_unique<Private>()) {
    d->failed = !d->start();
}

WorldRenderer::~WorldRenderer() = default;

QImage WorldRenderer::render(const Scene &scene, const QString &directory, double t, const QSize &size) {
    if (d->failed || size.isEmpty() || !d->resize(size))
        return {};
    QElapsedTimer clock;
    clock.start();
    const int revision = d->source.revision();
    d->source.setScene(scene, directory, t);
    const qint64 a = clock.nsecsElapsed();
    // A new world's pictures are uploaded on its first frame; the second one
    // shows them.
    QImage image = d->draw();
    if (d->source.revision() != revision)
        image = d->draw();
    // OMOTION_PROFILE=1 tells where a frame's time goes.
    if (qEnvironmentVariableIsSet("OMOTION_PROFILE"))
        fprintf(stderr, "world render: scene %.1f ms, draw %.1f ms\n", a / 1e6, (clock.nsecsElapsed() - a) / 1e6);
    return image;
}

void registerWorld() {
    qmlRegisterType<WorldGeometry>("Omotion", 1, 0, "WorldGeometry");
    qmlRegisterType<DustGeometry>("Omotion", 1, 0, "DustGeometry");
    qmlRegisterUncreatableType<WorldSource>("Omotion", 1, 0, "WorldSource", "Made by omotion.");
    setWorldRenderer([](const Scene &scene, AssetCache &assets, double t, const QSize &size) {
        return WorldRenderer::instance().render(scene, assets.directory(), t, size);
    });
}
