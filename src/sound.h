#pragma once

#include <QList>
#include <QString>
#include <QStringList>

// Sound on the film's timeline: cue sounds made from numbers, so the same
// scene always sounds the same, and what omotion can measure about a track.
namespace Sound {
inline constexpr int rate = 48000;

// The sounds a cue can ask for: "click", "tick", "hit", and "whoosh", whose
// swell peaks on the cue.
QStringList names();

struct Cue {
    double at = 0;
    QString sound;
};
// Writes a mono 16-bit WAV of the cues' sounds over duration seconds.
bool writeCues(const QList<Cue> &cues, double duration, const QString &path, QString *error);

// The track as mono samples at sampleRate, decoded by ffmpeg.
QList<float> decode(const QString &path, int sampleRate, QString *error);

// A track's tempo and beats, from the onsets in its loudness: the times, in
// seconds into the track, where a listener would tap.
struct Grid {
    double tempo = 0; // Beats per minute.
    QList<double> beats;
};
Grid beats(const QList<float> &samples, int sampleRate);

// The track's loudest moment in each of count equal slices, from 0 to 1, for
// drawing its waveform.
QList<float> peaks(const QList<float> &samples, int count);
}
