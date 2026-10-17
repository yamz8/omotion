#pragma once

// Showing a world: WorldSource hands a scene's world to WorldView.qml (the
// meshes, the pictures on them, where everything is at the playhead), and
// WorldRenderer draws that same QML without a window, for exports.

#include <QHash>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQuick3D/QQuick3DGeometry>
#include <memory>

#include "scene.h"
#include "world.h"

class WorldSource : public QObject {
    Q_OBJECT
    // Whether there's a world to show.
    Q_PROPERTY(bool active READ active NOTIFY changed)
    // One entry per world layer, back to front: its index and picture.
    Q_PROPERTY(QVariantList layers READ layers NOTIFY changed)
    // The camera and every layer's place at the current time.
    Q_PROPERTY(QVariantMap frame READ frame NOTIFY frameChanged)
    // Imported models: their layer and file, changing only with the scene.
    Q_PROPERTY(QVariantList models READ models NOTIFY changed)
    // Bumped when meshes need building again.
    Q_PROPERTY(int revision READ revision NOTIFY changed)

public:
    explicit WorldSource(QObject *parent = nullptr) : QObject(parent) {}

    void setScene(const Scene &scene, const QString &directory);
    void setTime(double t);
    // The window's free view, shown instead of the shot's camera; off for
    // exports.
    void setView(bool free, const World::View &view = {});

    bool active() const { return m_info.valid; }
    QVariantList layers() const { return m_layers; }
    QVariantList models() const { return m_models; }
    QVariantMap frame() const { return m_frame; }
    int revision() const { return m_revision; }
    const World::Mesh &mesh(int layer);
    // The frame's dust as camera-facing quads: position, uv, colour.
    const QByteArray &dustVertices() const { return m_dustVertices; }
    const QByteArray &dustIndices() const { return m_dustIndices; }

signals:
    void changed();
    void frameChanged();

private:
    void update();

    Scene m_scene;
    std::unique_ptr<AssetCache> m_assets = std::make_unique<AssetCache>();
    World::Info m_info;
    QVariantList m_layers;
    QVariantList m_models;
    QVariantMap m_frame;
    QHash<int, World::Mesh> m_meshes;
    QByteArray m_dustVertices;
    QByteArray m_dustIndices;
    QByteArray m_meshKey;
    double m_time = 0;
    int m_revision = 0;
    bool m_free = false;
    World::View m_view;
};

// A world layer's mesh, for a Model in QML.
class WorldGeometry : public QQuick3DGeometry {
    Q_OBJECT
    Q_PROPERTY(WorldSource *source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(int layer READ layer WRITE setLayer NOTIFY layerChanged)

public:
    WorldSource *source() const { return m_source; }
    void setSource(WorldSource *source);
    int layer() const { return m_layer; }
    void setLayer(int layer);

signals:
    void sourceChanged();
    void layerChanged();

private:
    void rebuild();

    QPointer<WorldSource> m_source;
    int m_layer = -1;
};

// The dust of the current frame, for a Model in QML.
class DustGeometry : public QQuick3DGeometry {
    Q_OBJECT
    Q_PROPERTY(WorldSource *source READ source WRITE setSource NOTIFY sourceChanged)

public:
    WorldSource *source() const { return m_source; }
    void setSource(WorldSource *source);

signals:
    void sourceChanged();

private:
    void rebuild();
    QPointer<WorldSource> m_source;
};

// Draws worlds without a window, on the GPU; one per program, made on first
// use on the main thread.
class WorldRenderer {
public:
    static WorldRenderer &instance();
    // The world of a scene at t, at size; null when it can't draw here (no
    // GPU, or a platform that has none, as in tests).
    QImage render(const Scene &scene, const QString &directory, double t, const QSize &size);
    ~WorldRenderer();

private:
    WorldRenderer();
    struct Private;
    std::unique_ptr<Private> d;
};

// Registers the QML types and hands renderFrame the renderer.
void registerWorld();
