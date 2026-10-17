#pragma once

#include <QColor>
#include <QPointer>
#include <QQuickPaintedItem>

#include "backend.h"

// The graph editor: what's selected's animated properties as curves over
// the scene, each fitted to the height (or one alone, solo, with its
// values); its keys can be dragged in time and value. Time runs across the
// whole item, as the timeline's tracks have it.
class GraphView : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(Backend *editor READ backend WRITE setBackend NOTIFY backendChanged)
    Q_PROPERTY(QColor accent READ accent WRITE setAccent NOTIFY accentChanged)
    // Just this property's curve, with its values along the side; empty for all.
    Q_PROPERTY(QString solo READ solo WRITE setSolo NOTIFY soloChanged)

public:
    explicit GraphView(QQuickItem *parent = nullptr);

    Backend *backend() const { return m_backend; }
    void setBackend(Backend *backend);
    QColor accent() const { return m_accent; }
    void setAccent(const QColor &accent);
    QString solo() const { return m_solo; }
    void setSolo(const QString &solo);

    // Each property's own colour, the same in the list beside the graph.
    static QColor colourOf(const QString &property);
    Q_INVOKABLE QColor colour(const QString &property) const { return colourOf(property); }

    // The mouse: pressing on a key picks it up (and moves the playhead
    // there); elsewhere it scrubs. With lockTime, a dragged key keeps its
    // time and only its value changes.
    Q_INVOKABLE void press(double x, double y);
    Q_INVOKABLE void drag(double x, double y, bool lockTime);
    Q_INVOKABLE void release();

    void paint(QPainter *painter) override;

signals:
    void backendChanged();
    void accentChanged();
    void soloChanged();

private:
    struct Range {
        double low = 0, high = 1;
    };
    Range rangeOf(const Backend::Curve &curve) const;
    double yOf(double v, const Range &range) const;
    double valueAt(double y, const Range &range) const;
    QList<Backend::Curve> shown() const;

    QPointer<Backend> m_backend;
    QColor m_accent = QColor("#FFD60A");
    QString m_solo;
    // The key being dragged, and its curve's range when the drag began (so
    // the scale holds still under the hand).
    QString m_dragProperty;
    int m_dragIndex = -1;
    Range m_dragRange;
};
