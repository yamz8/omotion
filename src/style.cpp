#include "style.h"
#include "studio.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QQmlEngine>
#include <QRegularExpression>

StyleTokens::StyleTokens(const QString &guide, QObject *parent) : QObject(parent), m_guide(guide) {
    // A guide replaced by a save is a new file, so the watch is set again.
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, &StyleTokens::reload);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &StyleTokens::reload);
    if (!guide.isEmpty())
        m_watcher.addPath(QFileInfo(guide).absolutePath());
    reload();
}

void StyleTokens::reload() {
    if (!m_guide.isEmpty() && QFileInfo::exists(m_guide) && !m_watcher.files().contains(m_guide))
        m_watcher.addPath(m_guide);
    const Tokens tokens = read(m_guide);
    if (tokens.colors == m_colors && tokens.fonts == m_fonts && tokens.rules == m_rules)
        return;
    m_colors = tokens.colors;
    m_fonts = tokens.fonts;
    m_rules = tokens.rules;
    emit changed();
}

StyleTokens::Tokens StyleTokens::read(const QString &guide) {
    Tokens tokens;
    QFile file(guide);
    if (guide.isEmpty() || !file.open(QIODevice::ReadOnly))
        return tokens;
    bool inTokens = false;
    static const QRegularExpression line(R"(^\s*(color|font|rule)\s+([A-Za-z_][\w-]*)\s+(.+?)\s*$)");
    for (const QString &text : QString::fromUtf8(file.readAll()).split('\n')) {
        const QString trimmed = text.trimmed();
        // A heading in capitals starts a section; TOKENS is the one read here.
        if (!trimmed.isEmpty() && trimmed == trimmed.toUpper() && trimmed.at(0).isLetter() && !trimmed.contains(' ')) {
            inTokens = trimmed == "TOKENS";
            continue;
        }
        if (!inTokens)
            continue;
        const auto match = line.match(text);
        if (!match.hasMatch())
            continue;
        const QString kind = match.captured(1), name = match.captured(2), value = match.captured(3);
        if (kind == "color" && QColor::isValidColorName(value))
            tokens.colors.insert(name, QColor::fromString(value).name(QColor::HexArgb).replace("#ff", "#"));
        else if (kind == "font")
            tokens.fonts.insert(name, value);
        else if (kind == "rule") {
            const QStringList numbers = value.split(QRegularExpression(R"(\s+)"));
            bool a = false, b = false;
            const double stiffness = numbers.value(0).toDouble(&a), damping = numbers.value(1).toDouble(&b);
            if (a && b && stiffness > 0 && damping > 0)
                tokens.rules.insert(name, QVariantMap{{"stiffness", stiffness}, {"damping", damping}});
        }
    }
    return tokens;
}

QString StyleTokens::guideFor(const QString &scenePath) {
    const QString studio = Studio::rootOf(scenePath);
    const QString beside = QFileInfo(scenePath).absoluteDir().filePath("style-guide.md");
    if (!studio.isEmpty())
        return QDir(studio).filePath("style-guide.md");
    return QFileInfo::exists(beside) ? beside : QString();
}

void StyleTokens::use(QQmlEngine *engine, const QString &guide) {
    // The singleton is made once per engine, from the guide set here first.
    engine->setProperty("omotionStyleGuide", guide);
    if (auto *existing = engine->property("omotionStyle").value<StyleTokens *>()) {
        if (existing->m_guide != guide) {
            existing->m_watcher.removePaths(existing->m_watcher.files() + existing->m_watcher.directories());
            existing->m_guide = guide;
            if (!guide.isEmpty())
                existing->m_watcher.addPath(QFileInfo(guide).absolutePath());
            existing->reload();
        }
    }
}
