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

    void paint(QPainter *painter) override;

signals:
    void backendChanged();
    void accentChanged();

private:
    QTransform viewTransform() const;

    QPointer<Backend> m_backend;
    QColor m_accent = QColor("#FFD60A");
};
