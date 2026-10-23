#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QVariantMap>

class QQmlEngine;

// The style guide's tokens, as a scene reads them: Style.colors.ink,
// Style.fonts.headline, and the motion rules Motion.rule("panel", t, start)
// springs by. They come from the TOKENS section of style-guide.md:
//
//   color ink #f4f1ea
//   font headline Adwaita Sans
//   rule panel 120 22        (stiffness, damping)
//
// so a film can't drift from its own guide, and a change to the guide
// reaches every frame.
class StyleTokens : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap colors READ colors NOTIFY changed)
    Q_PROPERTY(QVariantMap fonts READ fonts NOTIFY changed)
    Q_PROPERTY(QVariantMap rules READ rules NOTIFY changed)

public:
    explicit StyleTokens(const QString &guide, QObject *parent = nullptr);

    QVariantMap colors() const { return m_colors; }
    QVariantMap fonts() const { return m_fonts; }
    QVariantMap rules() const { return m_rules; }
    // { stiffness, damping } of the named rule, or empty when the guide has
    // no such rule, which check reports.
    Q_INVOKABLE QVariantMap rule(const QString &name) const { return m_rules.value(name).toMap(); }

    struct Tokens {
        QVariantMap colors, fonts, rules;
    };
    static Tokens read(const QString &guide);
    // The guide a scene answers to: its studio's, or one beside it.
    static QString guideFor(const QString &scenePath);
    // Makes Style available to scenes in this engine, read from guide.
    static void use(QQmlEngine *engine, const QString &guide);

signals:
    void changed();

private:
    void reload();

    QString m_guide;
    QFileSystemWatcher m_watcher;
    QVariantMap m_colors, m_fonts, m_rules;
};
