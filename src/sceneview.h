#pragma once

#include <QColor>
#include <QPointer>
#include <QQuickPaintedItem>

#include "backend.h"

// The canvas: draws the scene at the playhead, fitted to the item, with the
// selected layer's outline and pivot and the cut shape on top.
class SceneView : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(Backend *editor READ backend WRITE setBackend NOTIFY backendChanged)
    Q_PROPERTY(QColor accent READ accent WRITE setAccent NOTIFY accentChanged)
    // Item pixels per canvas pixel, for things drawn over the canvas.
    Q_PROPERTY(double zoomFactor READ zoom NOTIFY zoomChanged)
    // Where the canvas is in the item, for the 3D view that sits under it.
    Q_PROPERTY(QRectF canvasRect READ canvasRect NOTIFY zoomChanged)

public:
    explicit SceneView(QQuickItem *parent = nullptr);

    Backend *backend() const { return m_backend; }
    void setBackend(Backend *backend);
    QColor accent() const { return m_accent; }
    void setAccent(const QColor &accent);

    // Item coordinates to canvas pixels.
    Q_INVOKABLE QPointF toScene(double x, double y) const;
    // How many item pixels one canvas pixel takes up.
    Q_INVOKABLE double zoom() const { return viewTransform().m11(); }
    QRectF canvasRect() const;
    // Looking closer: zoom about an item point, pan by item pixels, or fit
    // the canvas back in the window. Only the view; not the camera.
    Q_INVOKABLE void zoomBy(double factor, double x, double y);
    Q_INVOKABLE void panBy(double dx, double dy);
    Q_INVOKABLE void fit();

    void paint(QPainter *painter) override;

signals:
    void backendChanged();
    void accentChanged();
    void zoomChanged();

private:
    double m_viewZoom = 1;
    QPointF m_focus;
    bool m_focusSet = false;
    QTransform viewTransform() const;

    QPointer<Backend> m_backend;
    QColor m_accent = QColor("#FFD60A");
};
