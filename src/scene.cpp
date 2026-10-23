#include "scene.h"
#include "studio.h"
#include "measure.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QJSValue>
#include <QMap>
#include <QUrl>
#include <QPainter>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QVariantMap>
#include <algorithm>
#include <cmath>

QList<Format> Formats::all() {
    return {{"16:9", {1920, 1080}}, {"9:16", {1080, 1920}}, {"1:1", {1080, 1080}}, {"4:5", {1080, 1350}}};
}

QStringList Formats::names() {
    QStringList names;
    for (const auto &format : all())
        names << format.name;
    return names;
}

bool Formats::find(const QString &name, Format *format) {
    const QString wanted = QString(name).trimmed().replace('x', ':');
    for (const auto &candidate : all())
        if (candidate.name == wanted) {
            if (format)
                *format = candidate;
            return true;
        }
    return false;
}

void registerSceneTypes() {
    static const bool registered = [] {
        qmlRegisterSingletonType(QUrl("qrc:/Motion.qml"), "Omotion", 1, 0, "Motion");
        return true;
    }();
    Q_UNUSED(registered);
}

namespace {
QString errorsText(const QList<QQmlError> &errors) {
    QStringList lines;
    for (const auto &error : errors)
        lines << error.toString();
    return lines.join('\n');
}

// A QML value as plain variants, whether it arrived as a JS array or a list.
QVariant plain(const QVariant &value) {
    return value.canConvert<QJSValue>() ? value.value<QJSValue>().toVariant() : value;
}

QString seconds(double t) { return QString::number(t, 'f', 2) + "s"; }

// Two descriptions of a state agree when they differ only in case, spacing,
// quotes, or a closing period.
QString comparable(QString text) {
    text = text.toLower().simplified();
    text.remove(QRegularExpression(R"([\"“”])"));
    while (text.endsWith('.'))
        text.chop(1);
    return text.trimmed();
}

// The scene's beats against the approved shot list, beat for beat. A scene
// that drifts from the shots the person approved builds a film nobody agreed
// to, so the structure is an error and the wording a warning.
QList<Problem> beatContract(const QList<Beat> &beats, const QString &root, double duration, int fps) {
    QList<Problem> problems;
    auto add = [&](bool error, const QString &message, double at = -1) {
        problems << Problem{error, message, {}, at};
    };
    for (const auto &beat : beats) {
        if (beat.purpose.isEmpty())
            add(true, QString("Beat \"%1\" has no purpose: say why it exists, as shotlist.md's \"why:\" does, "
                              "{ at: %2, name: \"%1\", purpose: \"...\", enter: \"...\", exit: \"...\" }.")
                          .arg(beat.name).arg(beat.at), beat.at);
        else if (beat.enter.isEmpty() || beat.exit.isEmpty())
            add(false, QString("Beat \"%1\" doesn't say its %2 state; copy it from shotlist.md.")
                           .arg(beat.name, beat.enter.isEmpty() ? "enter" : "exit"), beat.at);
    }
    const QList<Studio::ShotListBeat> shots = Studio::readShotList(root);
    if (shots.isEmpty())
        return problems;
    if (shots.size() != beats.size()) {
        QStringList names;
        for (const auto &shot : shots)
            names << shot.name;
        add(true, QString("The scene has %1 beats and shotlist.md has %2 (%3); they match one for one.")
                      .arg(beats.size()).arg(shots.size()).arg(names.join(", ")));
        return problems;
    }
    // Within half a frame, or the shot list's own rounding to a tenth.
    const double tolerance = qMax(0.5 / fps, 0.05);
    for (int i = 0; i < beats.size(); ++i) {
        const Beat &beat = beats[i];
        const auto &shot = shots[i];
        if (std::abs(beat.at - shot.start) > tolerance)
            add(true, QString("Beat \"%1\" starts at %2, but shotlist.md starts %3 at %4.")
                          .arg(beat.name, seconds(beat.at), shot.name, seconds(shot.start)), beat.at);
        if (comparable(beat.name) != comparable(shot.name))
            add(false, QString("Beat \"%1\" is \"%2\" in shotlist.md; use the same name.").arg(beat.name, shot.name),
                beat.at);
        const QList<QPair<QString, QPair<QString, QString>>> states{{"enter", {beat.enter, shot.enters}},
                                                                   {"exit", {beat.exit, shot.exits}}};
        for (const auto &[which, pair] : states)
            if (!pair.first.isEmpty() && !pair.second.isEmpty() && comparable(pair.first) != comparable(pair.second))
                add(false, QString("Beat \"%1\"'s %2 state differs from shotlist.md's \"%3\". Build to the "
                                   "approved state, or ask the person to change the shot list.")
                               .arg(beat.name, which, pair.second), beat.at);
    }
    if (std::abs(shots.last().end - duration) > tolerance)
        add(false, QString("shotlist.md ends at %1, but the scene lasts %2.")
                       .arg(seconds(shots.last().end), seconds(duration)));
    return problems;
}

QString typeName(const QObject *object);

// Every file the scene's objects load through a "source" (images, fonts,
// media), as an absolute path or, when it isn't a local file, the URL, each
// with the type that loads it. QML and JavaScript files are the scene's own
// components, not assets.
void collectSources(QObject *object, const QDir &folder, QMap<QString, QString> *sources) {
    const int index = object->metaObject()->indexOfProperty("source");
    if (index >= 0) {
        const QVariant value = object->metaObject()->property(index).read(object);
        const QUrl url = value.typeId() == QMetaType::QUrl ? value.toUrl() : QUrl(value.toString());
        if (!url.isEmpty() && url.scheme() != "qrc" && url.scheme() != "data") {
            const QString path = url.isRelative() ? folder.absoluteFilePath(url.path())
                                 : url.isLocalFile() ? url.toLocalFile()
                                                     : QString();
            const QString suffix = QFileInfo(path.isEmpty() ? url.path() : path).suffix().toLower();
            if (suffix != "qml" && suffix != "js" && suffix != "mjs")
                sources->insert(path.isEmpty() ? url.toString() : QFileInfo(path).absoluteFilePath(), typeName(object));
        }
    }
    for (QObject *child : object->children())
        collectSources(child, folder, sources);
    // Items a Repeater or Loader made are children of an item, not of it.
    if (auto *item = qobject_cast<QQuickItem *>(object))
        for (QQuickItem *child : item->childItems())
            if (child->parent() != object)
                collectSources(child, folder, sources);
}

// The files a studio's film loads against its assets: every file comes from
// assets/, where assets.md says where it came from, so the film never shows
// something nobody approved.
QList<Problem> assetManifest(const QMap<QString, QString> &sources, const QString &root) {
    QList<Problem> problems;
    const QDir studio(root);
    const QString assets = QDir(studio.filePath("assets")).absolutePath() + "/";
    for (auto it = sources.cbegin(); it != sources.cend(); ++it) {
        const QString &file = it.key();
        if (!QFileInfo(file).isAbsolute())
            problems << Problem{true, QString("%1 loads %2 from the network; download it into assets/ and list it "
                                              "in assets/assets.md with where it came from.").arg(it.value(), file),
                                {}, -1};
        else if (!file.startsWith(assets))
            problems << Problem{true, QString("%1 loads %2 from outside assets/; copy it into assets/ and list it "
                                              "in assets/assets.md with where it came from.")
                                          .arg(it.value(), studio.relativeFilePath(file)),
                                {}, -1};
        else if (!Studio::isListedAsset(root, file.mid(assets.size())))
            problems << Problem{false, QString("%1 isn't listed in assets/assets.md; add it with where it came from.")
                                           .arg(studio.relativeFilePath(file)),
                                {}, -1};
    }
    for (const auto &listed : Studio::listedAssetsNotFound(root))
        problems << Problem{true, QString("assets/assets.md lists %1, which isn't in assets/. Add the file, or mark "
                                          "the asset missing and ask the person for it.").arg(listed),
                            {}, -1};
    return problems;
}

QString typeName(const QObject *object) {
    QString name = QString::fromLatin1(object->metaObject()->className());
    name.remove(QRegularExpression("_QML(TYPE)?_\\d+$"));
    name.remove(QRegularExpression("^QQ(uick|ml)"));
    return name;
}

QString quoted(const QString &text) {
    const QString line = text.simplified();
    return "\"" + (line.size() > 32 ? line.left(31) + "…" : line) + "\"";
}


// The renderer's glyph cache can shift a few antialiased edge pixels by a level
// or two depending on what it drew before, which isn't the scene's doing. State
// kept between frames moves whole shapes, so only a clear difference counts.
bool sameFrame(const QImage &a, const QImage &b) {
    if (a.size() != b.size())
        return false;
    int different = 0;
    for (int y = 0; y < a.height(); ++y) {
        const QRgb *left = reinterpret_cast<const QRgb *>(a.constScanLine(y));
        const QRgb *right = reinterpret_cast<const QRgb *>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x)
            if (qAbs(qRed(left[x]) - qRed(right[x])) > 24 || qAbs(qGreen(left[x]) - qGreen(right[x])) > 24 ||
                qAbs(qBlue(left[x]) - qBlue(right[x])) > 24)
                if (++different >= 4)
                    return false;
    }
    return true;
}

struct TextIssue {
    const QQuickItem *item;
    QString text;
    bool cutOff = false;
};

// Text that lands outside the frame, or that its own item had to elide. Clipping
// ancestors hide what's outside them, so only the visible part has to fit.
void findTextIssues(QQuickItem *item, double opacity, const QRectF &clip, const QRectF &frame,
                    QList<TextIssue> *issues) {
    if (!item->isVisible())
        return;
    opacity *= item->opacity();
    if (opacity < 0.02)
        return;
    QRectF visible = clip;
    if (item->clip())
        visible &= item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
    if (item->inherits("QQuickText") || item->inherits("QQuickTextEdit")) {
        const QString text = item->property("text").toString();
        const QRectF shown = item->mapRectToScene(item->boundingRect()) & visible;
        if (!text.trimmed().isEmpty() && !shown.isEmpty() && !frame.adjusted(-1, -1, 1, 1).contains(shown))
            issues->append({item, text, false});
        if (item->property("truncated").toBool())
            issues->append({item, text, true});
    }
    for (QQuickItem *child : item->childItems())
        findTextIssues(child, opacity, visible, frame, issues);
}
}

Scene::Scene() = default;

Scene::~Scene() {
    // The scene's items belong to the window's content item; drop them while the
    // engine that runs their bindings still exists.
    m_window.reset();
    m_engine.reset();
}

bool Scene::load(const QString &path, const QString &formatName, QString *error, double scale) {
    m_scale = scale > 0 && scale <= 1 ? scale : 1;
    registerSceneTypes();
    m_root = nullptr;
    m_window.reset();
    m_engine.reset();
    m_warnings.clear();
    m_declarationProblems.clear();

    const QFileInfo info(path);
    if (!info.isFile()) {
        *error = path + " does not exist.";
        return false;
    }
    if (!Formats::find(formatName.isEmpty() ? "16:9" : formatName, &m_format)) {
        *error = QString("Unknown format %1; use %2.").arg(formatName, Formats::names().join(", "));
        return false;
    }
    m_path = info.absoluteFilePath();

    m_engine = std::make_unique<QQmlEngine>();
    QObject::connect(m_engine.get(), &QQmlEngine::warnings, [this](const QList<QQmlError> &warnings) {
        for (const auto &warning : warnings)
            m_warnings << warning.toString();
    });
    QQmlComponent component(m_engine.get(), QUrl::fromLocalFile(m_path), QQmlComponent::PreferSynchronous);
    if (component.isError()) {
        *error = errorsText(component.errors());
        return false;
    }
    QObject *object = component.beginCreate(m_engine->rootContext());
    if (!object) {
        *error = errorsText(component.errors());
        return false;
    }
    m_root = qobject_cast<QQuickItem *>(object);
    if (!m_root) {
        component.completeCreate();
        delete object;
        *error = m_path + ": the scene's root must be a visual type, such as Item or Rectangle.";
        return false;
    }

    m_window = std::make_unique<QQuickWindow>();
    m_window->setColor(Qt::black);
    m_window->resize((QSizeF(m_format.size) * m_scale).toSize());
    m_root->setParent(m_window->contentItem());
    m_root->setParentItem(m_window->contentItem());
    m_root->setSize(m_format.size);
    if (m_root->metaObject()->indexOfProperty("format") >= 0)
        m_root->setProperty("format", m_format.name);
    component.completeCreate();
    if (component.isError()) {
        *error = errorsText(component.errors());
        return false;
    }
    // After completion too, in case the scene bound its own size.
    m_root->setSize(m_format.size);
    if (m_scale < 1) {
        m_root->setTransformOrigin(QQuickItem::TopLeft);
        m_root->setScale(m_scale);
    }
    return readDeclarations(error);
}

bool Scene::readDeclarations(QString *error) {
    const QMetaObject *meta = m_root->metaObject();
    if (meta->indexOfProperty("t") < 0) {
        *error = m_path + ": declare \"property real t\", the time in seconds that omotion sets for every frame.";
        return false;
    }
    m_duration = m_root->property("duration").toDouble();
    if (meta->indexOfProperty("duration") < 0 || !(m_duration > 0) || m_duration > 3600) {
        *error = m_path + ": declare \"property real duration\" in seconds, above 0 and at most 3600.";
        return false;
    }
    m_fps = meta->indexOfProperty("fps") >= 0 ? m_root->property("fps").toInt() : 30;
    if (m_fps < 1 || m_fps > 120) {
        *error = m_path + ": fps must be between 1 and 120.";
        return false;
    }

    auto problem = [this](bool isError, const QString &message) {
        m_declarationProblems << Problem{isError, message, {}, -1};
    };

    m_beats.clear();
    if (meta->indexOfProperty("beats") >= 0) {
        const QVariantList list = plain(m_root->property("beats")).toList();
        for (int i = 0; i < list.size(); ++i) {
            const QVariantMap entry = plain(list[i]).toMap();
            bool number = false;
            const double at = entry.value("at").toDouble(&number);
            const QString name = entry.value("name").toString().trimmed();
            if (!number || name.isEmpty()) {
                problem(true, QString("Beat %1 needs a number \"at\" and a \"name\": { at: 2.0, name: \"Problem\" }.")
                                  .arg(i + 1));
                continue;
            }
            if (at < 0 || at >= m_duration)
                problem(true, QString("Beat \"%1\" starts at %2, outside the scene's %3.")
                                  .arg(name, seconds(at), seconds(m_duration)));
            else if (!m_beats.isEmpty() && at <= m_beats.last().at)
                problem(true, QString("Beat \"%1\" starts at %2, not after \"%3\"; list beats in order.")
                                  .arg(name, seconds(at), m_beats.last().name));
            else
                m_beats << Beat{at, name, entry.value("purpose").toString().trimmed(),
                                entry.value("enter").toString().trimmed(), entry.value("exit").toString().trimmed()};
        }
    }
    if (m_beats.isEmpty())
        problem(false, "No beats: name the moments the viewer should notice, so a contact sheet can show them.");
    else if (m_beats.first().at > 0)
        problem(false, QString("Nothing is named before the first beat at %1.").arg(seconds(m_beats.first().at)));

    m_formats.clear();
    if (meta->indexOfProperty("formats") >= 0) {
        for (const auto &value : plain(m_root->property("formats")).toList()) {
            Format format;
            if (Formats::find(value.toString(), &format))
                m_formats << format.name;
            else
                problem(true, QString("Unknown format %1 in formats; use %2.")
                                  .arg(value.toString(), Formats::names().join(", ")));
        }
    }
    if (m_formats.isEmpty())
        m_formats << "16:9";

    m_audio.clear();
    const QString audio = meta->indexOfProperty("audio") >= 0 ? m_root->property("audio").toString() : QString();
    if (!audio.isEmpty()) {
        const QString resolved = QFileInfo(audio).isAbsolute()
                                     ? audio : QFileInfo(m_path).absoluteDir().filePath(audio);
        if (QFileInfo(resolved).isFile())
            m_audio = QFileInfo(resolved).absoluteFilePath();
        else
            problem(true, "The audio file " + audio + " does not exist beside the scene.");
    }
    return true;
}

int Scene::frameCount() const { return qMax(1, int(std::lround(m_duration * m_fps))); }

QImage Scene::frame(double t) {
    if (!m_root)
        return {};
    m_root->setProperty("t", t);
    return m_window->grabWindow().convertToFormat(QImage::Format_RGB32);
}

namespace {
// Positioners and layouts place their children when they're polished, which
// otherwise happens only on the way to rendering.
void polish(QQuickItem *item) {
    item->ensurePolished();
    for (QQuickItem *child : item->childItems())
        polish(child);
}
}

void Scene::seek(double t) {
    if (!m_root)
        return;
    m_root->setProperty("t", t);
    polish(m_root);
}

QList<double> Scene::sheetTimes() const {
    QList<double> times;
    if (m_beats.isEmpty()) {
        for (int i = 0; i < 8; ++i)
            times << m_duration * (i + 0.5) / 8;
        return times;
    }
    for (int i = 0; i < m_beats.size(); ++i) {
        const double end = i + 1 < m_beats.size() ? m_beats[i + 1].at : m_duration;
        times << (m_beats[i].at + end) / 2;
    }
    return times;
}

QString Scene::beatNameAt(double t) const {
    QString name;
    for (const auto &beat : m_beats)
        if (beat.at <= t + 1e-9)
            name = beat.name;
    return name;
}

QImage Scene::contactSheet(const QList<double> &times, int columns, const QList<Strip> &strips) {
    const QSize frameSize = m_format.size;
    const bool portrait = frameSize.height() > frameSize.width();
    const bool square = frameSize.height() == frameSize.width();
    if (columns <= 0)
        columns = portrait ? 5 : square ? 4 : 3;
    columns = qMin(columns, qMax(1, int(times.size())));
    const int tileWidth = portrait ? 300 : square ? 400 : 560;
    const int tileHeight = tileWidth * frameSize.height() / frameSize.width();
    const int gap = 16, label = 52, header = 64;
    const int rows = (times.size() + columns - 1) / columns;
    const int width = gap + columns * (tileWidth + gap);
    // A strip's frames share the sheet's width, however many there are.
    int stripTile = 0, stripHeight = 0;
    for (const auto &strip : strips)
        if (!strip.times.isEmpty()) {
            stripTile = qMax(stripTile, (width - gap) / int(strip.times.size()) - gap);
            stripTile = qMin(stripTile, tileWidth);
        }
    for (const auto &strip : strips)
        if (!strip.times.isEmpty())
            stripHeight += header + stripTile * frameSize.height() / frameSize.width() + label + gap;

    QImage sheet(width, header + rows * (tileHeight + label + gap) + stripHeight, QImage::Format_RGB32);
    sheet.fill(QColor("#0e0e10"));
    QPainter painter(&sheet);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setRenderHint(QPainter::Antialiasing);

    QFont title = painter.font();
    title.setPixelSize(22);
    title.setBold(true);
    painter.setFont(title);
    painter.setPen(QColor("#f2f2f2"));
    painter.drawText(QRect(gap, 0, sheet.width() - 2 * gap, header), Qt::AlignVCenter | Qt::AlignLeft,
                     QString("%1   %2   %3   %4 fps")
                         .arg(QFileInfo(m_path).fileName(), m_format.name, seconds(m_duration))
                         .arg(m_fps));

    QFont name = painter.font();
    name.setPixelSize(16);
    QFont small = name;
    small.setBold(false);
    small.setPixelSize(13);
    for (int i = 0; i < times.size(); ++i) {
        const QRect tile(gap + (i % columns) * (tileWidth + gap), header + (i / columns) * (tileHeight + label + gap),
                         tileWidth, tileHeight);
        painter.drawImage(tile, frame(times[i]));
        painter.setPen(QColor("#3a3a40"));
        painter.drawRect(tile.adjusted(0, 0, -1, -1));

        const QString beat = beatNameAt(times[i]);
        const QRect text(tile.left(), tile.bottom() + 6, tileWidth, label - 10);
        painter.setPen(QColor("#f2f2f2"));
        painter.setFont(name);
        painter.drawText(text, Qt::AlignTop | Qt::AlignLeft,
                         QFontMetrics(name).elidedText(QString("%1  %2").arg(i + 1).arg(beat), Qt::ElideRight,
                                                       tileWidth));
        painter.setPen(QColor("#9a9aa2"));
        painter.setFont(small);
        painter.drawText(text, Qt::AlignBottom | Qt::AlignLeft,
                         QString("%1   frame %2").arg(seconds(times[i])).arg(int(std::lround(times[i] * m_fps))));
    }

    int top = header + rows * (tileHeight + label + gap);
    for (const auto &strip : strips) {
        if (strip.times.isEmpty())
            continue;
        const int stripTileHeight = stripTile * frameSize.height() / frameSize.width();
        painter.setPen(QColor("#f2f2f2"));
        painter.setFont(name);
        painter.drawText(QRect(gap, top, width - 2 * gap, header), Qt::AlignVCenter | Qt::AlignLeft, strip.heading);
        top += header;
        for (int i = 0; i < strip.times.size(); ++i) {
            const QRect tile(gap + i * (stripTile + gap), top, stripTile, stripTileHeight);
            painter.drawImage(tile, frame(strip.times[i]));
            painter.setPen(QColor("#3a3a40"));
            painter.drawRect(tile.adjusted(0, 0, -1, -1));
            painter.setPen(QColor("#9a9aa2"));
            painter.setFont(small);
            painter.drawText(QRect(tile.left(), tile.bottom() + 6, stripTile, label - 10), Qt::AlignTop | Qt::AlignLeft,
                             QString("%1   frame %2").arg(seconds(strip.times[i]))
                                 .arg(int(std::lround(strip.times[i] * m_fps))));
        }
        top += stripTileHeight + label + gap;
    }
    return sheet;
}

QList<Problem> Scene::check() {
    QList<Problem> problems = m_declarationProblems;
    // A film made with omotion new carries the skill its agent follows; an
    // outdated one leads the agent through an older workflow.
    const QString studio = Studio::rootOf(m_path);
    if (!studio.isEmpty()) {
        const QString skill = Studio::skillProblem(studio);
        if (!skill.isEmpty())
            problems << Problem{false, skill, {}, -1};
        // Only the studio's own film answers to its shot list; other QML
        // files beside it are components or experiments.
        if (QFileInfo(m_path) == QFileInfo(QDir(studio).filePath("scene.qml")))
            problems << beatContract(m_beats, studio, m_duration, m_fps);
    }
    for (const auto &warning : m_warnings)
        problems << Problem{false, warning, m_format.name, -1};

    // Anything that runs on its own clock makes a frame depend on when it was
    // rendered instead of on t.
    QSet<QString> reported;
    QList<QObject *> objects = m_root->findChildren<QObject *>();
    objects.prepend(m_root);
    static const QStringList clocks{"QQuickAbstractAnimation", "QQuickBehavior", "QQmlTimer",
                                    "QQuickFrameAnimation", "QQuickAnimatedImage", "QQuickAnimatedSprite",
                                    "QQuickSpriteSequence", "QQuickParticleSystem"};
    for (QObject *object : objects)
        for (const auto &clock : clocks)
            if (object->inherits(clock.toLatin1()) && !reported.contains(typeName(object))) {
                reported << typeName(object);
                problems << Problem{true, QString("%1 runs on the wall clock, so frames can't be asked for by time. "
                                                  "Compute the value from t instead (Motion.spring, Motion.progress).")
                                              .arg(typeName(object)),
                                    {}, -1};
            }

    QFile source(m_path);
    if (source.open(QIODevice::ReadOnly)) {
        static const QRegularExpression clockCall(R"(\bMath\.random\s*\(|\bDate\.now\s*\(|\bnew\s+Date\s*\()");
        const QStringList lines = QString::fromUtf8(source.readAll()).split('\n');
        for (int i = 0; i < lines.size(); ++i) {
            const auto match = clockCall.match(lines[i]);
            if (match.hasMatch())
                problems << Problem{true, QString("Line %1 calls %2), which differs on every render. Use "
                                                  "Motion.random(seed) or a value derived from t.")
                                              .arg(i + 1).arg(match.captured(0).remove(' ')),
                                    {}, -1};
        }
    }

    // Seekability: the same moments, asked for in opposite orders by two fresh
    // copies of the scene, must look identical.
    QList<double> times = sheetTimes();
    times << 0 << (frameCount() - 1) / double(m_fps);
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());

    const QRectF frameRect(QPointF(0, 0), QSizeF(m_format.size));
    QList<QImage> forward;
    QSet<QPair<const QQuickItem *, bool>> textReported;
    QMap<QString, QString> sources;
    if (!m_audio.isEmpty())
        sources.insert(m_audio, "audio");
    for (double t : times) {
        forward << frame(t);
        collectSources(m_root, QFileInfo(m_path).absoluteDir(), &sources);
        QList<TextIssue> issues;
        findTextIssues(m_root, 1, QRectF(-1e7, -1e7, 2e7, 2e7), frameRect, &issues);
        for (const auto &issue : issues) {
            const QPair<const QQuickItem *, bool> key{issue.item, issue.cutOff};
            if (textReported.contains(key))
                continue;
            textReported << key;
            problems << Problem{false,
                                issue.cutOff ? QString("Text %1 is cut off in %2.").arg(quoted(issue.text), m_format.name)
                                             : QString("Text %1 runs outside the %2 frame.")
                                                   .arg(quoted(issue.text), m_format.name),
                                m_format.name, t};
        }
    }

    if (!studio.isEmpty() && QFileInfo(m_path) == QFileInfo(QDir(studio).filePath("scene.qml"))) {
        problems << assetManifest(sources, studio);
        problems << Measure::style(*this, QDir(studio).filePath("style-guide.md"));
    }
    // The checklist's measurable items: text a viewer can read, and a reason
    // to stop scrolling.
    problems << Measure::text(*this);
    problems << Measure::hook(*this);

    Scene other;
    QString error;
    if (other.load(m_path, m_format.name, &error)) {
        for (int i = times.size() - 1; i >= 0; --i)
            if (!sameFrame(other.frame(times[i]), forward[i])) {
                problems << Problem{true, QString("The frame at %1 depends on what was rendered before it. Keep no "
                                                  "state between frames: derive everything from t.")
                                              .arg(seconds(times[i])),
                                    m_format.name, times[i]};
                break;
            }
    }
    return problems;
}

QString Scene::defaultOutput(const QString &scenePath, const QString &format) {
    const QFileInfo info(scenePath);
    Format found;
    Formats::find(format, &found);
    return info.absoluteDir().filePath("renders/" + info.completeBaseName() + "-" + found.slug() + ".mp4");
}

QString Scene::partialPath(const QString &output) {
    const QFileInfo info(output);
    return info.absoluteDir().filePath("." + info.completeBaseName() + ".part." + info.suffix());
}

bool Scene::render(const QString &output, QString *error, const std::function<void(int, int)> &progress) {
    const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
    if (ffmpeg.isEmpty()) {
        *error = "ffmpeg is not installed; install it with: omarchy-pkg-add ffmpeg";
        return false;
    }
    const QString target = QFileInfo(output).absoluteFilePath();
    if (!QDir().mkpath(QFileInfo(target).absolutePath())) {
        *error = "Could not create " + QFileInfo(target).absolutePath();
        return false;
    }
    const QString partial = partialPath(target);
    QFile::remove(partial);

    const QSize size = m_format.size;
    QStringList arguments{"-hide_banner", "-loglevel", "error", "-y",
                          "-f", "rawvideo", "-pix_fmt", "bgra",
                          "-s", QString("%1x%2").arg(size.width()).arg(size.height()),
                          "-r", QString::number(m_fps), "-i", "-"};
    if (!m_audio.isEmpty())
        arguments << "-i" << m_audio;
    arguments << "-map" << "0:v";
    if (!m_audio.isEmpty())
        arguments << "-map" << "1:a" << "-c:a" << "aac" << "-b:a" << "192k";
    arguments << "-t" << QString::number(frameCount() / double(m_fps), 'f', 6)
              << "-c:v" << "libx264" << "-preset" << "medium" << "-crf" << "18" << "-pix_fmt" << "yuv420p"
              << "-movflags" << "+faststart" << partial;

    QProcess encoder;
    encoder.setProcessChannelMode(QProcess::SeparateChannels);
    encoder.start(ffmpeg, arguments);
    if (!encoder.waitForStarted()) {
        *error = "Could not start ffmpeg: " + encoder.errorString();
        return false;
    }
    const int frames = frameCount();
    for (int i = 0; i < frames; ++i) {
        // Format_RGB32 is BGRA in memory on little-endian machines, which is
        // what ffmpeg is told to expect.
        const QImage image = frame(i / double(m_fps));
        if (image.size() != size) {
            encoder.kill();
            encoder.waitForFinished();
            QFile::remove(partial);
            *error = QString("A frame came back at %1x%2 instead of %3x%4.")
                         .arg(image.width()).arg(image.height()).arg(size.width()).arg(size.height());
            return false;
        }
        for (int y = 0; y < size.height(); ++y)
            encoder.write(reinterpret_cast<const char *>(image.constScanLine(y)), size.width() * 4);
        while (encoder.bytesToWrite() > 0 && encoder.state() == QProcess::Running)
            encoder.waitForBytesWritten(1000);
        if (encoder.state() != QProcess::Running)
            break;
        if (progress)
            progress(i + 1, frames);
    }
    encoder.closeWriteChannel();
    encoder.waitForFinished(-1);
    if (encoder.exitStatus() != QProcess::NormalExit || encoder.exitCode() != 0) {
        QFile::remove(partial);
        const QString message = QString::fromUtf8(encoder.readAllStandardError()).trimmed();
        *error = "ffmpeg failed" + (message.isEmpty() ? QString(".") : ": " + message);
        return false;
    }
    if (QFileInfo::exists(target) && !QFile::remove(target)) {
        QFile::remove(partial);
        *error = "Could not replace " + target;
        return false;
    }
    if (!QFile::rename(partial, target)) {
        QFile::remove(partial);
        *error = "Could not move the render into " + target;
        return false;
    }
    return true;
}
