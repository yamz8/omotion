#include "graphview.h"

#include <QPainter>
#include <QPainterPath>
#include <cmath>

namespace {
constexpr double Top = 12, Bottom = 12;
}

GraphView::GraphView(QQuickItem *parent) : QQuickPaintedItem(parent) {
    setAntialiasing(true);
    connect(this, &QQuickItem::widthChanged, this, [this] { update(); });
    connect(this, &QQuickItem::heightChanged, this, [this] { update(); });
}

void GraphView::setBackend(Backend *backend) {
    if (backend == m_backend)
        return;
    if (m_backend)
        disconnect(m_backend, nullptr, this, nullptr);
    m_backend = backend;
    if (m_backend) {
        auto repaint = [this] { update(); };
        connect(m_backend, &Backend::sceneChanged, this, repaint);
        connect(m_backend, &Backend::valuesChanged, this, repaint);
        connect(m_backend, &Backend::selectedChanged, this, repaint);
        connect(m_backend, &Backend::timeChanged, this, repaint);
    }
    emit backendChanged();
    update();
}

void GraphView::setAccent(const QColor &accent) {
    if (accent == m_accent)
        return;
    m_accent = accent;
    emit accentChanged();
    update();
}

void GraphView::setSolo(const QString &solo) {
    if (solo == m_solo)
        return;
    m_solo = solo;
    emit soloChanged();
    update();
}

QColor GraphView::colourOf(const QString &property) {
    // Blender's: x red, y green, z blue; the rest from a few clear hues.
    static const QHash<QString, QColor> known{
        {"x", QColor(232, 72, 85)},       {"worldX", QColor(232, 72, 85)},   {"y", QColor(122, 196, 74)},
        {"worldY", QColor(122, 196, 74)}, {"z", QColor(66, 135, 245)},       {"worldZ", QColor(66, 135, 245)},
        {"rotation", QColor(240, 200, 60)}, {"yaw", QColor(240, 200, 60)}, {"pitch", QColor(230, 140, 60)},
        {"roll", QColor(200, 110, 220)},  {"scale", QColor(80, 210, 200)}, {"zoom", QColor(80, 210, 200)},
        {"opacity", QColor(220, 220, 230)}};
    if (known.contains(property))
        return known.value(property);
    return QColor::fromHsv(int(qHash(property) % 360), 150, 230);
}

QList<Backend::Curve> GraphView::shown() const {
    QList<Backend::Curve> list;
    if (!m_backend)
        return list;
    for (const Backend::Curve &curve : m_backend->curves())
        if (m_solo.isEmpty() || curve.property == m_solo)
            list << curve;
    return list;
}

GraphView::Range GraphView::rangeOf(const Backend::Curve &curve) const {
    Range range{curve.low, curve.high};
    // A flat curve sits in the middle.
    if (range.high - range.low < 1e-9) {
        const double pad = std::max(1.0, std::abs(range.low) * .1);
        range.low -= pad;
        range.high += pad;
    }
    return range;
}

double GraphView::yOf(double v, const Range &range) const {
    const double h = std::max(1.0, height() - Top - Bottom);
    return Top + (1 - (v - range.low) / (range.high - range.low)) * h;
}

double GraphView::valueAt(double y, const Range &range) const {
    const double h = std::max(1.0, height() - Top - Bottom);
    return range.low + (1 - (y - Top) / h) * (range.high - range.low);
}

void GraphView::press(double x, double y) {
    if (!m_backend)
        return;
    const double duration = std::max(1e-6, m_backend->scene().duration);
    if (m_backend->playing())
        m_backend->togglePlay();
    // The nearest key within reach, of the curves shown.
    double best = 9;
    for (const Backend::Curve &curve : shown()) {
        const Range range = rangeOf(curve);
        for (int i = 0; i < curve.keys.size(); ++i) {
            const double d = std::hypot(curve.keys.at(i).t / duration * width() - x, yOf(curve.keys.at(i).v, range) - y);
            if (d < best) {
                best = d;
                m_dragProperty = curve.property;
                m_dragIndex = i;
                m_dragRange = range;
            }
        }
    }
    if (m_dragIndex >= 0) {
        m_backend->beginGesture();
        for (const Backend::Curve &curve : shown())
            if (curve.property == m_dragProperty)
                m_backend->seek(curve.keys.at(m_dragIndex).t);
        update();
        return;
    }
    m_backend->seek(std::clamp(x / std::max(1.0, width()), 0.0, 1.0) * duration);
}

void GraphView::drag(double x, double y, bool lockTime) {
    if (!m_backend)
        return;
    const double duration = std::max(1e-6, m_backend->scene().duration);
    if (m_dragIndex < 0) {
        m_backend->seek(std::clamp(x / std::max(1.0, width()), 0.0, 1.0) * duration);
        return;
    }
    double t = std::clamp(x / std::max(1.0, width()), 0.0, 1.0) * duration;
    for (const Backend::Curve &curve : m_backend->curves(2))
        if (curve.property == m_dragProperty && m_dragIndex < curve.keys.size() && lockTime)
            t = curve.keys.at(m_dragIndex).t;
    // To the frame, as keys are set.
    t = std::round(t * m_backend->scene().fps) / m_backend->scene().fps;
    m_backend->moveKey(m_dragProperty, m_dragIndex, t, valueAt(y, m_dragRange));
    m_backend->seek(t);
}

void GraphView::release() {
    if (m_dragIndex >= 0 && m_backend)
        m_backend->endGesture();
    m_dragIndex = -1;
    m_dragProperty.clear();
    update();
}

void GraphView::paint(QPainter *painter) {
    if (!m_backend || !m_backend->hasProject())
        return;
    painter->setRenderHint(QPainter::Antialiasing);
    const double duration = std::max(1e-6, m_backend->scene().duration);
    // Quarter lines across, and the middle stronger.
    for (int i = 0; i <= 4; ++i) {
        const double y = Top + i * (height() - Top - Bottom) / 4;
        painter->setPen(QPen(QColor(255, 255, 255, i == 2 ? 26 : 14), 1));
        painter->drawLine(QPointF(0, y), QPointF(width(), y));
    }
    const QList<Backend::Curve> curves = shown();
    if (curves.isEmpty()) {
        painter->setPen(QColor(138, 138, 147));
        painter->drawText(boundingRect(), Qt::AlignCenter,
                          m_backend->selected() < 0 && !m_backend->cameraSelected()
                              ? QString("Select a layer to see its curves.")
                              : QString("Nothing animated yet: K keys every property at the playhead."));
        return;
    }
    for (const Backend::Curve &curve : curves) {
        const Range range = curve.property == m_dragProperty ? m_dragRange : rangeOf(curve);
        const QColor colour = colourOf(curve.property);
        QPolygonF line;
        for (int i = 0; i < curve.samples.size(); ++i)
            line << QPointF(width() * i / std::max<qsizetype>(1, curve.samples.size() - 1), yOf(curve.samples.at(i), range));
        painter->setPen(QPen(colour, m_solo.isEmpty() ? 1.6 : 2.2));
        painter->setBrush(Qt::NoBrush);
        painter->drawPolyline(line);
        for (int i = 0; i < curve.keys.size(); ++i) {
            const Key &key = curve.keys.at(i);
            const QPointF at(key.t / duration * width(), yOf(key.v, range));
            const bool held = curve.property == m_dragProperty && i == m_dragIndex;
            painter->setPen(QPen(QColor("#0e0e10"), 1.5));
            painter->setBrush(held ? QColor(Qt::white) : colour);
            painter->drawEllipse(at, held ? 5.5 : 4, held ? 5.5 : 4);
        }
        if (!m_solo.isEmpty()) {
            // Alone, its values show: the highest and lowest.
            painter->setPen(QColor(200, 200, 210));
            QFont font = painter->font();
            font.setPixelSize(11);
            painter->setFont(font);
            painter->drawText(QPointF(4, Top + 11), QString::number(range.high, 'g', 4));
            painter->drawText(QPointF(4, height() - Bottom - 3), QString::number(range.low, 'g', 4));
        }
    }
}
