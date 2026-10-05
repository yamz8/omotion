#include "select.h"

#include <QLineF>
#include <algorithm>
#include <cmath>
#include <vector>

namespace {

// Eight neighbours, clockwise from east (y grows downwards).
const int DX[8] = {1, 1, 0, -1, -1, -1, 0, 1};
const int DY[8] = {0, 1, 1, 1, 0, -1, -1, -1};

// Labels the 4-connected regions of a mask; returns each pixel's label (0 for
// none) and the size of each label.
std::vector<int> label(const QImage &mask, std::vector<int> *sizes) {
    const int w = mask.width(), h = mask.height();
    std::vector<int> labels(size_t(w) * h, 0);
    sizes->assign(1, 0);
    std::vector<int> stack;
    for (int start = 0; start < w * h; ++start) {
        if (labels[start] || mask.constScanLine(start / w)[start % w] <= 127)
            continue;
        const int id = int(sizes->size());
        sizes->push_back(0);
        stack.push_back(start);
        labels[start] = id;
        while (!stack.empty()) {
            const int i = stack.back();
            stack.pop_back();
            ++(*sizes)[id];
            const int x = i % w, y = i / w;
            const int next[4][2] = {{x + 1, y}, {x - 1, y}, {x, y + 1}, {x, y - 1}};
            for (const auto &n : next) {
                if (n[0] < 0 || n[1] < 0 || n[0] >= w || n[1] >= h)
                    continue;
                const int j = n[1] * w + n[0];
                if (!labels[j] && mask.constScanLine(n[1])[n[0]] > 127) {
                    labels[j] = id;
                    stack.push_back(j);
                }
            }
        }
    }
    return labels;
}

// Douglas–Peucker on an open run of points, keeping the ends.
void simplify(const std::vector<QPointF> &points, int first, int last, double tolerance, std::vector<bool> &keep) {
    if (last <= first + 1)
        return;
    const QLineF chord(points[first], points[last]);
    double worst = -1;
    int at = -1;
    for (int i = first + 1; i < last; ++i) {
        const QPointF p = points[i];
        double d;
        if (chord.length() < 1e-9) {
            d = QLineF(p, chord.p1()).length();
        } else {
            const QPointF v = chord.p2() - chord.p1();
            d = std::abs(v.x() * (chord.p1().y() - p.y()) - v.y() * (chord.p1().x() - p.x())) / chord.length();
        }
        if (d > worst) {
            worst = d;
            at = i;
        }
    }
    if (worst > tolerance) {
        keep[at] = true;
        simplify(points, first, at, tolerance, keep);
        simplify(points, at, last, tolerance, keep);
    }
}

}

namespace Select {

QImage joinLevels(const QImage &source, const QPoint &seed) {
    const QImage image = source.convertToFormat(QImage::Format_ARGB32);
    const int w = image.width(), h = image.height();
    QImage levels(w, h, QImage::Format_Alpha8);
    levels.fill(255);
    if (!image.rect().contains(seed))
        return levels;
    const QRgb origin = image.pixel(seed);
    if (qAlpha(origin) < 16)
        return levels;

    // Colours are compared with the clicked one, not with their neighbours,
    // so a slow gradient doesn't carry the selection across the picture.
    auto distance = [&](QRgb c) {
        if (qAlpha(c) < 16)
            return 255;
        const double dr = qRed(c) - qRed(origin), dg = qGreen(c) - qGreen(origin), db = qBlue(c) - qBlue(origin);
        return std::min(254, int(std::lround(std::sqrt((dr * dr + dg * dg + db * db) / 3))));
    };
    // A bucket queue in order of level: a pixel's first level is its lowest,
    // because everything lower has been taken out already.
    std::vector<std::vector<int>> buckets(255);
    std::vector<uchar> seen(size_t(w) * h, 0);
    buckets[0].push_back(seed.y() * w + seed.x());
    seen[seed.y() * w + seed.x()] = 1;
    for (int level = 0; level < 255; ++level) {
        std::vector<int> &bucket = buckets[level];
        for (size_t k = 0; k < bucket.size(); ++k) {
            const int i = bucket[k];
            const int x = i % w, y = i / w;
            levels.scanLine(y)[x] = uchar(level);
            const int next[4][2] = {{x + 1, y}, {x - 1, y}, {x, y + 1}, {x, y - 1}};
            for (const auto &n : next) {
                if (n[0] < 0 || n[1] < 0 || n[0] >= w || n[1] >= h)
                    continue;
                const int j = n[1] * w + n[0];
                if (seen[j])
                    continue;
                const int d = distance(reinterpret_cast<const QRgb *>(image.constScanLine(n[1]))[n[0]]);
                if (d >= 255)
                    continue;
                seen[j] = 1;
                buckets[std::max(level, d)].push_back(j);
            }
        }
        std::vector<int>().swap(bucket);
    }
    return levels;
}

QImage regionAt(const QImage &levels, int tolerance) {
    const int w = levels.width(), h = levels.height();
    QImage mask(w, h, QImage::Format_Alpha8);
    for (int y = 0; y < h; ++y) {
        const uchar *from = levels.constScanLine(y);
        uchar *to = mask.scanLine(y);
        for (int x = 0; x < w; ++x)
            to[x] = from[x] <= tolerance ? 255 : 0;
    }
    if (Select::regionCount(mask) == 0)
        return mask;
    std::vector<int> stack;

    // Fill holes: whatever unselected can't be reached from the border is
    // enclosed by the selection, like a pupil inside an eye. A selection that
    // reaches the border is likely a background, though, and the things
    // inside it are objects, not holes; there only specks get filled.
    bool touchesBorder = false;
    for (int x = 0; x < w && !touchesBorder; ++x)
        touchesBorder = mask.constScanLine(0)[x] || mask.constScanLine(h - 1)[x];
    for (int y = 0; y < h && !touchesBorder; ++y)
        touchesBorder = mask.constScanLine(y)[0] || mask.constScanLine(y)[w - 1];
    std::vector<uchar> outside(size_t(w) * h, 0);
    stack.clear();
    auto seedOutside = [&](int x, int y) {
        const int i = y * w + x;
        if (!outside[i] && !mask.constScanLine(y)[x]) {
            outside[i] = 1;
            stack.push_back(i);
        }
    };
    for (int x = 0; x < w; ++x) {
        seedOutside(x, 0);
        seedOutside(x, h - 1);
    }
    for (int y = 0; y < h; ++y) {
        seedOutside(0, y);
        seedOutside(w - 1, y);
    }
    while (!stack.empty()) {
        const int i = stack.back();
        stack.pop_back();
        const int x = i % w, y = i / w;
        const int next[4][2] = {{x + 1, y}, {x - 1, y}, {x, y + 1}, {x, y - 1}};
        for (const auto &n : next)
            if (n[0] >= 0 && n[1] >= 0 && n[0] < w && n[1] < h)
                seedOutside(n[0], n[1]);
    }
    const int speck = std::max(4, w * h / 2000);
    for (int start = 0; start < w * h; ++start) {
        if (outside[start] || mask.constScanLine(start / w)[start % w])
            continue;
        // Gather this enclosed area, then fill it if it's allowed to be.
        std::vector<int> area{start};
        outside[start] = 2;
        for (size_t k = 0; k < area.size(); ++k) {
            const int x = area[k] % w, y = area[k] / w;
            const int next[4][2] = {{x + 1, y}, {x - 1, y}, {x, y + 1}, {x, y - 1}};
            for (const auto &n : next) {
                if (n[0] < 0 || n[1] < 0 || n[0] >= w || n[1] >= h)
                    continue;
                const int j = n[1] * w + n[0];
                if (!outside[j] && !mask.constScanLine(n[1])[n[0]]) {
                    outside[j] = 2;
                    area.push_back(j);
                }
            }
        }
        if (!touchesBorder || int(area.size()) <= speck)
            for (const int i : area)
                mask.scanLine(i / w)[i % w] = 255;
    }
    return mask;
}

QImage region(const QImage &image, const QPoint &seed, int tolerance) {
    return regionAt(joinLevels(image, seed), tolerance);
}

int autoTolerance(const QImage &levels) {
    // Clicked on a thing, the selection grows to its size and holds there
    // over a range of tolerances, then bursts out into the background, at
    // about the tolerance where it first reaches the edge of the image. So:
    // just under that. Reaching the edge almost at once means the click was
    // on a background; never reaching it means a piece surrounded by
    // transparency, which can take a generous tolerance.
    int border = 255;
    const int w = levels.width(), h = levels.height();
    for (int x = 0; x < w; ++x)
        border = std::min({border, int(levels.constScanLine(0)[x]), int(levels.constScanLine(h - 1)[x])});
    for (int y = 0; y < h; ++y)
        border = std::min({border, int(levels.constScanLine(y)[0]), int(levels.constScanLine(y)[w - 1])});
    if (border >= 255)
        return 32;
    if (border < 6)
        return 12;
    return border - 2;
}

void grow(QImage &mask, int radius) {
    if (radius < 1)
        return;
    // A square grown one pixel at a time, both ways, is quick and close enough.
    for (int step = 0; step < radius; ++step) {
        const QImage before = mask.copy();
        const int w = mask.width(), h = mask.height();
        for (int y = 0; y < h; ++y) {
            uchar *line = mask.scanLine(y);
            for (int x = 0; x < w; ++x) {
                if (line[x])
                    continue;
                for (int dy = -1; dy <= 1 && !line[x]; ++dy) {
                    const int ny = y + dy;
                    if (ny < 0 || ny >= h)
                        continue;
                    const uchar *other = before.constScanLine(ny);
                    if ((x > 0 && other[x - 1]) || other[x] || (x + 1 < w && other[x + 1]))
                        line[x] = 255;
                }
            }
        }
    }
}

void unite(QImage &a, const QImage &b) {
    if (a.size() != b.size()) {
        a = b;
        return;
    }
    for (int y = 0; y < a.height(); ++y) {
        uchar *line = a.scanLine(y);
        const uchar *other = b.constScanLine(y);
        for (int x = 0; x < a.width(); ++x)
            line[x] = std::max(line[x], other[x]);
    }
}

int regionCount(const QImage &mask) {
    std::vector<int> sizes;
    label(mask, &sizes);
    return int(sizes.size()) - 1;
}

QPolygonF outline(const QImage &mask, double tolerance) {
    std::vector<int> sizes;
    const std::vector<int> labels = label(mask, &sizes);
    if (sizes.size() < 2)
        return {};
    const int biggest = int(std::max_element(sizes.begin() + 1, sizes.end()) - sizes.begin());
    const int w = mask.width();
    auto in = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < w && y < mask.height() && labels[size_t(y) * w + x] == biggest;
    };

    // The first pixel of the region in reading order; its west is empty.
    int start = int(std::find(labels.begin(), labels.end(), biggest) - labels.begin());
    const QPoint first(start % w, start / w);

    // Moore-neighbour tracing: walk clockwise around each boundary pixel from
    // the empty neighbour we came from, to the next pixel of the region.
    std::vector<QPointF> points;
    QPoint p = first;
    int from = 4;  // West.
    const size_t limit = size_t(mask.width()) * mask.height() * 4 + 8;
    do {
        points.push_back(QPointF(p) + QPointF(.5, .5));
        int found = -1;
        for (int k = 1; k <= 8; ++k) {
            const int d = (from + k) % 8;
            if (in(p.x() + DX[d], p.y() + DY[d])) {
                found = d;
                break;
            }
        }
        if (found < 0)
            break;  // A single pixel.
        p += QPoint(DX[found], DY[found]);
        // The neighbour checked just before is empty; look from there next,
        // as seen from the new pixel.
        const int before = (found + 7) % 8;
        const QPoint back = p - QPoint(DX[found], DY[found]) + QPoint(DX[before], DY[before]);
        const QPoint delta = back - p;
        for (int d = 0; d < 8; ++d)
            if (DX[d] == delta.x() && DY[d] == delta.y())
                from = d;
    } while (p != first && points.size() < limit);

    if (points.size() < 3)
        return {};
    // Simplify the closed loop as two halves split at the point farthest from the start.
    int far = 0;
    for (int i = 1; i < int(points.size()); ++i)
        if (QLineF(points[i], points[0]).length() > QLineF(points[far], points[0]).length())
            far = i;
    points.push_back(points.front());
    std::vector<bool> keep(points.size(), false);
    keep[0] = keep[far] = keep.back() = true;
    simplify(points, 0, far, tolerance, keep);
    simplify(points, far, int(points.size()) - 1, tolerance, keep);
    QPolygonF polygon;
    for (size_t i = 0; i + 1 < points.size(); ++i)
        if (keep[i])
            polygon.append(points[i]);
    return polygon.size() >= 3 ? polygon : QPolygonF();
}

}
