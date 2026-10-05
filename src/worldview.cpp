#include "worldview.h"

#include <QCoreApplication>
#include <algorithm>
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
#include <QUrl>
#include <QtQml>
#include <rhi/qrhi.h>

void WorldSource::setScene(const Scene &scene, const QString &directory) {
    if (m_assets->directory() != directory)
        m_assets->setDirectory(directory);
    m_scene = scene;
    m_info = World::info(scene);
    // Meshes only change with the world and the layers' pictures and pivots.
    QJsonObject key{{"world", scene.raw.value("world")}, {"directory", directory}};
    QVariantList layers;
    QJsonArray parts;
    if (m_info.valid)
        for (const int index : World::layers(scene)) {
            const Layer &layer = scene.layers.at(index);
            const QString path = QDir(directory).filePath(layer.source);
            const QString picture = QDir(directory).filePath(World::texture(scene, *m_assets, index));
            layers.append(QVariantMap{{"layer", index},
                                      {"picture", QUrl::fromLocalFile(picture)},
                                      {"shaped", !layer.raw.value("shape").toObject().isEmpty()}});
            parts.append(QJsonArray{index, layer.source, layer.anchor.x(), layer.anchor.y(),
                                    QFileInfo(path).lastModified().toMSecsSinceEpoch()});
        }
    key["layers"] = parts;
    const QByteArray meshKey = QJsonDocument(key).toJson(QJsonDocument::Compact);
    const bool rebuilt = meshKey != m_meshKey || layers != m_layers;
    if (rebuilt) {
        m_meshKey = meshKey;
        m_meshes.clear();
        m_layers = layers;
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

const World::Mesh &WorldSource::mesh(int layer) {
    auto found = m_meshes.find(layer);
    if (found == m_meshes.end())
        found = m_meshes.insert(layer, World::mesh(m_scene, *m_assets, layer));
    return *found;
}

void WorldSource::update() {
    QVariantMap frame;
    if (m_info.valid) {
        const World::Frame f = World::frame(m_scene, *m_assets, m_time);
        frame["cameraPosition"] = QVariant::fromValue(f.cameraPosition);
        frame["cameraRotation"] = QVariant::fromValue(f.cameraRotation);
        frame["fov"] = f.fov;
        QVariantMap places;
        for (const World::Placement &place : f.layers)
            places.insert(QString::number(place.layer), QVariantMap{{"position", QVariant::fromValue(place.position)},
                                                                     {"rotation", QVariant::fromValue(place.rotation)},
                                                                     {"scale", place.scale},
                                                                     {"opacity", place.opacity},
                                                                     {"visible", place.visible}});
        frame["places"] = places;
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
        frame["dustCount"] = f.puffs.size();

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
                *v++ = r, *v++ = g, *v++ = b, *v++ = puff.alpha;
            }
            const quint32 base = quint32(n * 4);
            *i++ = base, *i++ = base + 1, *i++ = base + 2, *i++ = base, *i++ = base + 2, *i++ = base + 3;
        }
    }
    m_frame = frame;
    emit frameChanged();
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
    if (m_source && m_layer >= 0) {
        const World::Mesh &mesh = m_source->mesh(m_layer);
        if (!mesh.isNull()) {
            setVertexData(mesh.vertices);
            setIndexData(mesh.indices);
            setStride(mesh.stride * sizeof(float));
            setPrimitiveType(PrimitiveType::Triangles);
            addAttribute(Attribute::PositionSemantic, 0, Attribute::F32Type);
            addAttribute(Attribute::TexCoord0Semantic, 3 * sizeof(float), Attribute::F32Type);
            if (mesh.stride >= 11) {
                addAttribute(Attribute::ColorSemantic, 5 * sizeof(float), Attribute::F32Type);
                addAttribute(Attribute::TexCoord1Semantic, 9 * sizeof(float), Attribute::F32Type);
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
        QImage image(reinterpret_cast<const uchar *>(result.data.constData()), result.pixelSize.width(),
                     result.pixelSize.height(), QImage::Format_RGBA8888_Premultiplied);
        if (rhi->isYUpInFramebuffer())
            image = image.mirrored();
        return image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
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
    d->source.setScene(scene, directory);
    const qint64 a = clock.nsecsElapsed();
    d->source.setTime(t);
    const qint64 b = clock.nsecsElapsed();
    // A new world's pictures are uploaded on its first frame; the second one
    // shows them.
    QImage image = d->draw();
    if (d->source.revision() != revision)
        image = d->draw();
    // OMOTION_PROFILE=1 tells where a frame's time goes.
    if (qEnvironmentVariableIsSet("OMOTION_PROFILE"))
        fprintf(stderr, "world render: scene %.1f ms, frame %.1f ms, draw %.1f ms\n", a / 1e6, (b - a) / 1e6, (clock.nsecsElapsed() - b) / 1e6);
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
