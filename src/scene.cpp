#include "scene.h"
#include "studio.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QJSValue>
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
    return {{"16:9", {1920, 1080}}, {"9:16", {1080, 1920}}, {"1:1", {1080, 1080}}};
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

QString seconds(double t) { return QString::number(t, 'f', 2) + "s"; }

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

bool Scene::load(const QString &path, const QString &formatName, QString *error) {
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
    m_window->resize(m_format.size);
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
                m_beats << Beat{at, name};
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

QImage Scene::contactSheet(const QList<double> &times, int columns) {
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

    QImage sheet(gap + columns * (tileWidth + gap), header + rows * (tileHeight + label + gap),
                 QImage::Format_RGB32);
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
    for (double t : times) {
        forward << frame(t);
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
