#include "worldview.h"

#include <QCoreApplication>
#include <QDir>
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
            layers.append(QVariantMap{{"layer", index}, {"picture", QUrl::fromLocalFile(picture)}});
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
            setStride(5 * sizeof(float));
            setPrimitiveType(PrimitiveType::Triangles);
            addAttribute(Attribute::PositionSemantic, 0, Attribute::F32Type);
            addAttribute(Attribute::TexCoord0Semantic, 3 * sizeof(float), Attribute::F32Type);
            addAttribute(Attribute::IndexSemantic, 0, Attribute::U32Type);
            setBounds(mesh.minimum, mesh.maximum);
        }
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
    const int revision = d->source.revision();
    d->source.setScene(scene, directory);
    d->source.setTime(t);
    // A new world's pictures are uploaded on its first frame; the second one
    // shows them.
    QImage image = d->draw();
    if (d->source.revision() != revision)
        image = d->draw();
    return image;
}

void registerWorld() {
    qmlRegisterType<WorldGeometry>("Omotion", 1, 0, "WorldGeometry");
    qmlRegisterUncreatableType<WorldSource>("Omotion", 1, 0, "WorldSource", "Made by omotion.");
    setWorldRenderer([](const Scene &scene, AssetCache &assets, double t, const QSize &size) {
        return WorldRenderer::instance().render(scene, assets.directory(), t, size);
    });
}
