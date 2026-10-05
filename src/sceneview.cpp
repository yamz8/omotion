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
        connect(m_backend, &Backend::showMotionChanged, this, repaint);
        // A new project starts fitted in the window.
        connect(m_backend, &Backend::projectChanged, this, &SceneView::fit);
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
    const double fit = std::min(available / scene.width, availableHeight / scene.height);
    const double s = fit * m_viewZoom;
    // The point looked at sits in the middle of the item; until the view is
    // panned, that's the canvas's centre.
    const QPointF focus = m_focusSet ? m_focus : QPointF(scene.width / 2.0, scene.height / 2.0);
    transform.translate(width() / 2, height() / 2);
    transform.scale(s, s);
    transform.translate(-focus.x(), -focus.y());
    return transform;
}

void SceneView::zoomBy(double factor, double x, double y) {
    if (!m_backend || !m_backend->hasProject() || !(factor > 0))
        return;
    // The canvas point under the mouse stays under it.
    const QPointF under = toScene(x, y);
    const double zoom = std::clamp(m_viewZoom * factor, .1, 40.0);
    if (zoom == m_viewZoom)
        return;
    m_viewZoom = zoom;
    const QTransform view = viewTransform();
    const double s = view.m11();
    m_focus = under - (QPointF(x, y) - QPointF(width() / 2, height() / 2)) / s;
    m_focusSet = true;
    update();
    emit zoomChanged();
}

void SceneView::panBy(double dx, double dy) {
    if (!m_backend || !m_backend->hasProject())
        return;
    const double s = viewTransform().m11();
    if (!m_focusSet) {
        m_focus = QPointF(m_backend->scene().width / 2.0, m_backend->scene().height / 2.0);
        m_focusSet = true;
    }
    m_focus -= QPointF(dx, dy) / s;
    update();
    emit zoomChanged();
}

void SceneView::fit() {
    m_viewZoom = 1;
    m_focusSet = false;
    update();
    emit zoomChanged();
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

    const int selected = m_backend->selected();
    const bool motion = m_backend->showMotion() && !m_backend->playing() && selected >= 0 &&
                        selected < scene.layers.size() && scene.layers.at(selected).particles.isEmpty();
    const double now = m_backend->time();

    painter->save();
    painter->setTransform(view, true);
    renderScene(*painter, scene, m_backend->assets(), now);
    painter->restore();

    // Onion skin: the layer faintly where it was and where it's going, at the
    // keyframes either side (or a quarter second, between keys).
    if (motion) {
        const Layer &layer = scene.layers.at(selected);
        const QList<double> keys = scene.keyTimes(selected);
        double before = std::max(0.0, now - .25), after = std::min(scene.duration, now + .25);
        for (double t : keys) {
            if (t < now - .5 / scene.fps)
                before = t;
            else if (t > now + .5 / scene.fps) {
                after = t;
                break;
            }
        }
        const QSizeF size = m_backend->assets().size(layer.source);
        for (const auto &[t, tint] : {std::pair{before, QColor(255, 120, 120)}, std::pair{after, QColor(120, 200, 255)}}) {
            if (std::abs(t - now) < .5 / scene.fps)
                continue;
            const QTransform ghost = layerTransform(scene, m_backend->assets(), selected, t) * view;
            double factor = 1;
            const double scale = std::max(std::hypot(ghost.m11(), ghost.m12()), std::hypot(ghost.m21(), ghost.m22()));
            const QImage image = m_backend->assets().mip(layer.source, scale, &factor);
            if (image.isNull())
                continue;
            painter->save();
            painter->setTransform(QTransform::fromScale(1 / factor, 1 / factor) * ghost);
            painter->setOpacity(.28);
            painter->drawImage(QPointF(0, 0), image);
            painter->setOpacity(.8);
            painter->setPen(QPen(tint, 1.5 / std::max(1e-6, std::sqrt(std::abs((QTransform::fromScale(1 / factor, 1 / factor) * ghost).determinant()))),
                                 Qt::DashLine));
            painter->setBrush(Qt::NoBrush);
            painter->drawRect(QRectF(QPointF(0, 0), size * factor));
            painter->restore();
        }

        // The path the pivot travels through the whole scene, with the keys.
        const QPointF pivot(layer.anchor.x() * size.width(), layer.anchor.y() * size.height());
        QPolygonF path;
        const int steps = std::clamp(int(scene.duration * scene.fps), 2, 2000);
        for (int i = 0; i <= steps; ++i)
            path.append((layerTransform(scene, m_backend->assets(), selected, scene.duration * i / steps) * view).map(pivot));
        QColor line = m_accent;
        line.setAlpha(170);
        painter->setPen(QPen(line, 1.5));
        painter->setBrush(Qt::NoBrush);
        painter->drawPolyline(path);
        painter->setPen(QPen(QColor("#0e0e10"), 1));
        painter->setBrush(m_accent);
        for (double t : keys) {
            const QPointF at = (layerTransform(scene, m_backend->assets(), selected, t) * view).map(pivot);
            painter->drawEllipse(at, 3.5, 3.5);
        }
    }

    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(QPen(QColor(255, 255, 255, 40), 1));
    painter->setBrush(Qt::NoBrush);
    painter->drawRect(canvas);

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
