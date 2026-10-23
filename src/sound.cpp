#include "sound.h"

#include <QFile>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <numeric>

namespace {
constexpr double pi = 3.14159265358979323846;

// A repeatable noise source, so a cue sounds the same on every render.
struct Noise {
    quint32 state;
    explicit Noise(quint32 seed) : state(seed * 2654435761u + 1) {}
    double next() {
        state = state * 1664525u + 1013904223u;
        return (state >> 8) / double(1 << 24) * 2 - 1;
    }
};

// Adds one sound into the mix at sample start; a whoosh begins before it so
// that its swell peaks there.
void add(QList<double> &mix, const QString &sound, double at, quint32 seed) {
    const int start = int(std::lround(at * Sound::rate));
    Noise noise(seed);
    auto put = [&](int i, double value) {
        if (i >= 0 && i < mix.size())
            mix[i] += value;
    };
    if (sound == "click") {
        for (int i = 0; i < Sound::rate / 40; ++i) {
            const double t = i / double(Sound::rate);
            put(start + i, 0.5 * noise.next() * std::exp(-t / 0.0008) + 0.35 * std::sin(2 * pi * 3000 * t) * std::exp(-t / 0.004));
        }
    } else if (sound == "tick") {
        for (int i = 0; i < Sound::rate / 30; ++i) {
            const double t = i / double(Sound::rate);
            put(start + i, 0.4 * std::sin(2 * pi * 1800 * t) * std::exp(-t / 0.006));
        }
    } else if (sound == "hit") {
        // A kick: a sine falling from 120 to 45 Hz, with a short transient.
        double phase = 0;
        for (int i = 0; i < Sound::rate / 2; ++i) {
            const double t = i / double(Sound::rate);
            const double frequency = 45 + 75 * std::exp(-t / 0.04);
            phase += 2 * pi * frequency / Sound::rate;
            put(start + i, 0.85 * std::sin(phase) * std::exp(-t / 0.18) + 0.3 * noise.next() * std::exp(-t / 0.002));
        }
    } else if (sound == "whoosh") {
        // Noise through a sweeping low-pass, swelling for 0.35 s into the cue.
        const int rise = int(0.35 * Sound::rate), fall = int(0.12 * Sound::rate);
        double low = 0;
        for (int i = -rise; i < fall; ++i) {
            const double p = i < 0 ? 1 + i / double(rise) : 1 - i / double(fall);
            const double cutoff = 300 + 5000 * p * p;
            const double a = 1 - std::exp(-2 * pi * cutoff / Sound::rate);
            low += a * (noise.next() - low);
            put(start + i, 0.6 * low * p * p);
        }
    }
}

void writeLe16(QByteArray &out, quint16 v) { out.append(char(v & 0xff)).append(char(v >> 8)); }
void writeLe32(QByteArray &out, quint32 v) {
    for (int i = 0; i < 4; ++i)
        out.append(char((v >> (8 * i)) & 0xff));
}
}

QStringList Sound::names() { return {"click", "tick", "hit", "whoosh"}; }

bool Sound::writeCues(const QList<Cue> &cues, double duration, const QString &path, QString *error) {
    QList<double> mix(qMax(1, int(std::ceil(duration * rate))), 0.0);
    for (int i = 0; i < cues.size(); ++i)
        add(mix, cues[i].sound, cues[i].at, quint32(i + 1));
    QByteArray data;
    data.reserve(mix.size() * 2);
    for (double v : mix) {
        // A soft limit, so cues that land together don't clip.
        const qint16 sample = qint16(std::lround(std::tanh(v) * 32000));
        writeLe16(data, quint16(sample));
    }
    QByteArray wav("RIFF");
    writeLe32(wav, 36 + data.size());
    wav += "WAVEfmt ";
    writeLe32(wav, 16);
    writeLe16(wav, 1); // PCM
    writeLe16(wav, 1); // mono
    writeLe32(wav, rate);
    writeLe32(wav, rate * 2);
    writeLe16(wav, 2);
    writeLe16(wav, 16);
    wav += "data";
    writeLe32(wav, data.size());
    wav += data;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(wav) < 0 || !file.commit()) {
        *error = path + ": " + file.errorString();
        return false;
    }
    return true;
}

QList<float> Sound::decode(const QString &path, int sampleRate, QString *error) {
    const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
    if (ffmpeg.isEmpty()) {
        *error = "ffmpeg is not installed; install it with: omarchy-pkg-add ffmpeg";
        return {};
    }
    QProcess process;
    process.start(ffmpeg, {"-hide_banner", "-loglevel", "error", "-i", path, "-ac", "1", "-ar",
                           QString::number(sampleRate), "-f", "f32le", "-"});
    if (!process.waitForFinished(-1) || process.exitCode() != 0) {
        *error = "Could not read " + path + ": " + QString::fromUtf8(process.readAllStandardError()).trimmed();
        return {};
    }
    const QByteArray raw = process.readAllStandardOutput();
    QList<float> samples(raw.size() / 4);
    for (int i = 0; i < samples.size(); ++i)
        samples[i] = qFromLittleEndian<float>(raw.constData() + i * 4);
    return samples;
}

Sound::Grid Sound::beats(const QList<float> &samples, int sampleRate) {
    Grid grid;
    // The onset envelope: how much louder each 10 ms gets than the last, in
    // the log of its energy, which is where a listener hears a beat start.
    const int hop = sampleRate / 100, window = hop * 2;
    QList<double> energy;
    for (int start = 0; start + window <= samples.size(); start += hop) {
        double sum = 0;
        for (int i = start; i < start + window; ++i)
            sum += double(samples[i]) * samples[i];
        energy << std::log(1e-9 + sum / window);
    }
    if (energy.size() < 200)
        return grid; // Under two seconds says nothing about a tempo.
    QList<double> onset(energy.size(), 0.0);
    for (int i = 1; i < energy.size(); ++i)
        onset[i] = qMax(0.0, energy[i] - energy[i - 1]);
    const double mean = std::accumulate(onset.cbegin(), onset.cend(), 0.0) / onset.size();
    for (double &o : onset)
        o = qMax(0.0, o - mean);

    // The tempo: the lag, between 60 and 180 beats a minute, at which the
    // envelope best matches itself, preferring the middle of that range.
    const double hopSeconds = hop / double(sampleRate);
    int bestLag = 0;
    double best = -1;
    for (int lag = int(60.0 / 180 / hopSeconds); lag <= int(60.0 / 60 / hopSeconds); ++lag) {
        double sum = 0;
        for (int i = lag; i < onset.size(); ++i)
            sum += onset[i] * onset[i - lag];
        const double bpm = 60.0 / (lag * hopSeconds);
        const double weight = std::exp(-0.5 * std::pow(std::log2(bpm / 120.0) / 0.9, 2));
        if (sum * weight > best) {
            best = sum * weight;
            bestLag = lag;
        }
    }
    if (bestLag == 0 || best <= 0)
        return grid;
    // Refine the period to a fraction of a hop with a parabola through the
    // neighboring lags, so a long track doesn't drift off its beats.
    auto correlation = [&](int lag) {
        double sum = 0;
        for (int i = lag; i < onset.size(); ++i)
            sum += onset[i] * onset[i - lag];
        return sum;
    };
    const double a = correlation(bestLag - 1), b = correlation(bestLag), c = correlation(bestLag + 1);
    const double shift = (a - 2 * b + c) != 0 ? 0.5 * (a - c) / (a - 2 * b + c) : 0;
    const double period = bestLag + qBound(-0.5, shift, 0.5);
    grid.tempo = 60.0 / (period * hopSeconds);

    // The phase: where a grid at that period collects the most onset.
    double bestPhase = 0, most = -1;
    for (int phase = 0; phase < bestLag; ++phase) {
        double sum = 0;
        for (double at = phase; at < onset.size(); at += period)
            sum += onset[int(at)];
        if (sum > most) {
            most = sum;
            bestPhase = phase;
        }
    }
    // Each beat placed at the strongest onset within a tenth of a beat of
    // the grid, where the music's own timing puts it.
    const int reach = qMax(1, int(period / 10));
    for (double at = bestPhase; at < onset.size(); at += period) {
        int peak = int(std::lround(at));
        for (int i = qMax(0, peak - reach); i <= qMin(int(onset.size()) - 1, int(std::lround(at)) + reach); ++i)
            if (onset[i] > onset[peak])
                peak = i;
        // The window that grew loudest first holds the hit in its second
        // half: 1.5 hops past its start, on average.
        grid.beats << qMax(0.0, (peak + 1.5) * hopSeconds);
    }
    return grid;
}

QList<float> Sound::peaks(const QList<float> &samples, int count) {
    QList<float> result(qMax(0, count), 0.0f);
    if (samples.isEmpty() || count <= 0)
        return result;
    float loudest = 0;
    for (int i = 0; i < count; ++i) {
        const qsizetype from = samples.size() * i / count, to = samples.size() * (i + 1) / count;
        for (qsizetype j = from; j < to; ++j)
            result[i] = qMax(result[i], std::abs(samples[j]));
        loudest = qMax(loudest, result[i]);
    }
    if (loudest > 0)
        for (float &p : result)
            p /= loudest;
    return result;
}
