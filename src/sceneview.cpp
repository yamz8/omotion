#include "sceneview.h"

#include <QPainter>
#include <QPainterPath>
#include <algorithm>

namespace {
constexpr double Margin = 24;
}

SceneView::SceneView(QQuickItem *parent) : QQuickPaintedItem(parent) {
    setAntialiasing(true);
    connect(this, &QQuickItem::widthChanged, this, [this] { update(); emit zoomChanged(); });
    connect(this, &QQuickItem::heightChanged, this, [this] { update(); emit zoomChanged(); });
}

void SceneView::setBackend(Backend *backend) {
    if (backend == m_backend)
        return;
    if (m_backend)
        disconnect(m_backend, nullptr, this, nullptr);
    m_backend = backend;
    if (m_backend) {
        auto repaint = [this] { update(); };
        connect(m_backend, &Backend::sceneChanged, this, repaint);
        connect(m_backend, &Backend::sceneChanged, this, &SceneView::zoomChanged);
        connect(m_backend, &Backend::timeChanged, this, repaint);
        connect(m_backend, &Backend::selectedChanged, this, repaint);
        connect(m_backend, &Backend::toolChanged, this, repaint);
        connect(m_backend, &Backend::playingChanged, this, repaint);
    }
    emit backendChanged();
    update();
}

void SceneView::setAccent(const QColor &accent) {
    if (accent == m_accent)
        return;
    m_accent = accent;
    emit accentChanged();
    update();
}

QTransform SceneView::viewTransform() const {
    QTransform transform;
    if (!m_backend || !m_backend->hasProject())
        return transform;
    const Scene &scene = m_backend->scene();
    const double available = std::max(1.0, width() - 2 * Margin), availableHeight = std::max(1.0, height() - 2 * Margin);
    const double s = std::min(available / scene.width, availableHeight / scene.height);
    transform.translate((width() - scene.width * s) / 2, (height() - scene.height * s) / 2);
    transform.scale(s, s);
    return transform;
}

QPointF SceneView::toScene(double x, double y) const { return viewTransform().inverted().map(QPointF(x, y)); }

void SceneView::paint(QPainter *painter) {
    if (!m_backend || !m_backend->hasProject())
        return;
    const Scene &scene = m_backend->scene();
    const QTransform view = viewTransform();
    const QRectF canvas = view.mapRect(QRectF(0, 0, scene.width, scene.height));

    // A checkerboard shows through wherever the background is see-through.
    if (scene.background.alpha() < 255) {
        painter->save();
        painter->setClipRect(canvas);
        const int cell = 10;
        for (int y = 0; y * cell < canvas.height(); ++y)
            for (int x = 0; x * cell < canvas.width(); ++x)
                painter->fillRect(QRectF(canvas.left() + x * cell, canvas.top() + y * cell, cell, cell),
                                  (x + y) % 2 ? QColor("#2a2a2e") : QColor("#1c1c1f"));
        painter->restore();
    }

    painter->save();
    painter->setTransform(view, true);
    renderScene(*painter, scene, m_backend->assets(), m_backend->time());
    painter->restore();

    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(QPen(QColor(255, 255, 255, 40), 1));
    painter->setBrush(Qt::NoBrush);
    painter->drawRect(canvas);

    const int selected = m_backend->selected();
    if (selected >= 0 && selected < scene.layers.size() && !m_backend->playing() &&
        scene.layers.at(selected).particles.isEmpty()) {
        const Layer &layer = scene.layers.at(selected);
        const QSizeF size = m_backend->assets().size(layer.source);
        const QTransform toView = layerTransform(scene, m_backend->assets(), selected, m_backend->time()) * view;
        painter->setPen(QPen(m_accent, 1.5, Qt::DashLine));
        painter->drawPolygon(toView.map(QRectF(QPointF(0, 0), size)));

        const QPointF pivot = toView.map(QPointF(layer.anchor.x() * size.width(), layer.anchor.y() * size.height()));
        painter->setPen(QPen(m_accent, 2));
        painter->setBrush(QColor(0, 0, 0, 160));
        painter->drawEllipse(pivot, 5, 5);
        painter->drawLine(pivot - QPointF(9, 0), pivot + QPointF(9, 0));
        painter->drawLine(pivot - QPointF(0, 9), pivot + QPointF(0, 9));
    }

    // The shape being painted with the brush, tinted.
    if (m_backend->tool() == "brush" && !m_backend->brushPreview().isNull()) {
        const QImage shape = m_backend->brushPreview();
        QImage tinted(shape.size(), QImage::Format_ARGB32_Premultiplied);
        tinted.fill(m_accent);
        {
            QPainter tint(&tinted);
            tint.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            tint.drawImage(0, 0, shape);
        }
        painter->save();
        painter->setTransform(m_backend->brushPreviewToCanvas() * view);
        painter->setOpacity(.45);
        painter->drawImage(QPointF(0, 0), tinted);
        painter->restore();
    }

    const QList<QPointF> cut = m_backend->cutPolygon();
    if (!cut.isEmpty()) {
        QPolygonF shape;
        for (const QPointF &point : cut)
            shape.append(view.map(point));
        QPainterPath path;
        path.addPolygon(shape);
        if (shape.size() > 2)
            path.closeSubpath();
        QColor fill = m_accent;
        fill.setAlpha(50);
        painter->fillPath(path, fill);
        painter->setPen(QPen(m_accent, 2));
        painter->setBrush(Qt::NoBrush);
        painter->drawPolyline(shape);
        if (shape.size() > 2) {
            painter->setPen(QPen(m_accent, 1.5, Qt::DashLine));
            painter->drawLine(shape.last(), shape.first());
        }
        // Handles to grab; a traced lasso has so many that they shrink to dots.
        const double radius = shape.size() > 40 ? 2.5 : 4.5;
        painter->setPen(QPen(QColor("#0e0e10"), 1.5));
        painter->setBrush(m_accent);
        for (const QPointF &point : shape)
            painter->drawEllipse(point, radius, radius);
    }
}
