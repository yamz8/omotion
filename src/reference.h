#pragma once

#include <QColor>
#include <QList>
#include <QString>
#include <QStringList>

// What a reference film or set of frames shows, measured: the palette, the
// time between scene changes, and how much moves. The agent completes the
// rest of the style guide from it, and copies none of the reference itself.
namespace Reference {
struct Swatch {
    QColor color;
    double share = 0; // Of all the pixels, 0 to 1.
};
struct Analysis {
    QList<Swatch> palette;   // Largest share first.
    QList<double> cuts;      // Seconds, for a film.
    double shotLength = 0;   // Average seconds between scene changes; 0 for stills.
    double energy = 0;       // Average change between frames a tenth of a second apart, 0 to 1, cuts aside.
    double duration = 0;
    int frames = 0;
};

// A video, or one or more images.
bool analyze(const QStringList &inputs, Analysis *analysis, QString *error);

// The measured lines, as REFERENCE ANALYSIS in style-guide.md writes them.
QStringList guideLines(const Analysis &analysis);

// Replaces the Palette, Pacing, and Motion lines under REFERENCE ANALYSIS in
// the guide with measured ones, leaving everything else as the agent wrote it.
bool writeInto(const QString &guide, const Analysis &analysis, QString *error);

// How far apart two colors look: CIE76 ΔE in Lab, where under about 2 is
// invisible and 5 is a close match.
double deltaE(const QColor &a, const QColor &b);
}
