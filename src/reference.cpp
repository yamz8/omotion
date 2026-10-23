#include "reference.h"

#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>

namespace {
constexpr int sampleWidth = 192, sampleHeight = 108, sampleRate = 10;

struct Lab {
    double l = 0, a = 0, b = 0;
};

double linear(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
double gamma(double c) { return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1 / 2.4) - 0.055; }

Lab toLab(double r, double g, double b) {
    const double R = linear(r), G = linear(g), B = linear(b);
    double x = (0.4124 * R + 0.3576 * G + 0.1805 * B) / 0.95047;
    double y = 0.2126 * R + 0.7152 * G + 0.0722 * B;
    double z = (0.0193 * R + 0.1192 * G + 0.9505 * B) / 1.08883;
    auto f = [](double t) { return t > 0.008856 ? std::cbrt(t) : 7.787 * t + 16.0 / 116; };
    x = f(x), y = f(y), z = f(z);
    return {116 * y - 16, 500 * (x - y), 200 * (y - z)};
}

QColor fromLab(const Lab &lab) {
    const double y = (lab.l + 16) / 116, x = lab.a / 500 + y, z = y - lab.b / 200;
    auto f = [](double t) { return t * t * t > 0.008856 ? t * t * t : (t - 16.0 / 116) / 7.787; };
    const double X = f(x) * 0.95047, Y = f(y), Z = f(z) * 1.08883;
    const double r = 3.2406 * X - 1.5372 * Y - 0.4986 * Z;
    const double g = -0.9689 * X + 1.8758 * Y + 0.0415 * Z;
    const double b = 0.0557 * X - 0.2040 * Y + 1.0570 * Z;
    auto channel = [](double c) { return qBound(0, int(std::lround(gamma(qBound(0.0, c, 1.0)) * 255)), 255); };
    return QColor(channel(r), channel(g), channel(b));
}

double distance(const Lab &p, const Lab &q) {
    return std::sqrt((p.l - q.l) * (p.l - q.l) + (p.a - q.a) * (p.a - q.a) + (p.b - q.b) * (p.b - q.b));
}

bool isImage(const QString &path) {
    return QImageReader::supportedImageFormats().contains(QFileInfo(path).suffix().toLower().toLatin1());
}

// Frames as small RGB images, from a video through ffmpeg or from images.
bool frames(const QStringList &inputs, QList<QImage> *out, bool *film, QString *error) {
    *film = inputs.size() == 1 && !isImage(inputs.first());
    if (!*film) {
        for (const auto &path : inputs) {
            QImage image(path);
            if (image.isNull()) {
                *error = "Could not read " + path;
                return false;
            }
            *out << image.convertToFormat(QImage::Format_RGB888)
                        .scaled(sampleWidth, sampleHeight, Qt::IgnoreAspectRatio, Qt::FastTransformation);
        }
        return true;
    }
    const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
    if (ffmpeg.isEmpty()) {
        *error = "ffmpeg is not installed; install it with: omarchy-pkg-add ffmpeg";
        return false;
    }
    QProcess process;
    process.start(ffmpeg, {"-hide_banner", "-loglevel", "error", "-i", inputs.first(), "-vf",
                           QString("fps=%1,scale=%2:%3:flags=neighbor").arg(sampleRate).arg(sampleWidth).arg(sampleHeight), "-f",
                           "rawvideo", "-pix_fmt", "rgb24", "-"});
    if (!process.waitForFinished(-1) || process.exitCode() != 0) {
        *error = "Could not read " + inputs.first() + ": " + QString::fromUtf8(process.readAllStandardError()).trimmed();
        return false;
    }
    const QByteArray raw = process.readAllStandardOutput();
    const int size = sampleWidth * sampleHeight * 3;
    for (qsizetype at = 0; at + size <= raw.size(); at += size) {
        QImage image(reinterpret_cast<const uchar *>(raw.constData() + at), sampleWidth, sampleHeight,
                     sampleWidth * 3, QImage::Format_RGB888);
        *out << image.copy();
    }
    if (out->isEmpty()) {
        *error = inputs.first() + " has no frames.";
        return false;
    }
    return true;
}

double change(const QImage &a, const QImage &b) {
    double sum = 0;
    for (int y = 0; y < a.height(); ++y) {
        const uchar *p = a.constScanLine(y), *q = b.constScanLine(y);
        for (int x = 0; x < a.width() * 3; ++x)
            sum += std::abs(int(p[x]) - int(q[x]));
    }
    return sum / (a.width() * a.height() * 3 * 255.0);
}
}

double Reference::deltaE(const QColor &a, const QColor &b) {
    return distance(toLab(a.redF(), a.greenF(), a.blueF()), toLab(b.redF(), b.greenF(), b.blueF()));
}

bool Reference::analyze(const QStringList &inputs, Analysis *analysis, QString *error) {
    *analysis = {};
    QList<QImage> images;
    bool film = false;
    if (inputs.isEmpty()) {
        *error = "Name a reference film, or one or more images.";
        return false;
    }
    if (!frames(inputs, &images, &film, error))
        return false;
    analysis->frames = images.size();
    analysis->duration = film ? images.size() / double(sampleRate) : 0;

    // The palette: k-means in Lab over every sampled pixel, started from
    // colors spread across the lightness range so it comes out the same
    // every time.
    QList<Lab> pixels;
    // About 400,000 pixels say as much about a palette as millions.
    const int stride = qMax(1, int(images.size() * sampleWidth * sampleHeight / 400000));
    for (int f = 0; f < images.size(); f += stride)
        for (const QImage &image : {images[f]})
        for (int y = 0; y < image.height(); ++y) {
            const uchar *row = image.constScanLine(y);
            for (int x = 0; x < image.width(); ++x)
                pixels << toLab(row[3 * x] / 255.0, row[3 * x + 1] / 255.0, row[3 * x + 2] / 255.0);
        }
    constexpr int k = 8;
    QList<Lab> sorted = pixels;
    std::sort(sorted.begin(), sorted.end(), [](const Lab &a, const Lab &b) { return a.l < b.l; });
    QList<Lab> centers;
    for (int i = 0; i < k; ++i)
        centers << sorted[qMin(sorted.size() - 1, qsizetype((i + 0.5) * sorted.size() / k))];
    // Then the most distant pixels, so a small accent isn't lost in a gray.
    for (int round = 0; round < 3; ++round) {
        double farthest = 0;
        Lab far;
        for (int i = 0; i < pixels.size(); i += 7) {
            double nearest = 1e9;
            for (const Lab &c : centers)
                nearest = std::min(nearest, distance(pixels[i], c));
            if (nearest > farthest) {
                farthest = nearest;
                far = pixels[i];
            }
        }
        if (farthest > 10)
            centers << far;
    }
    QList<int> owner(pixels.size(), 0);
    for (int iteration = 0; iteration < 20; ++iteration) {
        QList<Lab> sums(centers.size());
        QList<int> counts(centers.size(), 0);
        for (int i = 0; i < pixels.size(); ++i) {
            int best = 0;
            double bestDistance = 1e18;
            for (int c = 0; c < centers.size(); ++c) {
                const double d = distance(pixels[i], centers[c]);
                if (d < bestDistance) {
                    bestDistance = d;
                    best = c;
                }
            }
            owner[i] = best;
            sums[best].l += pixels[i].l, sums[best].a += pixels[i].a, sums[best].b += pixels[i].b;
            ++counts[best];
        }
        for (int c = 0; c < centers.size(); ++c)
            if (counts[c] > 0)
                centers[c] = {sums[c].l / counts[c], sums[c].a / counts[c], sums[c].b / counts[c]};
    }
    struct Cluster {
        Lab center;
        int count;
    };
    QList<Cluster> clusters;
    QList<int> counts(centers.size(), 0);
    for (int o : owner)
        ++counts[o];
    for (int c = 0; c < centers.size(); ++c)
        if (counts[c] > 0)
            clusters << Cluster{centers[c], counts[c]};
    // Colors closer than a glance can tell apart are one.
    std::sort(clusters.begin(), clusters.end(), [](const Cluster &a, const Cluster &b) { return a.count > b.count; });
    for (int i = 0; i < clusters.size(); ++i)
        for (int j = clusters.size() - 1; j > i; --j)
            if (distance(clusters[i].center, clusters[j].center) < 8) {
                const double total = clusters[i].count + clusters[j].count;
                Lab &c = clusters[i].center;
                const Lab &d = clusters[j].center;
                c = {(c.l * clusters[i].count + d.l * clusters[j].count) / total,
                     (c.a * clusters[i].count + d.a * clusters[j].count) / total,
                     (c.b * clusters[i].count + d.b * clusters[j].count) / total};
                clusters[i].count += clusters[j].count;
                clusters.removeAt(j);
            }
    std::sort(clusters.begin(), clusters.end(), [](const Cluster &a, const Cluster &b) { return a.count > b.count; });
    // A color of the film is 2% of it, or less when it's an accent: unlike
    // everything else that's kept.
    QList<Lab> kept;
    for (const Cluster &cluster : clusters) {
        const double share = cluster.count / double(pixels.size());
        double nearest = 1e9;
        for (const Lab &k : kept)
            nearest = std::min(nearest, distance(k, cluster.center));
        if (share >= 0.02 || (share >= 0.003 && nearest > 25)) {
            analysis->palette << Swatch{fromLab(cluster.center), share};
            kept << cluster.center;
        }
    }

    if (!film)
        return true;
    // Pacing: a scene change is where most of what's on screen is replaced
    // within half a second. Over one background (motion graphics), that's
    // the share of the foreground that changed, which a crossfade between
    // beats reaches and a word arriving doesn't; over footage, it's how much
    // the colors change, which a cut does and a pan doesn't.
    constexpr int span = 5;
    QList<double> changes{0};
    for (int i = 1; i < images.size(); ++i)
        changes << change(images[i - 1], images[i]);
    const QColor ground = analysis->palette.isEmpty() ? QColor(Qt::black) : analysis->palette.first().color;
    const bool graphics = !analysis->palette.isEmpty() && analysis->palette.first().share > 0.6;
    auto isGround = [&](const uchar *p) {
        return std::abs(p[0] - ground.red()) + std::abs(p[1] - ground.green()) + std::abs(p[2] - ground.blue()) < 40;
    };
    auto histogram = [](const QImage &image) {
        QList<double> bins(512, 0.0);
        for (int y = 0; y < image.height(); ++y) {
            const uchar *row = image.constScanLine(y);
            for (int x = 0; x < image.width(); ++x)
                bins[(row[3 * x] >> 5) * 64 + (row[3 * x + 1] >> 5) * 8 + (row[3 * x + 2] >> 5)] += 1;
        }
        for (double &b : bins)
            b /= image.width() * image.height();
        return bins;
    };
    QList<double> replaced(images.size(), 0.0);
    QList<QList<double>> histograms;
    if (!graphics)
        for (const QImage &image : images)
            histograms << histogram(image);
    for (int i = span; i < images.size(); ++i) {
        if (graphics) {
            int foreground = 0, differ = 0;
            for (int y = 0; y < sampleHeight; ++y) {
                const uchar *p = images[i - span].constScanLine(y), *q = images[i].constScanLine(y);
                for (int x = 0; x < sampleWidth; ++x) {
                    const uchar *a = p + 3 * x, *b = q + 3 * x;
                    if (isGround(a) && isGround(b))
                        continue;
                    ++foreground;
                    differ += std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]) > 60;
                }
            }
            // Below a sliver of foreground, nothing is on screen to replace.
            replaced[i] = foreground > sampleWidth * sampleHeight / 200 ? differ / double(foreground) : 0;
        } else {
            double sum = 0;
            for (int b = 0; b < 512; ++b)
                sum += std::abs(histograms[i][b] - histograms[i - span][b]);
            replaced[i] = sum / 2;
        }
    }
    const double threshold = graphics ? 0.6 : 0.35;
    QList<bool> inChange(images.size(), false);
    for (int i = span; i < images.size(); ++i) {
        const bool peak = replaced[i] >= replaced[i - 1] && (i + 1 >= images.size() || replaced[i] > replaced[i + 1]);
        if (!peak || replaced[i] < threshold)
            continue;
        // The change happened where the single biggest step within the span was.
        int at = i;
        for (int j = i - span + 1; j <= i; ++j)
            if (changes[j] > changes[at])
                at = j;
        const double time = at / double(sampleRate);
        if (analysis->cuts.isEmpty() || time - analysis->cuts.last() >= 1.0) {
            analysis->cuts << time;
            for (int j = i - span + 1; j <= i; ++j)
                inChange[j] = true;
        }
    }
    double moving = 0;
    int still = 0;
    for (int i = 1; i < changes.size(); ++i)
        if (!inChange[i]) {
            moving += changes[i];
            ++still;
        }
    // What's taken away as the film ends isn't a new shot.
    while (!analysis->cuts.isEmpty() && analysis->duration - analysis->cuts.last() < 0.3)
        analysis->cuts.removeLast();
    // The typical shot: the median gap between changes, which the film's
    // first and last shots, cut short by its ends, don't skew.
    if (analysis->cuts.size() >= 3) {
        QList<double> gaps;
        for (int i = 1; i < analysis->cuts.size(); ++i)
            gaps << analysis->cuts[i] - analysis->cuts[i - 1];
        std::sort(gaps.begin(), gaps.end());
        analysis->shotLength = gaps.size() % 2 ? gaps[gaps.size() / 2]
                                               : (gaps[gaps.size() / 2 - 1] + gaps[gaps.size() / 2]) / 2;
    } else {
        analysis->shotLength = analysis->duration / (analysis->cuts.size() + 1);
    }
    analysis->energy = still > 0 ? moving / still : 0;
    return true;
}

QStringList Reference::guideLines(const Analysis &analysis) {
    QStringList colors;
    for (const auto &swatch : analysis.palette)
        colors << QString("%1 %2%").arg(swatch.color.name()).arg(qRound(swatch.share * 100));
    QStringList lines;
    lines << "Palette (measured): " + colors.join(", ");
    if (analysis.duration > 0) {
        lines << QString("Pacing (measured): a scene change every %1 s on average, %2 changes in %3 s")
                     .arg(analysis.shotLength, 0, 'f', 1)
                     .arg(analysis.cuts.size())
                     .arg(analysis.duration, 0, 'f', 1);
        const double percent = analysis.energy * 100;
        lines << QString("Motion (measured): %1, %2% of the frame changing per tenth of a second between cuts")
                     .arg(percent < 0.5 ? "nearly still" : percent < 2 ? "calm" : percent < 6 ? "lively" : "restless")
                     .arg(percent, 0, 'f', 1);
    }
    return lines;
}

bool Reference::writeInto(const QString &guide, const Analysis &analysis, QString *error) {
    QFile file(guide);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = "Could not read " + guide;
        return false;
    }
    QStringList lines = QString::fromUtf8(file.readAll()).split('\n');
    file.close();
    const QStringList measured = guideLines(analysis);
    int section = -1;
    for (int i = 0; i < lines.size(); ++i)
        if (lines[i].trimmed() == "REFERENCE ANALYSIS")
            section = i;
    if (section < 0) {
        lines << "" << "REFERENCE ANALYSIS";
        section = lines.size() - 1;
    }
    // Replace each measured line, or the template's line of the same kind,
    // in place; anything new goes at the end of the section.
    int end = section + 1;
    while (end < lines.size() && !lines[end].trimmed().isEmpty())
        ++end;
    for (const QString &line : measured) {
        const QString kind = line.section(' ', 0, 0);
        bool placed = false;
        for (int i = section + 1; i < end; ++i)
            if (lines[i].startsWith(kind + " (measured):") || lines[i].startsWith(kind + ":")) {
                lines[i] = line;
                placed = true;
                break;
            }
        if (!placed)
            lines.insert(end++, line);
    }
    QSaveFile out(guide);
    if (!out.open(QIODevice::WriteOnly) || out.write(lines.join('\n').toUtf8()) < 0 || !out.commit()) {
        *error = guide + ": " + out.errorString();
        return false;
    }
    return true;
}
