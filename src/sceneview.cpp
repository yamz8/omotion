#include "sceneview.h"

#include <QPainter>
#include <QPainterPath>
#include <QJsonArray>
#include <QJsonObject>
#include <QLineF>

#include "world.h"
#include <algorithm>
#include <tuple>

namespace {
constexpr double Margin = 24;
}

SceneView::SceneView(QQuickItem *parent) : QQuickPaintedItem(parent) {
    setAntialiasing(true);
    auto resized = [this] {
        if (m_backend)
            m_backend->setViewportSize(width(), height());
        update();
        emit zoomChanged();
    };
    connect(this, &QQuickItem::widthChanged, this, resized);
    connect(this, &QQuickItem::heightChanged, this, resized);
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
        connect(m_backend, &Backend::valuesChanged, this, repaint);
        connect(m_backend, &Backend::viewChanged, this, repaint);
        m_backend->setViewportSize(width(), height());
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

QRectF SceneView::canvasRect() const {
    if (!m_backend || !m_backend->hasProject())
        return {};
    const Scene &scene = m_backend->scene();
    return viewTransform().mapRect(QRectF(0, 0, scene.width, scene.height));
}

QPointF SceneView::toScene(double x, double y) const { return viewTransform().inverted().map(QPointF(x, y)); }

QTransform SceneView::fromView() const {
    return m_backend && m_backend->freeView() ? QTransform() : viewTransform();
}

QPointF SceneView::toView(double x, double y) const { return fromView().inverted().map(QPointF(x, y)); }

double SceneView::viewZoom() const { return fromView().m11(); }

void SceneView::paintGizmo(QPainter *painter) {
    const Backend::Gizmo gizmo = m_backend->gizmo();
    const QTransform to = fromView();
    painter->save();
    // Through the shot's camera, what's off the canvas isn't in the shot.
    if (!m_backend->freeView())
        painter->setClipRect(canvasRect());
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(QColor(255, 255, 255, 28), 1));
    for (const QPolygonF &line : gizmo.grid)
        painter->drawPolyline(to.map(line));
    // The shot's camera, seen from the free view.
    if (gizmo.camera.size() == 5) {
        const bool picked = m_backend->cameraSelected();
        painter->setPen(QPen(picked ? m_accent : QColor(255, 255, 255, 170), 1.5));
        for (int i = 1; i < 5; ++i) {
            painter->drawLine(to.map(gizmo.camera.at(0)), to.map(gizmo.camera.at(i)));
            painter->drawLine(to.map(gizmo.camera.at(i)), to.map(gizmo.camera.at(i % 4 + 1)));
        }
        // Its top edge marked, so which way is up shows.
        const QPointF top = (to.map(gizmo.camera.at(1)) + to.map(gizmo.camera.at(2))) / 2;
        painter->drawLine(top, top + (top - to.map(gizmo.camera.at(0))) * .15);
    }
    // The others selected with it, fainter.
    for (const QList<QPointF> &box : gizmo.others) {
        QColor faint = m_accent;
        faint.setAlpha(120);
        painter->setPen(QPen(faint, 1.2, Qt::DashLine));
        for (int a = 0; a < 8; ++a)
            for (int bit : {1, 2, 4})
                if (!(a & bit))
                    painter->drawLine(to.map(box.at(a)), to.map(box.at(a | bit)));
    }
    if (gizmo.corners.size() == 8) {
        painter->setPen(QPen(m_accent, 1.5, Qt::DashLine));
        for (int a = 0; a < 8; ++a)
            for (int bit : {1, 2, 4})
                if (!(a & bit))
                    painter->drawLine(to.map(gizmo.corners.at(a)), to.map(gizmo.corners.at(a | bit)));
    }
    if (gizmo.arrows.size() == 3) {
        const int active = m_backend->handle();
        // Blender's colours for x, y and z; the one in use is brighter.
        const QColor colours[3] = {QColor(232, 72, 85), QColor(122, 196, 74), QColor(66, 135, 245)};
        const QPointF pivot = to.map(gizmo.pivot);
        // The rings, coloured by their axes: yaw about y, pitch x, roll z.
        for (const auto &[polygon, id, colour] : {std::tuple{&gizmo.ring, 4, colours[1]}, std::tuple{&gizmo.ringPitch, 5, colours[0]},
                                                 std::tuple{&gizmo.ringRoll, 6, colours[2]}}) {
            QColor faint = colour;
            faint.setAlpha(active == id ? 255 : 130);
            painter->setPen(QPen(active == id ? m_accent : faint, active == id ? 2.5 : 1.3));
            painter->drawPolyline(to.map(*polygon));
        }
        for (int i = 0; i < 3; ++i) {
            const QPointF end = to.map(gizmo.arrows.at(i));
            if (QLineF(pivot, end).length() < 4)
                continue;
            painter->setPen(QPen(colours[i], active == i + 1 ? 3.5 : 2.2, Qt::SolidLine, Qt::RoundCap));
            painter->drawLine(pivot, end);
            painter->setBrush(colours[i]);
            painter->drawEllipse(end, 4.5, 4.5);
            painter->setBrush(Qt::NoBrush);
        }
        painter->setPen(QPen(m_accent, 2));
        painter->setBrush(QColor(0, 0, 0, 160));
        painter->drawEllipse(pivot, 5, 5);
        // While grabbed and kept to an axis, the axis runs right across.
        if (active >= 1 && active <= 3) {
            const QPointF along = to.map(gizmo.arrows.at(active - 1)) - pivot;
            const double length = std::hypot(along.x(), along.y());
            if (length > 1) {
                QColor line = colours[active - 1];
                line.setAlpha(150);
                painter->setPen(QPen(line, 1));
                painter->drawLine(pivot - along * (4000 / length), pivot + along * (4000 / length));
            }
        }
    }
    painter->restore();
}

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
        // Only the squares in sight: zoomed far in, the canvas is huge.
        const int cell = 10;
        const QRectF shown = canvas & QRectF(0, 0, width(), height());
        const int x0 = int((shown.left() - canvas.left()) / cell), x1 = int(std::ceil((shown.right() - canvas.left()) / cell));
        const int y0 = int((shown.top() - canvas.top()) / cell), y1 = int(std::ceil((shown.bottom() - canvas.top()) / cell));
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x)
                painter->fillRect(QRectF(canvas.left() + x * cell, canvas.top() + y * cell, cell, cell),
                                  (x + y) % 2 ? QColor("#2a2a2e") : QColor("#1c1c1f"));
        painter->restore();
    }

    // The free view: just the world (drawn underneath), the shot's camera
    // and what's selected in it.
    if (m_backend->freeView()) {
        paintGizmo(painter);
        return;
    }

    const int selected = m_backend->selected();
    const bool motion = m_backend->showMotion() && !m_backend->playing() && selected >= 0 &&
                        selected < scene.layers.size() && scene.layers.at(selected).particles.isEmpty();
    const double now = m_backend->time();

    // A world is shown in 3D underneath; the rest is drawn over it.
    painter->save();
    painter->setTransform(view, true);
    renderScene(*painter, scene, m_backend->assets(), now, m_backend->world()->active());
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

    // A world's pieces are outlined where they are in 3D, with handles.
    const bool inWorld = m_backend->world()->active() && World::layers(scene).contains(selected);
    if (inWorld)
        paintGizmo(painter);
    else if (selected >= 0 && selected < scene.layers.size() && !m_backend->playing() &&
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

    // Flat layers selected along with it, outlined fainter.
    for (const QVariant &value : m_backend->alsoSelected()) {
        const int other = value.toInt();
        if (other < 0 || other >= scene.layers.size() || (m_backend->world()->active() && World::layers(scene).contains(other)))
            continue;
        const QSizeF size = m_backend->assets().size(scene.layers.at(other).source);
        QColor faint = m_accent;
        faint.setAlpha(140);
        painter->setPen(QPen(faint, 1.2, Qt::DashLine));
        painter->setBrush(Qt::NoBrush);
        painter->drawPolygon((layerTransform(scene, m_backend->assets(), other, now) * view).map(QRectF(QPointF(), size)));
    }
    // Empties: crosses, as Blender draws them; the selected one brighter.
    for (int i = 0; i < scene.layers.size(); ++i) {
        const Layer &layer = scene.layers.at(i);
        if (!layer.raw.value("empty").toBool() || !layer.visible)
            continue;
        const QPointF at = (layerTransform(scene, m_backend->assets(), i, now) * view).map(QPointF());
        painter->setPen(QPen(i == selected ? m_accent : QColor(255, 255, 255, 190), i == selected ? 2.2 : 1.5));
        painter->setBrush(Qt::NoBrush);
        painter->drawLine(at - QPointF(9, 0), at + QPointF(9, 0));
        painter->drawLine(at - QPointF(0, 9), at + QPointF(0, 9));
        painter->drawEllipse(at, 4, 4);
    }
    // The selected layer's path, where it has one.
    if (selected >= 0 && selected < scene.layers.size())
        for (const QJsonValue &entry : scene.layers.at(selected).raw.value("constraints").toArray()) {
            const QJsonObject c = entry.toObject();
            if (c.value("type").toString() != "path")
                continue;
            const int parent = scene.indexOf(scene.layers.at(selected).parent);
            const QTransform toView = (parent >= 0 ? chainTransform(scene, m_backend->assets(), parent, now) : QTransform()) *
                                      cameraTransform(scene, followOf(scene, selected), now) * view;
            QPolygonF line;
            for (const QJsonValue &p : c.value("points").toArray())
                line << toView.map(QPointF(p.toArray().at(0).toDouble(), p.toArray().at(1).toDouble()));
            QColor colour = m_accent;
            colour.setAlpha(170);
            painter->setPen(QPen(colour, 1.5, Qt::DashLine));
            painter->drawPolyline(line);
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

    // Puppet pins, where they've bent to.
    if (m_backend->tool() == "pin") {
        painter->setPen(QPen(QColor("#0e0e10"), 2));
        painter->setBrush(m_accent);
        for (const QVariant &point : m_backend->pinPoints()) {
            const QPointF at = view.map(point.toPointF());
            painter->drawEllipse(at, 6, 6);
            painter->drawEllipse(at, 2, 2);
        }
    }

    const QList<QPointF> cut = m_backend->cutPolygon();
    if (!cut.isEmpty()) {
        QPolygonF shape;
        for (const QPointF &point : cut)
            shape.append(view.map(point));
        QPainterPath path;
        path.addPolygon(shape);
        // A path stays open; a cut's shape closes.
        const bool open = m_backend->tool() == "path";
        if (shape.size() > 2 && !open)
            path.closeSubpath();
        QColor fill = m_accent;
        fill.setAlpha(50);
        if (!open)
            painter->fillPath(path, fill);
        painter->setPen(QPen(m_accent, 2));
        painter->setBrush(Qt::NoBrush);
        painter->drawPolyline(shape);
        if (shape.size() > 2 && !open) {
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
