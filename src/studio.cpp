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
        {"notes.md", ":/studio/notes.md"},
        {"activity.md", ":/studio/activity.md"},
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

bool Studio::Gate::waitingForPerson() const {
    return !passed && status.contains("waiting for the person", Qt::CaseInsensitive);
}

QList<Studio::Gate> Studio::readGates(const QString &root) {
    QList<Gate> gates;
    QFile file(QDir(root).filePath("gates.md"));
    if (!file.open(QIODevice::ReadOnly))
        return gates;
    static const QRegularExpression line(R"(^\s*- \[([ xX])\] (\d+) (.+?)\s*$)");
    for (const auto &text : QString::fromUtf8(file.readAll()).split('\n')) {
        const auto match = line.match(text);
        if (!match.hasMatch())
            continue;
        Gate gate;
        gate.passed = match.captured(1) != " ";
        gate.number = match.captured(2).toInt();
        const QString rest = match.captured(3);
        const int colon = rest.indexOf(": ");
        const int paren = rest.lastIndexOf(" (");
        if (colon > 0) {
            gate.title = rest.left(colon);
            gate.status = rest.mid(colon + 2);
        } else if (paren > 0 && rest.endsWith(')')) {
            gate.title = rest.left(paren);
            gate.status = rest.mid(paren + 2, rest.size() - paren - 3);
        } else {
            gate.title = rest;
        }
        gates << gate;
    }
    return gates;
}

namespace {
const QRegularExpression &noteHeading() {
    static const QRegularExpression pattern(
        R"re(^- \[([ xX])\] (\d+) · ([\d.]+)s · ([^·]+?) · ([^·]*?) · \((-?\d+), (-?\d+)\)(?: · on "(.*)")?\s*$)re");
    return pattern;
}
}

QList<Studio::Note> Studio::readNotes(const QString &root) {
    QList<Note> notes;
    QFile file(QDir(root).filePath("notes.md"));
    if (!file.open(QIODevice::ReadOnly))
        return notes;
    for (const auto &line : QString::fromUtf8(file.readAll()).split('\n')) {
        const auto heading = noteHeading().match(line);
        if (heading.hasMatch()) {
            Note note;
            note.done = heading.captured(1) != " ";
            note.id = heading.captured(2);
            note.at = heading.captured(3).toDouble();
            note.format = heading.captured(4).trimmed();
            note.beat = heading.captured(5).trimmed();
            note.point = {heading.captured(6).toInt(), heading.captured(7).toInt()};
            note.target = heading.captured(8);
            notes << note;
            continue;
        }
        if (notes.isEmpty() || !line.startsWith("  "))
            continue;
        const QString detail = line.trimmed();
        Note &note = notes.last();
        if (detail.startsWith("evidence:"))
            note.evidence = detail.mid(9).trimmed();
        else if (detail.startsWith("note:"))
            note.text = detail.mid(5).trimmed();
        else if (detail.startsWith("fixed:") || detail.startsWith("reply:"))
            note.answers << detail;
        else if (!note.answers.isEmpty())
            note.answers.last() += " " + detail;
        else if (!note.text.isEmpty())
            note.text += " " + detail;
    }
    return notes;
}

QString Studio::addNote(const QString &root, Note note, QString *error) {
    const QString path = QDir(root).filePath("notes.md");
    QByteArray contents;
    QFile existing(path);
    if (existing.open(QIODevice::ReadOnly))
        contents = existing.readAll();
    else
        contents = resource(":/studio/notes.md");
    int last = 0;
    for (const auto &other : readNotes(root))
        last = qMax(last, other.id.toInt());
    note.id = QString("%1").arg(last + 1, 3, 10, QChar('0'));
    note.evidence = "reviews/notes/" + note.id + ".png";

    // The heading's fields are separated by "·", so none of them may hold one.
    auto oneLine = [](QString text) {
        return text.replace('\n', ' ').replace('"', '\'').replace(QString("·"), "-").simplified();
    };
    QString entry = QString("- [ ] %1 · %2s · %3 · %4 · (%5, %6)")
                        .arg(note.id, noteTime(note.at), oneLine(note.format),
                             note.beat.isEmpty() ? QString("no beat") : oneLine(note.beat))
                        .arg(note.point.x())
                        .arg(note.point.y());
    if (!note.target.trimmed().isEmpty())
        entry += " · on \"" + oneLine(note.target).left(60) + "\"";
    entry += "\n";
    entry += "  evidence: " + note.evidence + "\n";
    entry += "  note: " + oneLine(note.text) + "\n";

    if (!contents.endsWith('\n'))
        contents += '\n';
    contents += '\n' + entry.toUtf8();
    if (!write(path, contents, error))
        return {};
    return note.id;
}

QString Studio::noteTime(double at) {
    QString text = QString::number(at, 'f', 3);
    if (text.endsWith('0'))
        text.chop(1);
    return text;
}

QString Studio::noteReference(const Note &note) {
    QString where = QString("%1s in %2").arg(noteTime(note.at), note.format);
    if (!note.beat.isEmpty() && note.beat != "no beat")
        where += ", beat " + note.beat;
    if (!note.target.isEmpty())
        where += ", on \"" + note.target + "\"";
    return QString("Note %1 in notes.md (%2): %3").arg(note.id, where, note.text) +
           (note.evidence.isEmpty() ? QString() : QString(" Evidence: %1.").arg(note.evidence));
}

QStringList Studio::missingAssets(const QString &root) {
    QStringList missing;
    QFile file(QDir(root).filePath("assets/assets.md"));
    if (!file.open(QIODevice::ReadOnly))
        return missing;
    for (const auto &line : QString::fromUtf8(file.readAll()).split('\n')) {
        if (!line.trimmed().startsWith('|') || !line.contains("missing", Qt::CaseInsensitive))
            continue;
        const QStringList cells = line.split('|', Qt::SkipEmptyParts);
        if (!cells.isEmpty() && !cells.first().trimmed().startsWith("---"))
            missing << cells.first().trimmed();
    }
    return missing;
}

QStringList Studio::readActivity(const QString &root) {
    QStringList entries;
    QFile file(QDir(root).filePath("activity.md"));
    if (!file.open(QIODevice::ReadOnly))
        return entries;
    for (const auto &line : QString::fromUtf8(file.readAll()).split('\n'))
        if (line.startsWith("- "))
            entries.prepend(line.mid(2).trimmed());
    return entries;
}
