#include "studio.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>

namespace {
const char skillPath[] = ".claude/skills/omotion/SKILL.md";

QByteArray resource(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool write(const QString &path, const QByteArray &contents, QString *error) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        *error = "Could not create " + QFileInfo(path).absolutePath();
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) < 0 || !file.commit()) {
        *error = path + ": " + file.errorString();
        return false;
    }
    return true;
}

QString skillHash(const QByteArray &text) {
    return QString::fromLatin1(QCryptographicHash::hash(text, QCryptographicHash::Sha256).toHex().left(12));
}

const QRegularExpression &stampPattern() {
    static const QRegularExpression pattern(R"(<!-- omotion (\S+) skill ([0-9a-f]+) -->)");
    return pattern;
}
}

QByteArray Studio::stampedSkill() {
    const QByteArray text = resource(":/skill.md");
    return text + QString("\n<!-- omotion %1 skill %2 -->\n").arg(omotionVersion, skillHash(text)).toUtf8();
}

bool Studio::create(const QString &folder, QString *error, QStringList *created) {
    const QDir directory(folder);
    if (directory.exists() && !directory.isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)) {
        *error = folder + " already has files in it; pick a new folder for the film.";
        return false;
    }
    if (QFileInfo(folder).exists() && !QFileInfo(folder).isDir()) {
        *error = folder + " is a file; pick a new folder for the film.";
        return false;
    }
    const QString root = QFileInfo(folder).absoluteFilePath();
    const QList<QPair<QString, QString>> files{
        {"brief.md", ":/studio/brief.md"},
        {"style-guide.md", ":/studio/style-guide.md"},
        {"shotlist.md", ":/studio/shotlist.md"},
        {"assets/assets.md", ":/studio/assets.md"},
        {"gates.md", ":/studio/gates.md"},
        {"scene.qml", ":/starter.qml"},
    };
    QStringList written;
    for (const auto &[name, source] : files) {
        if (!write(QDir(root).filePath(name), resource(source), error))
            return false;
        written << name;
    }
    for (const auto &name : {"reviews", "renders"}) {
        if (!QDir(root).mkpath(name)) {
            *error = "Could not create " + QDir(root).filePath(name);
            return false;
        }
        written << QString(name) + "/";
    }
    QStringList skill;
    if (!installSkill(root, error, &skill))
        return false;
    if (created)
        *created = written + skill;
    return true;
}

QString Studio::rootOf(const QString &path) {
    QFileInfo info(path);
    QString folder = info.isDir() ? info.absoluteFilePath() : info.absolutePath();
    while (!folder.isEmpty()) {
        if (QFileInfo(QDir(folder).filePath("gates.md")).isFile())
            return folder;
        const QString parent = QFileInfo(folder).absolutePath();
        if (parent == folder)
            break;
        folder = parent;
    }
    return {};
}

bool Studio::installSkill(const QString &root, QString *error, QStringList *written) {
    if (!write(QDir(root).filePath(skillPath), stampedSkill(), error))
        return false;
    QStringList files{skillPath};
    // AGENTS.md is how agents other than Claude Code find the skill. One the
    // person already has is theirs, so it's left alone.
    const QString agents = QDir(root).filePath("AGENTS.md");
    if (!QFileInfo::exists(agents)) {
        if (!write(agents, resource(":/studio/AGENTS.md"), error))
            return false;
        files << "AGENTS.md";
    }
    if (written)
        *written = files;
    return true;
}

QString Studio::skillProblem(const QString &root) {
    QFile file(QDir(root).filePath(skillPath));
    if (!file.open(QIODevice::ReadOnly))
        return QString("This film has no agent skill. Run \"omotion skill install\" in %1 to add it.").arg(root);
    const auto match = stampPattern().match(QString::fromUtf8(file.readAll()));
    if (!match.hasMatch())
        return {};
    const QString current = skillHash(resource(":/skill.md"));
    if (match.captured(2) == current)
        return {};
    return QString("This film's agent skill came from omotion %1 and differs from this omotion's. Run "
                   "\"omotion skill install\" in %2 to update .claude/skills/omotion/SKILL.md.")
        .arg(match.captured(1), root);
}
