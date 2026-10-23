#include "measure.h"
#include "style.h"

#include <QColor>
#include <QFile>
#include <QFont>
#include <QFontInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineF>
#include <QMap>
#include <QQuickItem>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <numeric>
#include <cmath>

namespace {
QString seconds(double t) { return QString::number(t, 'f', 2) + "s"; }

QString quoted(const QString &text) {
    const QString line = text.simplified();
    return "\"" + (line.size() > 32 ? line.left(31) + "…" : line) + "\"";
}

// Plain characters a viewer reads, without any markup.
QString readable(QString text) {
    text.remove(QRegularExpression("<[^>]*>"));
    return text.simplified();
}

bool isText(const QQuickItem *item) { return item->inherits("QQuickText") || item->inherits("QQuickTextEdit"); }

// Something drawn that text can run into: a picture, a filled or outlined
// rectangle, or other text.
bool isDrawn(const QQuickItem *item) {
    if (isText(item) || item->inherits("QQuickImage"))
        return true;
    if (item->inherits("QQuickRectangle")) {
        const QColor fill = item->property("color").value<QColor>();
        const QObject *border = item->property("border").value<QObject *>();
        const bool outlined = border && border->property("width").toDouble() > 0 &&
                              border->property("color").value<QColor>().alphaF() > 0.05;
        return fill.alphaF() > 0.05 || outlined;
    }
    return false;
}

// The size text is drawn at on the frame, after every scale above it.
double drawnPixels(const QQuickItem *item) {
    const QFont font = item->property("font").value<QFont>();
    const double size = font.pixelSize() > 0 ? font.pixelSize() : QFontInfo(font).pixelSize();
    return size * QLineF(item->mapToScene(QPointF(0, 0)), item->mapToScene(QPointF(0, 1))).length();
}

struct Seen {
    QQuickItem *item;
    QRectF rect;    // On the frame, as far as it's visible.
    QRectF full;    // On the frame, all of it.
    double opacity; // Including every ancestor's.
};

void walk(QQuickItem *item, double opacity, const QRectF &clip, QList<Seen> *seen) {
    if (!item->isVisible())
        return;
    opacity *= item->opacity();
    if (opacity < 0.02)
        return;
    QRectF visible = clip;
    if (item->clip())
        visible &= item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
    if (isDrawn(item)) {
        const QRectF full = isText(item) ? item->mapRectToScene(item->boundingRect())
                                         : item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
        const QRectF shown = full & visible;
        if (!shown.isEmpty() && (!isText(item) || !readable(item->property("text").toString()).isEmpty()))
            seen->append({item, shown, full, opacity});
    }
    for (QQuickItem *child : item->childItems())
        walk(child, opacity, visible, seen);
}

// Where a text's letters are: its line boxes without the leading above and
// below, which is about a fifth of a line at most sizes.
QRectF glyphBox(const Seen &seen) {
    const int lines = qMax(1, seen.item->property("lineCount").toInt());
    const double inset = seen.rect.height() / lines * 0.2;
    return seen.rect.adjusted(0, inset, 0, -inset);
}

bool isAncestor(const QQuickItem *ancestor, const QQuickItem *item) {
    for (const QQuickItem *parent = item->parentItem(); parent; parent = parent->parentItem())
        if (parent == ancestor)
            return true;
    return false;
}

// One text as the viewer sees it: an item showing one string.
struct Shown {
    QQuickItem *item = nullptr;
    QString text;
    double peak = 0;       // Its highest opacity anywhere in the film.
    QList<int> frames;     // Frames where it's at full strength and whole.
    QHash<int, Seen> at;   // What it looked like at each of those frames.
};

struct Run {
    int first = 0;
    int length = 0;
};

Run longestRun(const QList<int> &frames) {
    Run best, current;
    for (int i = 0; i < frames.size(); ++i) {
        if (i > 0 && frames[i] == frames[i - 1] + 1)
            ++current.length;
        else
            current = {frames[i], 1};
        if (current.length > best.length)
            best = current;
    }
    return best;
}
}

QRectF Measure::safeArea(const Format &format) {
    const QSizeF size(format.size);
    // Reels, Stories, and TikTok lay their captions and buttons over the
    // bottom fifth and the account over the top; feeds and players over little.
    if (format.name == "9:16")
        return QRectF(size.width() * 0.06, size.height() * 0.12, size.width() * 0.88, size.height() * 0.68);
    return QRectF(size.width() * 0.05, size.height() * 0.05, size.width() * 0.9, size.height() * 0.9);
}

QList<Problem> Measure::text(Scene &scene) {
    const Format format = scene.format();
    const QRectF frame(QPointF(0, 0), QSizeF(format.size));
    const QRectF safe = safeArea(format);
    const int frames = scene.frameCount();
    const double fps = scene.fps();

    // Every frame, without rendering: each text and where it is, and what's
    // drawn around it.
    QMap<QPair<QQuickItem *, QString>, Shown> texts;
    QHash<QQuickItem *, QStringList> strings; // Each item's texts, in the order they appear.
    QHash<QPair<QQuickItem *, QQuickItem *>, QList<int>> overlaps;
    QList<QList<Seen>> drawn(frames);
    for (int i = 0; i < frames; ++i) {
        scene.seek(i / fps);
        walk(scene.root(), 1, QRectF(-1e7, -1e7, 2e7, 2e7), &drawn[i]);
        for (const Seen &seen : drawn[i]) {
            if (!isText(seen.item))
                continue;
            const QString text = readable(seen.item->property("text").toString());
            Shown &shown = texts[{seen.item, text}];
            shown.item = seen.item;
            shown.text = text;
            shown.peak = qMax(shown.peak, seen.opacity);
            if (!strings[seen.item].contains(text))
                strings[seen.item] << text;
        }
    }
    // A text counts as shown where it's at its own full strength and whole.
    for (int i = 0; i < frames; ++i)
        for (const Seen &seen : drawn[i]) {
            if (!isText(seen.item))
                continue;
            Shown &shown = texts[{seen.item, readable(seen.item->property("text").toString())}];
            if (shown.peak >= 0.3 && seen.opacity >= shown.peak * 0.95 && seen.rect == seen.full &&
                frame.adjusted(-1, -1, 1, 1).contains(seen.full)) {
                shown.frames << i;
                shown.at.insert(i, seen);
            }
        }
    // Only fully shown text runs into things: at full strength, against
    // anything drawn that isn't its own container or the background.
    for (int i = 0; i < frames; ++i)
        for (const Seen &a : drawn[i]) {
            if (!isText(a.item))
                continue;
            const Shown &shown = texts.value({a.item, readable(a.item->property("text").toString())});
            if (!shown.at.contains(i))
                continue;
            for (const Seen &b : drawn[i]) {
                if (b.item == a.item || isAncestor(b.item, a.item) || isAncestor(a.item, b.item) || b.opacity < 0.3)
                    continue;
                if (b.full.width() * b.full.height() >= frame.width() * frame.height() * 0.9)
                    continue;
                const QRectF both = a.rect & b.rect;
                if (both.isEmpty())
                    continue;
                bool runsInto;
                if (isText(b.item)) {
                    // Text over text: their glyphs, not their line boxes, which
                    // tight leading overlaps on purpose.
                    const QRectF glyphs = glyphBox(a) & glyphBox(b);
                    const double smaller = qMin(a.rect.width() * a.rect.height(), b.rect.width() * b.rect.height());
                    runsInto = a.item < b.item && !glyphs.isEmpty() && // Each pair once.
                               glyphs.width() * glyphs.height() > smaller * 0.05;
                } else {
                    // Text set on a card or picture is placed there; text that
                    // crosses its edge has run into it.
                    runsInto = !b.rect.contains(a.rect);
                }
                if (runsInto)
                    overlaps[{a.item, b.item}] << i;
            }
        }

    QList<Problem> problems;
    QSet<QQuickItem *> safeReported;
    // Small text, by size: one decision about a size is one problem to fix.
    struct Small {
        QStringList texts;
        double at = -1;
        QRectF area;
    };
    QMap<int, Small> small;
    for (auto it = texts.cbegin(); it != texts.cend(); ++it) {
        const Shown &shown = it.value();
        // Text that types out or counts up is read in its final form.
        const QStringList &all = strings.value(shown.item);
        if (all.size() > 5 ? shown.text != all.last()
                           : std::any_of(all.cbegin(), all.cend(), [&](const QString &other) {
                                 return other.size() > shown.text.size() && other.startsWith(shown.text);
                             }))
            continue;
        if (shown.frames.isEmpty())
            continue;
        const Run run = longestRun(shown.frames);
        const Seen &settled = shown.at.value(shown.frames[shown.frames.indexOf(run.first) + run.length / 2]);
        const double settledAt = shown.frames[shown.frames.indexOf(run.first) + run.length / 2] / fps;

        const double held = run.length / fps;
        const double needed = qMax(0.6, shown.text.size() / 25.0);
        if (held + 0.5 / fps < needed)
            problems << Problem{true,
                                QString("Text %1 is fully on screen for %2 s (%3 to %4); its %5 characters need %6 s "
                                        "(at least 0.6 s, or one second per 25 characters).")
                                    .arg(quoted(shown.text), QString::number(held, 'f', 2), seconds(run.first / fps),
                                         seconds((run.first + run.length) / fps))
                                    .arg(shown.text.size())
                                    .arg(QString::number(needed, 'f', 2)),
                                format.name, run.first / fps, settled.full};

        const double pixels = drawnPixels(shown.item);
        const double onPhone = pixels * phoneWidth / format.size.width();
        if (onPhone + 1e-6 < readablePixels) {
            Small &group = small[qRound(pixels)];
            if (!group.texts.contains(quoted(shown.text)))
                group.texts << quoted(shown.text);
            if (group.at < 0 || settledAt < group.at) {
                group.at = settledAt;
                group.area = settled.full;
            }
        }
        if (!safe.contains(settled.full) && !safeReported.contains(shown.item)) {
            safeReported << shown.item;
            problems << Problem{false,
                                QString("Text %1 crosses the %2 safe area (%3), where the platform's own controls "
                                        "can cover it.")
                                    .arg(quoted(shown.text), format.name,
                                         format.name == "9:16" ? "the top 12%, the bottom 20%, and 6% at each side"
                                                               : "5% at each edge"),
                                format.name, settledAt, settled.full};
        }
    }
    for (auto it = small.cbegin(); it != small.cend(); ++it) {
        const double onPhone = it.key() * phoneWidth / format.size.width();
        const QStringList &texts = it.value().texts;
        problems << Problem{true,
                            QString("Text at %1 px is %2 px when the film plays %3 px wide on a phone; make it at "
                                    "least %4 px in %5: %6.")
                                .arg(it.key())
                                .arg(QString::number(onPhone, 'f', 1), QString::number(phoneWidth, 'f', 0))
                                .arg(int(std::ceil(readablePixels * format.size.width() / phoneWidth)))
                                .arg(format.name,
                                     texts.size() > 4 ? texts.mid(0, 4).join(", ") + QString(", and %1 more")
                                                                                           .arg(texts.size() - 4)
                                                      : texts.join(", ")),
                            format.name, it.value().at, it.value().area};
    }
    for (auto it = overlaps.cbegin(); it != overlaps.cend(); ++it) {
        // A pass across something is motion; a fifth of a second or more is a
        // collision the viewer sees.
        const Run run = longestRun(it.value());
        if (run.length < qMax(3.0, 0.2 * fps))
            continue;
        QQuickItem *a = it.key().first;
        QQuickItem *b = it.key().second;
        // Read both as they were when they met.
        scene.seek(run.first / fps);
        const QString other = isText(b) ? "text " + quoted(b->property("text").toString())
                              : b->inherits("QQuickImage")
                                  ? "the image " + b->property("source").toUrl().fileName()
                                  : QString("a %1 x %2 rectangle").arg(qRound(b->width())).arg(qRound(b->height()));
        const QRectF area = a->mapRectToScene(a->boundingRect()) | b->mapRectToScene(QRectF(0, 0, b->width(), b->height()));
        problems << Problem{false,
                            QString("Text %1 runs into %2 from %3 to %4.")
                                .arg(quoted(a->property("text").toString()), other, seconds(run.first / fps),
                                     seconds((run.first + run.length) / fps)),
                            format.name, run.first / fps, area};
    }
    std::stable_sort(problems.begin(), problems.end(),
                     [](const Problem &a, const Problem &b) { return a.at < b.at; });
    return problems;
}

namespace {
// Letters and digits only, so "JetBrainsMono Nerd Font" and "JetBrains Mono
// (Nerd Font build)" compare by their name.
QString squeezed(const QString &text) {
    QString out;
    for (const QChar c : text)
        if (c.isLetterOrNumber())
            out += c.toLower();
    return out;
}

bool guideNamesFont(const QString &guide, QString family) {
    family.remove(QRegularExpression(R"(\s+(Nerd Font( Mono| Propo)?|NF|NFM|NFP|Variable)$)",
                                     QRegularExpression::CaseInsensitiveOption));
    return squeezed(guide).contains(squeezed(family));
}
}

QList<Problem> Measure::style(Scene &scene, const QString &styleGuide) {
    QList<Problem> problems;
    QFile file(styleGuide);
    if (!file.open(QIODevice::ReadOnly))
        return problems;
    const QString guide = QString::fromUtf8(file.readAll());
    QFile original(":/studio/style-guide.md");
    if (original.open(QIODevice::ReadOnly) && guide == QString::fromUtf8(original.readAll()))
        return problems;

    QSet<QString> palette;
    static const QRegularExpression hex(R"(#([0-9a-fA-F]{8}|[0-9a-fA-F]{6}|[0-9a-fA-F]{3})\b)");
    for (auto it = hex.globalMatch(guide); it.hasNext();) {
        const QColor color(it.next().captured(0));
        if (color.isValid())
            palette << color.name(QColor::HexRgb);
    }
    if (palette.isEmpty())
        return problems; // A guide without measured colors has no palette to hold the film to.
    // Colors the guide names in words ("white type on black").
    for (const QString &name : QColor::colorNames())
        if (QRegularExpression("\\b" + name + "\\b", QRegularExpression::CaseInsensitiveOption).match(guide).hasMatch())
            palette << QColor(name).name(QColor::HexRgb);

    // Colors and fonts as they appear, beat by beat: the middle of each.
    QMap<QString, QString> colors; // #rrggbb -> where it first appears
    QMap<QString, QString> fonts;
    const QList<double> times = scene.sheetTimes();
    for (double t : times) {
        scene.seek(t);
        QList<Seen> seen;
        walk(scene.root(), 1, QRectF(-1e7, -1e7, 2e7, 2e7), &seen);
        const QString beat = scene.beatNameAt(t);
        const QString where = beat.isEmpty() ? seconds(t) : QString("beat \"%1\"").arg(beat);
        for (const Seen &s : seen) {
            QList<QColor> used;
            if (isText(s.item)) {
                used << s.item->property("color").value<QColor>();
                const QString family = s.item->property("font").value<QFont>().family();
                if (!family.isEmpty() && !guideNamesFont(guide, family) && !fonts.contains(family))
                    fonts.insert(family, where);
            } else if (s.item->inherits("QQuickRectangle")) {
                used << s.item->property("color").value<QColor>();
                if (const QObject *border = s.item->property("border").value<QObject *>();
                    border && border->property("width").toDouble() > 0)
                    used << border->property("color").value<QColor>();
            }
            for (const QColor &color : used) {
                if (!color.isValid() || color.alphaF() < 0.05)
                    continue;
                const QString name = color.name(QColor::HexRgb);
                if (!palette.contains(name) && !colors.contains(name))
                    colors.insert(name, where);
            }
        }
    }
    QStringList list;
    for (auto it = colors.cbegin(); it != colors.cend(); ++it)
        list << QString("%1 (%2)").arg(it.key(), it.value());
    if (!list.isEmpty())
        problems << Problem{false,
                            QString("Colors that style-guide.md's palette doesn't name: %1. Use the guide's, or add "
                                    "them to it.").arg(list.join(", ")),
                            {}, -1, {}};
    list.clear();
    for (auto it = fonts.cbegin(); it != fonts.cend(); ++it)
        list << QString("%1 (%2)").arg(it.key(), it.value());
    if (!list.isEmpty())
        problems << Problem{false,
                            QString("Fonts that style-guide.md doesn't name: %1. Load the guide's from assets/ with "
                                    "FontLoader, or add them to it.").arg(list.join(", ")),
                            {}, -1, {}};
    return problems;
}

QList<Problem> Measure::rules(const QString &scenePath, const QString &styleGuide) {
    QList<Problem> problems;
    const QVariantMap rules = StyleTokens::read(styleGuide).rules;
    QFile file(scenePath);
    if (rules.isEmpty() || !file.open(QIODevice::ReadOnly))
        return problems;
    QStringList known;
    for (auto it = rules.cbegin(); it != rules.cend(); ++it)
        known << QString("%1 %2/%3").arg(it.key()).arg(it.value().toMap()["stiffness"].toDouble())
                     .arg(it.value().toMap()["damping"].toDouble());
    static const QRegularExpression spring(R"(Motion\.spring\s*\(([^()]*(?:\([^()]*\)[^()]*)*)\))");
    static const QRegularExpression rule(R"re(Motion\.rule\s*\(\s*"([^"]*)")re");
    const QStringList lines = QString::fromUtf8(file.readAll()).split('\n');
    for (int i = 0; i < lines.size(); ++i) {
        for (auto it = spring.globalMatch(lines[i]); it.hasNext();) {
            const QStringList args = it.next().captured(1).split(',');
            double stiffness = 170, damping = 26;
            bool literal = true;
            if (args.size() >= 3)
                stiffness = args[2].trimmed().toDouble(&literal);
            if (literal && args.size() >= 4)
                damping = args[3].trimmed().toDouble(&literal);
            if (!literal)
                continue; // Worked out from something else; check can't say.
            const bool matches = std::any_of(rules.cbegin(), rules.cend(), [&](const QVariant &r) {
                return r.toMap()["stiffness"].toDouble() == stiffness && r.toMap()["damping"].toDouble() == damping;
            });
            if (!matches)
                problems << Problem{false,
                                    QString("Line %1 springs at %2/%3, which matches no motion rule in "
                                            "style-guide.md (%4); use Motion.rule(name, t, start).")
                                        .arg(i + 1).arg(stiffness).arg(damping).arg(known.join(", ")),
                                    {}, -1, {}};
        }
        for (auto it = rule.globalMatch(lines[i]); it.hasNext();) {
            const QString name = it.next().captured(1);
            if (!rules.contains(name))
                problems << Problem{true,
                                    QString("Line %1 asks for the motion rule \"%2\", which style-guide.md doesn't "
                                            "have (%3).").arg(i + 1).arg(name, known.join(", ")),
                                    {}, -1, {}};
        }
    }
    return problems;
}

QList<Problem> Measure::sync(Scene &scene) {
    QList<Problem> problems;
    const double fps = scene.fps();
    const QString format = scene.format().name;
    QList<double> change;
    for (const auto &cue : scene.cues()) {
        if (!cue.on.isEmpty()) {
            for (const auto &beat : scene.beats())
                if (beat.name == cue.on) {
                    const double frames = (cue.at - beat.at) * fps;
                    if (std::abs(frames) > 1 + 1e-6)
                        problems << Problem{true,
                                            QString("Cue \"%1\" at %2 is %3 frames %4 the beat \"%5\" it's on, at %6.")
                                                .arg(cue.name, seconds(cue.at))
                                                .arg(qRound(std::abs(frames)))
                                                .arg(frames > 0 ? "after" : "before", beat.name, seconds(beat.at)),
                                            {}, cue.at, {}};
                }
            continue;
        }
        // Without a beat, a cue belongs to the largest visible change within
        // a quarter of a second of it.
        if (change.isEmpty()) {
            Scene small;
            QString error;
            if (!small.load(scene.path(), format, &error, 0.125))
                return problems;
            change = changes(small);
        }
        const int at = int(std::lround(cue.at * fps));
        const int reach = int(std::lround(0.25 * fps));
        int peak = -1;
        for (int f = qMax(1, at - reach); f <= qMin(int(change.size()) - 1, at + reach); ++f)
            if (peak < 0 || change[f] > change[peak])
                peak = f;
        if (peak < 0 || change[peak] < 0.002) {
            problems << Problem{false,
                                QString("Nothing visible changes within 0.25 s of cue \"%1\" at %2; a sound needs "
                                        "something to belong to.").arg(cue.name, seconds(cue.at)),
                                format, cue.at, {}};
            continue;
        }
        const int off = at - peak;
        if (std::abs(off) > 1)
            problems << Problem{true,
                                QString("Cue \"%1\" at %2 is %3 frames %4 the change on screen at %5.")
                                    .arg(cue.name, seconds(cue.at))
                                    .arg(std::abs(off))
                                    .arg(off > 0 ? "after" : "before", seconds(peak / fps)),
                                format, cue.at, {}};
    }

    // Cuts against the music.
    QFile file(scene.audio() + ".beats.json");
    if (!scene.audio().isEmpty() && file.open(QIODevice::ReadOnly)) {
        const QJsonObject grid = QJsonDocument::fromJson(file.readAll()).object();
        QList<double> music;
        // The beats the trim keeps, where they play in the film.
        for (const auto &value : grid["beats"].toArray())
            if (value.toDouble() >= scene.audioTrim())
                music << scene.filmTime(value.toDouble());
        const double period = grid["tempo"].toDouble() > 0 ? 60.0 / grid["tempo"].toDouble() : 0;
        for (const auto &beat : scene.beats()) {
            if (beat.at <= 0 || music.isEmpty())
                continue;
            const double nearest = *std::min_element(music.cbegin(), music.cend(), [&](double a, double b) {
                return std::abs(a - beat.at) < std::abs(b - beat.at);
            });
            const double frames = (beat.at - nearest) * fps;
            if (std::abs(frames) > 1 + 1e-6 && (period == 0 || std::abs(beat.at - nearest) < period / 2))
                problems << Problem{false,
                                    QString("Beat \"%1\" cuts at %2, %3 frames %4 the music's beat at %5.")
                                        .arg(beat.name, seconds(beat.at))
                                        .arg(qRound(std::abs(frames)))
                                        .arg(frames > 0 ? "after" : "before", seconds(nearest)),
                                    {}, beat.at, {}};
        }
    }
    return problems;
}

QList<Problem> Measure::hook(Scene &scene) {
    QList<Problem> problems;
    const QImage first = scene.frame(0);
    // One color, give or take noise and a hairline: nothing to see.
    const QImage tiny = first.scaled(48, 48, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const QRgb corner = tiny.pixel(0, 0);
    int different = 0;
    for (int y = 0; y < tiny.height(); ++y)
        for (int x = 0; x < tiny.width(); ++x) {
            const QRgb p = tiny.pixel(x, y);
            if (std::abs(qRed(p) - qRed(corner)) + std::abs(qGreen(p) - qGreen(corner)) +
                    std::abs(qBlue(p) - qBlue(corner)) > 24)
                ++different;
        }
    if (different < 3)
        problems << Problem{false,
                            "Frame 0 shows only one color, and it's often the thumbnail. Put something on screen "
                            "by 0.2 s, unless the shot list chose an empty opening.",
                            scene.format().name, 0, {}};

    const double end = qMin(2.0, scene.duration());
    double most = 0;
    const QImage start = first.scaled(160, 160, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    for (double t = 0.25; t <= end + 1e-9; t += 0.25) {
        const QImage later = scene.frame(t).scaled(160, 160, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        double sum = 0;
        for (int y = 0; y < start.height(); ++y)
            for (int x = 0; x < start.width(); ++x) {
                const QRgb a = start.pixel(x, y), b = later.pixel(x, y);
                sum += std::abs(qRed(a) - qRed(b)) + std::abs(qGreen(a) - qGreen(b)) + std::abs(qBlue(a) - qBlue(b));
            }
        most = qMax(most, sum / (start.width() * start.height() * 3 * 255.0));
    }
    if (most < 0.004)
        problems << Problem{false,
                            "Nothing visibly changes in the first two seconds; a viewer scrolling past has no reason "
                            "to stop.",
                            scene.format().name, 0, {}};
    return problems;
}

namespace {
double difference(const QImage &a, const QImage &b) {
    if (a.isNull() || b.isNull() || a.size() != b.size())
        return 0;
    double sum = 0;
    for (int y = 0; y < a.height(); ++y) {
        const QRgb *p = reinterpret_cast<const QRgb *>(a.constScanLine(y));
        const QRgb *q = reinterpret_cast<const QRgb *>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x)
            sum += std::abs(qRed(p[x]) - qRed(q[x])) + std::abs(qGreen(p[x]) - qGreen(q[x])) +
                   std::abs(qBlue(p[x]) - qBlue(q[x]));
    }
    return sum / (double(a.width()) * a.height() * 3 * 255);
}
}

QList<double> Measure::changes(Scene &small, QList<double> *flashes) {
    QList<double> result;
    if (flashes)
        flashes->clear();
    QImage beforePrevious, previous;
    for (int i = 0; i < small.frameCount(); ++i) {
        const QImage current = small.frame(i / double(small.fps())).convertToFormat(QImage::Format_RGB32);
        result << difference(previous, current);
        if (flashes) {
            // The frame before this one, against its two neighbors.
            if (i >= 2)
                (*flashes)[i - 1] = qMax(0.0, qMin(result[i - 1], result[i]) - difference(beforePrevious, current));
            *flashes << 0;
        }
        beforePrevious = previous;
        previous = current;
    }
    return result;
}

QList<double> Measure::fastestChanges(const QList<double> &changes, int fps, int count, double gap) {
    QList<int> order(changes.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return changes[a] > changes[b]; });
    QList<double> times;
    for (int frame : order) {
        if (times.size() >= count || changes[frame] <= 0.002)
            break;
        const double t = frame / double(fps);
        if (std::none_of(times.cbegin(), times.cend(), [&](double other) { return std::abs(other - t) < gap; }))
            times << t;
    }
    std::sort(times.begin(), times.end());
    return times;
}

QList<Measure::Moment> Measure::moments(const QString &scenePath, const QString &format, int count, QString *error) {
    Scene small;
    if (!small.load(scenePath, format, error, 0.125))
        return {};
    error->clear();
    QList<double> flashes;
    const QList<double> change = changes(small, &flashes);
    QList<double> strays;
    for (double t : fastestChanges(flashes, small.fps(), 2))
        if (flashes[int(std::lround(t * small.fps()))] > 0.004)
            strays << t;
    QList<Moment> found;
    for (double t : fastestChanges(change, small.fps(), count))
        if (std::none_of(strays.cbegin(), strays.cend(), [&](double s) { return std::abs(s - t) < 0.2; }))
            found << Moment{t, false};
    for (double t : strays)
        found << Moment{t, true};
    std::sort(found.begin(), found.end(), [](const Moment &a, const Moment &b) { return a.at < b.at; });
    return found;
}
