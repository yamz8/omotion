#pragma once

// A project is a folder: scene.json beside the images it uses. These are the
// operations that touch that folder, kept apart from the UI so the command
// line renderer and the tests share them.

#include <QByteArray>
#include <QJsonObject>
#include <QPolygonF>
#include <QString>
#include <QVector3D>

#include "scene.h"

namespace Project {

QString sceneFile(const QString &directory);
bool isImage(const QString &path);

// Turns whatever was opened into a project folder and returns it. An image
// gets a new "<name>.omotion" folder beside it holding a copy, so the original
// is never touched; opening it again reopens that folder. A folder of loose
// images gets a scene.json with one layer per image.
QString open(const QString &path, QString *error);

Scene load(const QString &directory, QString *error, QByteArray *bytes = nullptr);
QByteArray serialize(const Scene &scene);
// Writes beside scene.json and renames over it, so a crash or a reader
// never sees half a file.
bool save(const QString &directory, const Scene &scene, QString *error, QByteArray *bytes = nullptr);

// Cuts the part of a layer inside a canvas-space polygon out into its own
// image and layer, placed exactly where it was and parented to the layer it
// came from. feather softens the edge, in canvas pixels. With punch, the
// source layer gets a hole where the piece was, worked out so the two still
// add up to the original; with fill, the hole is painted in from the colours
// around it, so moving the piece away doesn't leave a gap; with aiFill, the
// AI paints it (LaMa), inventing texture, where it's set up. The piece remembers its shape (in "cut", in the
// source's pixels), so it can be reshaped later. New images are always new
// files, so undo can go back to the old ones. The piece is named after its
// source unless given a name. Returns the new layer's index, or -1.
int cutOut(Scene &scene, AssetCache &assets, int layer, const QPolygonF &polygon, double t,
           bool punch, QString *error, const QString &name = {}, double feather = 0, bool fill = true,
           bool aiFill = false);

// Gives a piece a new shape and edge, redrawing it and its source's holes
// from the source's original image. Its animation and pivot are kept. fill
// sets how its hole is filled: 0 not, 1 smoothly, 2 by the AI, -1 as it was.
bool reshapeCut(Scene &scene, AssetCache &assets, int piece, const QPolygonF &polygon, double t,
                double feather, QString *error, int fill = -1);
bool isCut(const Scene &scene, int layer);
// A piece's shape in canvas pixels at time t, and its edge softness.
QPolygonF cutOutline(const Scene &scene, AssetCache &assets, int piece, double t, double *feather = nullptr,
                     int *fill = nullptr);
// A piece's shape as a mask over the image it was cut from (Alpha8, the
// whole image's size, 255 inside), whether it was cut by an outline or a
// mask, with the transform from that image's pixels to the canvas at t.
QImage cutMask(const Scene &scene, AssetCache &assets, int piece, double t, QTransform *toCanvas);
// Gives a piece a new shape, as a mask like cutMask's, saved into the
// project; redraws it and its source's holes. Its animation stays.
bool reshapeCutMask(Scene &scene, AssetCache &assets, int piece, const QImage &mask, QString *error);

// Sound: copies an audio file into the project and makes it the scene's
// track ("audio" in scene.json: source, offset into it in seconds, volume).
bool setAudio(Scene &scene, const QString &directory, const QString &path, QString *error);
bool isAudio(const QString &path);
// How loud the track is along the scene, as count peaks from 0 to 1, read
// with ffmpeg; empty when it can't be.
QList<double> audioPeaks(const QString &file, double offset, double duration, int count);

// Smart select on a layer: the area of colour within tolerance around each
// canvas point, joined, as a cut shape in canvas pixels. A tolerance below 0
// picks one for each point (see Select::autoTolerance), and *used says the
// last one. regions says how many separate areas that made; only the biggest
// is outlined. levels, when given, keeps each point's search to reuse while
// the points stay the same, so trying tolerances is instant.
QPolygonF smartOutline(const Scene &scene, AssetCache &assets, int layer, const QList<QPointF> &points, double t,
                       int tolerance, int *regions = nullptr, int *used = nullptr, QList<QImage> *levels = nullptr);

// AI select on a layer: the thing under the take points, less what's under
// the leave-out ones, as a cut shape in canvas pixels. choices says how many
// guesses the AI made; choice picks one (0 is its best).
QPolygonF aiOutline(const Scene &scene, AssetCache &assets, int layer, const QList<QPointF> &take,
                    const QList<QPointF> &leave, double t, int choice, int *choices, QString *error);

// Depth for a layer's image, from the AI.
QImage depthOf(const Scene &scene, AssetCache &assets, int layer, QString *error);

// Splits a layer into count layers by depth, from a depth map of its image
// (16-bit grey, larger nearer, any size). The source stays as the farthest
// layer; the nearer ones are cut out of it as pieces, nearest on top, each
// with the places of the ones in front of it filled in behind, so nothing
// opens up when they move apart. Returns the new layers' indexes.
QList<int> splitByDepth(Scene &scene, AssetCache &assets, int layer, const QImage &depth, int count, double feather,
                        QString *error);

// Animates a depth-split layer as a slow camera move: each depth layer
// follows the camera by its depth (the background 30%, the nearest fully),
// and the camera drifts strength canvas pixels sideways and pushes in, as a
// loop over the scene's duration. A scene shorter than 10 seconds is made 10,
// stretching any animation it already has to match.
bool animateParallax(Scene &scene, AssetCache &assets, int layer, double strength, QString *error);

// Checks one particle setting and gives its JSON value, or false with why.
bool particleSetting(const QString &name, const QVariant &value, QJsonValue *json, QString *error);

// Adds a particle layer of a kind (see Scene::particleKinds) on top, with
// its default settings and any given over them. Returns its index, or -1.
int addParticles(Scene &scene, const QString &kind, const QJsonObject &settings, QString *error);

// Saved motions: keyframes kept under a name, to use in any project, in
// ~/.local/share/omotion/motions as JSON (times relative to the first key).
QString motionDirectory();
// Keeps motions somewhere else; for tests.
void setMotionDirectory(const QString &directory);
QStringList savedMotions();
bool saveMotion(const QString &name, const QList<KeyClip> &keys, QString *error);
QList<KeyClip> loadMotion(const QString &name, QString *error);

// The motion presets, by name: sway, float, pulse, bounce, spin, fade.
const QStringList &presets();
// Gives a layer a ready-made motion, replacing that property's animation.
// All but fade fit whole cycles (spin, whole turns) into the scene, so they
// loop with it; fade starts at t. amount scales the motion (1 is the default).
bool applyPreset(Scene &scene, AssetCache &assets, int layer, const QString &preset, double t, double amount,
                 QString *error);

// Image files in the project's assets folder that none of the given scenes
// (say, the scene and its undo history) mentions anywhere.
QStringList unusedAssets(const QString &directory, const QList<QJsonObject> &scenes);
// Moves those files to the trash; returns how many bytes that freed, or -1.
qint64 cleanAssets(const QString &directory, const QList<QJsonObject> &scenes, int *count, QString *error);

// Keeps pieces pointing at their source when it's renamed.
void renameCutSource(Scene &scene, const QString &from, const QString &to);

// Moves a layer's pivot to a canvas point without moving the layer.
void setPivot(Scene &scene, AssetCache &assets, int layer, const QPointF &point, double t);

// An empty: a layer with nothing to draw, a point to aim at or move by (a
// target for a rig), at a point in its parent's pixels. Its index.
int addEmpty(Scene &scene, const QString &name, const QString &parent, const QPointF &at);

// Rigs a layer, as Blender's constraints do (see Scene::constrained):
// "ik" makes it the upper bone of a two-bone chain whose lower bone is
// other (its child), reaching for a new empty put where the lower's tip is
// now; "lookAt" turns it toward other, as it faces now; "wheel" makes it
// roll as other (the body it's on) moves, turning about its middle; "path"
// moves it along points (canvas pixels, in options' "points") from
// options' "start" to "end" seconds, turning with the way when "turn".
// Returns false with why when it can't.
bool rig(Scene &scene, AssetCache &assets, int layer, const QString &type, const QString &other, QString *error,
         const QJsonObject &options = {});
// Takes a layer's rig away (constraints and a wheel's link); the empties it
// aimed at stay.
bool unrig(Scene &scene, int layer);

// Curve modifiers on a layer's (or the camera's) property, as Blender's:
// "cycle" repeats its keys over and over, "noise" adds a smooth wobble of
// amount (its units) at speed (wobbles a second); "none" takes them away.
bool setModifier(Scene &scene, int layer, const QString &property, const QString &type, double amount, double speed,
                 QString *error);
// Lays a saved motion on a layer as a clip, as Blender's NLA does: from
// start (seconds), repeated, at a speed, scaled by amount; it adds to the
// layer's own animation without changing its keys. The motion is copied
// into the scene, so it goes where the project goes.
bool addClip(Scene &scene, int layer, const QString &motion, double start, int repeat, double speed, double amount,
             QString *error);
bool removeClip(Scene &scene, int layer, int clip);

// A setting of the scene's look (see applyLook, and focus and aperture for
// depth of field): checked and set; 0 for a setting that's neutral when
// the default, which takes it out.
bool setLook(Scene &scene, const QString &name, double value, QString *error);
const QStringList &lookSettings();

// Drawing, as Grease Pencil does: a new drawing layer on top (its index);
// a stroke (canvas points) added to its frame at time t, made there if it
// has none; and the last stroke of the frame shown at t taken away.
int addDrawing(Scene &scene, const QString &name);
bool addStroke(Scene &scene, AssetCache &assets, int layer, double t, const QList<QPointF> &canvas, const QColor &colour,
               double width, QString *error);
bool removeStroke(Scene &scene, int layer, double t);

// Lights and materials in a world (see World::Light): a light layer at a
// place in meters (right, up, ahead of the picture's camera); a light's
// colour, power or reach; a piece's shine or roughness (0 to 1).
int addLight(Scene &scene, const QString &name, const QVector3D &at, const QJsonObject &settings);
bool setLight(Scene &scene, int layer, const QString &name, const QVariant &value, QString *error);
bool setMaterial(Scene &scene, int layer, const QString &name, double value, QString *error);

// What changed between two versions of a scene, in a few words a line:
// layers added and taken away, and for each layer (and the camera, the
// world and the scene) which properties and keys changed.
QStringList describeChanges(const Scene &before, const Scene &after);

// How well a typed query matches each of some names (operators, for the
// search box), best first: the indexes of those that match at all. Every
// word typed must start a word of the name, or appear in it; starts of
// words and the name's start count most.
QList<int> rankMatches(const QString &query, const QStringList &names);

// A video clip as a layer that plays: its frames, at the scene's rate,
// written into the project by ffmpeg (as many as max at most), fitted to
// the canvas, from the start of the scene, looping. Its index, or -1.
bool isVideo(const QString &path);
int importVideo(Scene &scene, const QString &directory, const QString &path, QString *error, int max = 3000);

// A video layer's "start" (seconds into the scene), "speed" or "loop".
bool setVideo(Scene &scene, int layer, const QString &name, const QVariant &value, QString *error);

// Motion tracking, as Blender's: follows the patch of a video layer's
// picture around a canvas point from one time to another, frame by frame,
// and keys a layer (onto, or a new empty) to follow it, x and y in its
// parent's pixels. The layer it keyed, or -1.
int track(Scene &scene, AssetCache &assets, int video, const QPointF &point, double from, double to, QString *error,
          const QString &onto = {}, QList<QPointF> *path = nullptr);

// Copies an image into the project's assets and adds it as a layer on top.
int importImage(Scene &scene, const QString &directory, const QString &path, QString *error);

}
