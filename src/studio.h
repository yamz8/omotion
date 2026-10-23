#pragma once

#include <QList>
#include <QPoint>
#include <QSize>
#include <QString>
#include <QStringList>

// The omotion release; the agent skill a project carries is stamped with it.
inline constexpr char omotionVersion[] = "0.1.0";

// A studio is one film's folder: the brief, style guide, shot list, assets,
// gates, a scene, and the agent skill that knows how to work in it, so the
// folder alone carries everything a later session needs.
namespace Studio {
// Creates the studio in an empty or new folder; never writes into a folder
// that already has files.
bool create(const QString &folder, QString *error, QStringList *created = nullptr);

// The studio a path is in: the nearest folder at or above it holding gates.md.
QString rootOf(const QString &path);

// Writes the bundled skill into the studio, and AGENTS.md when it has none.
bool installSkill(const QString &root, QString *error, QStringList *written = nullptr);

// The bundled skill as it is written into a project, stamped with the release
// and a hash of its text.
QByteArray stampedSkill();

// Empty when the studio's skill is the one this omotion would write, or the
// person took it over by removing its stamp; otherwise why it's out of date.
QString skillProblem(const QString &root);

// One line of gates.md: "- [x] 2 Style guide and shot list: approved by ...".
struct Gate {
    int number = 0;
    QString title;
    bool passed = false;
    QString status; // What follows the title, such as who approved it or what it waits for.
    bool waitingForPerson() const;
};
QList<Gate> readGates(const QString &root);

// A note the person pinned to a moment of the film, kept in notes.md. The
// agent treats an open one as a defect: it fixes it with evidence and ticks
// it, or replies and leaves it open.
struct Note {
    QString id; // "001"
    bool done = false;
    double at = 0;
    QString format;
    QString beat;
    QPoint point;   // In the format's frame, in pixels.
    QString target; // The text under the pointer, if any.
    QString evidence;
    QString text;
    QStringList answers; // The agent's "fixed:" and "reply:" lines.
};
QList<Note> readNotes(const QString &root);
// Appends the note with the next free id, which it returns, and sets its
// evidence to reviews/notes/<id>.png, so the id and the frame can't disagree.
// The rest of the file is left as it is, so an agent's edits are kept.
QString addNote(const QString &root, Note note, QString *error);
// A note's time as notes.md writes it: precise to the frame at any rate up to
// 120 fps, without trailing zeros past two decimals ("12.60", "0.033").
QString noteTime(double at);
// The note as one line, for pasting into an agent's terminal.
QString noteReference(const Note &note);

// A beat as shotlist.md writes it: "03.0-06.0s   BECOMES A SLIDE", then
// indented "enters:", "exits:", and "why:" lines, which may wrap.
struct ShotListBeat {
    double start = 0;
    double end = 0;
    QString name; // The heading up to any ": " subtitle.
    QString enters;
    QString exits;
    QString why;
    QString onScreen; // Every word on screen in the beat, as the shot list has it.
};
// Empty while shotlist.md is missing or still the template.
QList<ShotListBeat> readShotList(const QString &root);

// What a gate's evidence still lacks, read only from the studio's files, so a
// later session can see where the film stands and what to do next. formats
// are the scene's, whose renders gate 6 needs.
QStringList missingEvidence(const QString &root, int gate, const QStringList &formats);

// The files assets.md lists for the assets the studio has, relative to
// assets/ ("screens/editor.png"); a name may hold * or ?, and one ending in /
// is a folder.
QStringList listedAssetFiles(const QString &root);
// Whether a path relative to assets/ is one assets.md lists.
bool isListedAsset(const QString &root, const QString &relative);
// Listed files that aren't in assets/, each as assets.md writes it.
QStringList listedAssetsNotFound(const QString &root);

// Captures a web page as a PNG with the installed Chromium, at size, and in
// a studio lists it in assets.md with where it came from, for the person to
// approve. A page behind a login can't be captured this way; the person
// takes that screenshot.
bool capture(const QString &url, const QString &output, const QSize &size, const QString &root, QString *error);

// Assets that assets.md lists as missing.
QStringList missingAssets(const QString &root);
// activity.md's entries, newest first.
QStringList readActivity(const QString &root);
}
