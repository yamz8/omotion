#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QFileSystemWatcher>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <functional>
#include <QPointF>
#include <QPolygonF>
#include <QSizeF>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <QAudioOutput>
#include <QFutureWatcher>
#include <QMediaPlayer>

#include "ai.h"
#include "exporter.h"
#include "filepicker.h"
#include "scene.h"
#include "worldview.h"

// Everything the window shows and does, around one open project. Every edit
// is saved straight to scene.json, and edits made to scene.json from outside
// (an editor, a script, an agent) show up live, so the file is always the
// truth and there's never unsaved work.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool hasProject READ hasProject NOTIFY projectChanged)
    // The scene's world, for the 3D view under the canvas.
    Q_PROPERTY(WorldSource *world READ world CONSTANT)
    Q_PROPERTY(QString projectPath READ projectPath NOTIFY projectChanged)
    Q_PROPERTY(QString projectName READ projectName NOTIFY projectChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged)
    Q_PROPERTY(int revision READ revision NOTIFY sceneChanged)
    Q_PROPERTY(QVariantMap sceneInfo READ sceneInfo NOTIFY sceneChanged)
    Q_PROPERTY(QVariantList layers READ layers NOTIFY sceneChanged)
    Q_PROPERTY(int selected READ selected WRITE select NOTIFY selectedChanged)
    // Layers selected along with it (Ctrl+click): moved with it.
    Q_PROPERTY(QVariantList alsoSelected READ alsoSelected NOTIFY selectedChanged)
    Q_PROPERTY(bool cameraSelected READ cameraSelected NOTIFY selectedChanged)
    Q_PROPERTY(QVariantMap values READ values NOTIFY valuesChanged)
    Q_PROPERTY(double time READ time WRITE seek NOTIFY timeChanged)
    Q_PROPERTY(int frame READ frame NOTIFY timeChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
    Q_PROPERTY(QString tool READ tool NOTIFY toolChanged)
    Q_PROPERTY(QVariantList cutPoints READ cutPoints NOTIFY toolChanged)
    Q_PROPERTY(double feather READ feather NOTIFY toolChanged)
    Q_PROPERTY(QString fillMode READ fillMode NOTIFY toolChanged)
    Q_PROPERTY(int tolerance READ tolerance NOTIFY toolChanged)
    Q_PROPERTY(int usedTolerance READ usedTolerance NOTIFY toolChanged)
    Q_PROPERTY(double brushRadius READ brushRadius NOTIFY toolChanged)
    // The selected layer's puppet pins where they are now, on the canvas.
    Q_PROPERTY(QVariantList pinPoints READ pinPoints NOTIFY valuesChanged)
    Q_PROPERTY(QString reshaping READ reshaping NOTIFY toolChanged)
    Q_PROPERTY(QVariantList keySelection READ keySelection NOTIFY keySelectionChanged)
    Q_PROPERTY(QStringList blends READ blends CONSTANT)
    Q_PROPERTY(QString busy READ busy NOTIFY busyChanged)
    // The scene's sound: source, offset, volume, and peaks along the scene
    // for the timeline; empty without one.
    Q_PROPERTY(QVariantMap audio READ audio NOTIFY audioChanged)
    // Whether the canvas shows the selected layer's path and its ghosts.
    Q_PROPERTY(bool showMotion READ showMotion WRITE setShowMotion NOTIFY showMotionChanged)
    Q_PROPERTY(QString aiPrompt READ aiPrompt NOTIFY aiPromptChanged)
    Q_PROPERTY(bool downloading READ downloading NOTIFY downloadChanged)
    Q_PROPERTY(double downloadProgress READ downloadProgress NOTIFY downloadChanged)
    Q_PROPERTY(QString wandMode READ wandMode NOTIFY toolChanged)
    Q_PROPERTY(int aiChoice READ aiChoice NOTIFY toolChanged)
    Q_PROPERTY(int aiChoices READ aiChoices NOTIFY toolChanged)
    Q_PROPERTY(bool exporting READ exporting NOTIFY exportChanged)
    Q_PROPERTY(double exportProgress READ exportProgress NOTIFY exportChanged)
    // Looking at the world from the window's own free view rather than
    // through the shot's camera.
    Q_PROPERTY(bool freeView READ freeView NOTIFY viewChanged)
    // A piece being moved by a handle or grabbed with G: 0 freely, 1 to 3
    // along x, y or z, 4 to 6 turning about y (yaw), x (pitch) or z (roll);
    // -1 when nothing is.
    Q_PROPERTY(int handle READ handle NOTIFY viewChanged)
    // The outliner: the camera, the world and its pieces (as they were cut
    // from each other), then the layers over it, as a tree of rows.
    Q_PROPERTY(QVariantList outliner READ outliner NOTIFY sceneChanged)
    // The world's own settings (its look, as scene.json has them), empty
    // without a world.
    Q_PROPERTY(QVariantMap worldInfo READ worldInfo NOTIFY sceneChanged)
    // The selected layer's AI steps (see Steps::of), and how many steps are
    // out of date across the scene.
    Q_PROPERTY(QVariantList steps READ steps NOTIFY valuesChanged)
    Q_PROPERTY(int staleSteps READ staleSteps NOTIFY sceneChanged)
    // The graph editor: the selected layer's (or the camera's) animated
    // properties, by name.
    Q_PROPERTY(QStringList curveNames READ curveNames NOTIFY valuesChanged)
    // Saying what you want: an agent (Claude Code, or OMOTION_AGENT) works on
    // the project through scene.json and the CLI while the window follows;
    // then what it changed waits to be kept or taken back.
    Q_PROPERTY(bool agentAvailable READ agentAvailable CONSTANT)
    Q_PROPERTY(bool agentBusy READ agentBusy NOTIFY agentChanged)
    Q_PROPERTY(bool agentReview READ agentReview NOTIFY agentChanged)
    Q_PROPERTY(QString agentReply READ agentReply NOTIFY agentChanged)
    Q_PROPERTY(QStringList agentChanges READ agentChanges NOTIFY agentChanged)

public:
    explicit Backend(FilePicker *picker = nullptr, QObject *parent = nullptr);
    ~Backend() override;

    bool hasProject() const { return !m_directory.isEmpty(); }
    QString projectPath() const { return m_directory; }
    QString projectName() const;
    QString error() const { return m_error; }
    QString notice() const { return m_notice; }
    int revision() const { return m_revision; }
    QVariantMap sceneInfo() const;
    QVariantList layers() const;
    int selected() const { return m_selected; }
    QVariantList alsoSelected() const {
        QVariantList list;
        for (const int i : m_also)
            list << i;
        return list;
    }
    bool cameraSelected() const { return m_cameraSelected; }
    QVariantMap values() const;
    double time() const { return m_time; }
    int frame() const;
    bool playing() const { return m_playing; }
    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }
    QString tool() const { return m_tool; }
    QVariantList cutPoints() const;
    double feather() const { return m_feather; }
    // How the next cut's hole is filled: "AI", "smooth" or "off".
    QString fillMode() const { return m_fill == 2 ? "AI" : m_fill == 0 ? "off" : "smooth"; }
    // The smart select tolerance; below 0 while it's picked automatically.
    int tolerance() const { return m_tolerance; }
    int usedTolerance() const { return m_usedTolerance; }
    double brushRadius() const { return m_brushRadius; }
    QVariantList pinPoints() const;
    // The shape being painted, small, for the canvas to show, and where it goes.
    QImage brushPreview() const { return m_brushPreview; }
    QTransform brushPreviewToCanvas() const;
    QString reshaping() const { return m_reshaping; }
    QVariantList keySelection() const;
    QStringList blends() const { return Scene::blends(); }
    // What the AI is busy with, in words, or empty.
    QString busy() const { return m_busy; }
    bool showMotion() const { return m_showMotion; }
    QVariantMap audio() const;
    void setShowMotion(bool show) {
        if (show != m_showMotion) {
            m_showMotion = show;
            emit showMotionChanged();
        }
    }
    // An offer waiting for Enter: "download" the models, or "install" the runtime.
    QString aiPrompt() const { return m_aiPrompt; }
    bool downloading() const { return m_download.busy(); }
    double downloadProgress() const { return m_downloadProgress; }
    QString wandMode() const { return m_wandMode; }
    int aiChoice() const { return m_aiChoice; }
    int aiChoices() const { return m_aiChoices; }
    bool exporting() const { return m_exporter.busy(); }
    double exportProgress() const { return m_exportProgress; }
    bool freeView() const { return m_freeView; }
    QVariantList outliner() const;
    QVariantMap worldInfo() const;
    QVariantList steps() const;
    int staleSteps() const;
    int handle() const { return m_handle; }

    // What the world's view shows: the shot's camera at the playhead, or the
    // free view; and the size of the image it makes (the canvas, or the
    // window's area for the free view). Points "in the view" below are on
    // that image.
    World::View view() const;
    QSizeF viewSize() const;
    // The handles drawn on the selected piece of a world (or the camera, in
    // the free view), in view points: its box's corners (8, as World::box
    // has them), its pivot, the ends of its x, y and z arrows, and its
    // turning ring. Empty when there's nothing to draw.
    struct Gizmo {
        QList<QPointF> corners;
        QPointF pivot;
        QList<QPointF> arrows;
        QPolygonF ring, ringPitch, ringRoll;
        // The others selected with it: their boxes' corners.
        QList<QList<QPointF>> others;
        // In the free view, the shot's camera: its eye and the corners of
        // its frame; and a grid on the ground, a line a meter.
        QList<QPointF> camera;
        QList<QPolygonF> grid;
        // The lights, where they are and their colours.
        QList<QPair<QPointF, QColor>> lights;
    };
    Gizmo gizmo() const;
    // An animated property of what's selected, as the graph editor shows it:
    // its keys, and its own value (links left out, so keys lie on it) at
    // even steps over the scene, with the lowest and highest it gets.
    struct Curve {
        QString property;
        QList<Key> keys;
        QList<double> samples;
        double low = 0, high = 1;
    };
    QList<Curve> curves(int samples = 240) const;
    QStringList curveNames() const;
    bool agentAvailable() const { return !agentProgram().isEmpty(); }
    bool agentBusy() const { return m_agent != nullptr; }
    bool agentReview() const { return m_agentReview; }
    QString agentReply() const { return m_agentReply; }
    QStringList agentChanges() const { return m_agentChanges; }
    // The agent's program: OMOTION_AGENT, or Claude Code where it's installed.
    static QString agentProgram();

    const Scene &scene() const { return m_scene; }
    AssetCache &assets() { return m_assets; }
    WorldSource *world() { return &m_world; }
    QList<QPointF> cutPolygon() const { return m_cut; }

    // Opening.
    Q_INVOKABLE bool open(const QString &path);
    Q_INVOKABLE void openImageDialog();
    Q_INVOKABLE void openFolderDialog();
    Q_INVOKABLE void importImageDialog();
    bool importImage(const QString &path);

    // Time.
    Q_INVOKABLE void seek(double t);
    Q_INVOKABLE void togglePlay();
    Q_INVOKABLE void stepFrames(int frames);
    Q_INVOKABLE void jumpKey(int direction);
    Q_INVOKABLE void jumpTo(bool end);

    // Layers.
    Q_INVOKABLE void select(int index);
    // Adds a layer to the selection, or takes it out (Ctrl+click).
    Q_INVOKABLE void selectAlso(int index);
    // Selects the camera: edits, keyframes and dragging the canvas then move
    // the view rather than a layer.
    Q_INVOKABLE void selectCamera();
    // How much the selected layer follows the camera, 0 to 1; below 0 goes
    // back to following its parent.
    Q_INVOKABLE void setFollow(double follow);
    Q_INVOKABLE void selectNext(int direction);
    Q_INVOKABLE void selectAt(double x, double y);
    Q_INVOKABLE void toggleVisible(int index);
    Q_INVOKABLE void moveLayer(int direction);
    Q_INVOKABLE void deleteLayer();
    Q_INVOKABLE bool renameLayer(int index, const QString &name);
    // A copy just above, with the same look and animation, for glows and echoes.
    Q_INVOKABLE void duplicateLayer();
    // Adds rain, snow and the like on top (see Scene::particleKinds), and
    // changes the selected particle layer's settings.
    Q_INVOKABLE void addParticles(const QString &kind);
    Q_INVOKABLE void setParticle(const QString &setting, const QVariant &value);
    Q_PROPERTY(QStringList particleKinds READ particleKinds CONSTANT)
    QStringList particleKinds() const { return Scene::particleKinds(); }
    Q_INVOKABLE void setBlend(const QString &blend);
    Q_INVOKABLE void cycleBlend();

    // Properties and keyframes, on the selected layer at the playhead.
    Q_INVOKABLE void setProperty(const QString &property, double value);
    Q_INVOKABLE void adjust(const QString &property, double delta);
    Q_INVOKABLE void multiply(const QString &property, double factor);
    Q_INVOKABLE void keyProperty(const QString &property);
    // Drives a property from another, written as Scene::parseLink reads it
    // ("car.x × 0.8"); empty text unlinks it. False, with an error shown,
    // when the text doesn't make a link.
    Q_INVOKABLE bool setLink(const QString &property, const QString &text);
    Q_INVOKABLE void keyAll();
    Q_INVOKABLE void removeKeys();
    Q_INVOKABLE void cycleEase();
    // Gives the keyframes at the playhead (or the picked ones) a custom
    // easing curve, with handles at (x1,y1) and (x2,y2). Drags call it many
    // times between beginGesture and endGesture, for one undo step.
    Q_INVOKABLE void setCurve(double x1, double y1, double x2, double y2);
    // Gives the same keyframes an easing by name, physical ones included:
    // "spring(3)", "bounce(2)", "arc(200)". False, with an error, if unknown.
    Q_INVOKABLE bool setEase(const QString &ease);
    Q_INVOKABLE void setSceneValue(const QString &name, const QVariant &value);
    // Does one of the selected layer's steps again, or differently (an
    // action from its list); the AI's run in the background.
    Q_INVOKABLE void runStep(const QString &step, const QString &action);
    // Drawing (the "draw" tool): strokes on the selected drawing layer, at
    // the playhead, in a colour and a width (canvas pixels).
    Q_PROPERTY(QString drawColour READ drawColour WRITE setDrawColour NOTIFY toolChanged)
    QString drawColour() const { return m_drawColour.name(QColor::HexArgb); }
    void setDrawColour(const QString &colour);
    Q_INVOKABLE void startDrawing();
    Q_INVOKABLE void beginStroke(double x, double y);
    Q_INVOKABLE void strokeTo(double x, double y);
    Q_INVOKABLE void endStroke();
    Q_INVOKABLE void undoStroke();
    // Tracking: the point of the selected video layer under a canvas point
    // followed from the playhead to the end; an empty keyed to follow it.
    Q_INVOKABLE void trackAt(double x, double y);
    // The selected video layer's start, speed or loop.
    Q_INVOKABLE void setVideo(const QString &name, const QVariant &value);
    // Lights in a world: a new one in front of the camera; the selected
    // light's settings ("color", "power", "reach"); a piece's material
    // ("shine", "roughness").
    Q_INVOKABLE void addLight();
    Q_INVOKABLE void setLightValue(const QString &name, const QVariant &value);
    Q_INVOKABLE void setMaterial(const QString &name, double value);
    // The selected piece's shape's modifiers (World::setShapeModifier).
    Q_INVOKABLE void setShapeModifier(const QString &type, int amount);
    // Rigs the selected layer (see Project::rig: "ik", "lookAt", "wheel"),
    // or takes its rig away; adds an empty in the middle of the view; and
    // with the path tool's points, makes it follow them from the playhead to
    // the end (turning with the way, with turn).
    Q_INVOKABLE void rigSelected(const QString &type, const QString &other);
    Q_INVOKABLE void unrigSelected();
    Q_INVOKABLE void addEmpty();
    Q_INVOKABLE void finishPath(bool turn);
    // The empty whose cross is within reach of a canvas point, or -1.
    int emptyNear(double x, double y, double reach) const;
    // Gives a key of the selected thing's property a curve out of it, by
    // its handles (as setCurve, for just that key).
    Q_INVOKABLE void setKeyCurve(const QString &property, int index, double x1, double y1, double x2, double y2);
    // Curve modifiers and clips on the selected thing (see Project).
    Q_INVOKABLE void setModifier(const QString &property, const QString &type, double amount = 0, double speed = 2);
    Q_INVOKABLE void addClip(const QString &motion, int repeat = 1);
    Q_INVOKABLE void removeClip(int clip);
    // Rigid bodies: the selected layer as one (or a fixed one), or not; and
    // every body let fall from the playhead to the end, baked into keys.
    Q_INVOKABLE void setBody(bool body, bool fixed, double bounce);
    Q_INVOKABLE void simulate();
    // Moves a key of the selected thing's property to a new time and value
    // (between its neighbours); a drag of them is one undo step, from
    // beginGesture to endGesture.
    Q_INVOKABLE void moveKey(const QString &property, int index, double t, double v);
    // Hands words to the agent, with what's selected and the playhead;
    // stopping it puts everything back. Its changes are one undo step, kept
    // or taken back when it's done.
    Q_INVOKABLE void askAgent(const QString &request);
    Q_INVOKABLE void cancelAgent();
    Q_INVOKABLE void keepAgent();
    Q_INVOKABLE void takeBackAgent();
    // The search box's order for a query over operator names (see
    // Project::rankMatches).
    Q_INVOKABLE QVariantList rankCommands(const QString &query, const QStringList &names) const;
    // Redoes every step that's out of date, in order, in the background.
    Q_INVOKABLE void redoStale();
    // A setting of the scene's look (see Project::lookSettings).
    Q_INVOKABLE void setLook(const QString &name, double value);
    // A world setting: hazeColor, hazeDensity, grain, shutter (degrees) or
    // dust (a colour).
    Q_INVOKABLE void setWorldValue(const QString &name, const QVariant &value);

    // Selecting keyframe columns on the timeline, retiming them by dragging,
    // and copying them, onto the same layer or another.
    Q_INVOKABLE void selectKey(int layer, double t, bool add);
    Q_INVOKABLE void clearKeySelection();
    Q_INVOKABLE void beginKeyDrag();
    Q_INVOKABLE void dragKeys(double delta);
    Q_INVOKABLE void endKeyDrag();
    Q_INVOKABLE void copyKeys();
    Q_INVOKABLE void pasteKeys();

    // Dragging a layer around the canvas, as one undo step.
    // In a world, along means Shift: nearer and farther rather than across.
    Q_INVOKABLE void beginDrag(double x, double y, bool along = false);
    Q_INVOKABLE void dragTo(double x, double y);
    Q_INVOKABLE void endDrag();
    Q_INVOKABLE void beginGesture();
    Q_INVOKABLE void endGesture();

    // Cutting a layer into pieces, and moving pivots.
    Q_INVOKABLE void setTool(const QString &tool);
    Q_INVOKABLE void addCutPoint(double x, double y);
    Q_INVOKABLE void removeCutPoint();
    // Editing the shape: the point within radius of x, y; a new point on the
    // edge within radius (its index, or -1); moving and removing points.
    Q_INVOKABLE int cutPointNear(double x, double y, double radius) const;
    Q_INVOKABLE int insertCutPoint(double x, double y, double radius);
    Q_INVOKABLE void moveCutPoint(int index, double x, double y);
    Q_INVOKABLE void removeCutPointAt(int index);
    // Steps the soft edge for the next cut: 0, 1, 3, 8, 20 canvas pixels.
    Q_INVOKABLE void cycleFeather();
    // How the hole a cut leaves is filled: by the AI (when it's set up),
    // smoothly from the colours around it, or not at all; cycles.
    Q_INVOKABLE void toggleFill();
    // Opens the selected piece's shape in the cut tool to change it.
    Q_INVOKABLE void startReshape();
    // Puppet pins (the "pin" tool): add one on the selected layer, find the
    // one near a canvas point, drag one to bend the layer (keyframed when
    // its pin is animated), or take one away.
    Q_INVOKABLE int addPinAt(double x, double y);
    Q_INVOKABLE int pinNear(double x, double y, double radius) const;
    Q_INVOKABLE void beginPinDrag(int pin, double x, double y);
    Q_INVOKABLE void pinDragTo(double x, double y);
    Q_INVOKABLE void endPinDrag();
    Q_INVOKABLE void removePin(int pin);
    // The brush: paints the selected piece's shape (drag to add, with erase
    // to take away), applied with finishBrush. Works on outlines and masks.
    Q_INVOKABLE void startBrush();
    Q_INVOKABLE void beginBrush(double x, double y, bool erase);
    Q_INVOKABLE void brushTo(double x, double y);
    Q_INVOKABLE void endBrush();
    Q_INVOKABLE void adjustBrush(double factor);
    Q_INVOKABLE void finishBrush();
    // In the wand tool, AI select leaves out what's under a point.
    Q_INVOKABLE void excludeAt(double x, double y);
    // Switches the wand between AI select and colour select.
    Q_INVOKABLE void toggleWandMode();
    // Splits the selected layer (or the bottom one) into count layers by
    // depth, with the AI, in the background.
    Q_INVOKABLE void splitByDepth(int count);
    // Animates a depth-split layer as a slow parallax camera move.
    Q_INVOKABLE void animateParallax();
    // The world: the picture read in 3D (the AI, in the background), a
    // selected piece given a shape, the world sized by it, and driven.
    Q_INVOKABLE void makeWorld(double horizon = -1);
    Q_INVOKABLE void shapeSelected();
    Q_INVOKABLE void setRealSize(double meters);
    Q_INVOKABLE void driveSelected(double fromX, double fromAhead, double toX, double toAhead, double kmh, bool face);
    // Gives the selected layer a ready-made motion (see Project::presets).
    Q_INVOKABLE void applyPreset(const QString &preset);
    // Saves the selected layer's animation (or the picked keyframes) as a
    // motion to use in any project, and puts one on at the playhead.
    Q_INVOKABLE void saveMotion(const QString &name);
    Q_INVOKABLE void applyMotion(const QString &name);
    Q_INVOKABLE QStringList savedMotions() const;
    Q_INVOKABLE void acceptAiPrompt();
    Q_INVOKABLE void dismissAiPrompt();
    Q_INVOKABLE void cancelDownload();
    // Sound: pick a file to play along (and go into video exports), set where
    // in it the scene starts and how loud, or take it away.
    Q_INVOKABLE void importAudioDialog();
    Q_INVOKABLE bool setAudioFile(const QString &path);
    Q_INVOKABLE void setAudioValue(const QString &name, double value);
    Q_INVOKABLE void removeAudio();
    // Moves images nothing uses any more (not even undo) to the trash.
    Q_INVOKABLE void cleanUp();
    // Waits for any background work (the AI, big cuts) to finish; for tests.
    void waitForJobs();
    // Smart select (the "wand" tool): the area of similar colour around a
    // canvas point on the selected layer becomes the cut shape; add joins it
    // to what's selected. The tolerance is picked for each click until it's
    // changed by hand; changing it redoes the clicks. cycleTolerance goes
    // back to automatic after the steps.
    Q_INVOKABLE void smartSelect(double x, double y, bool add);
    Q_INVOKABLE void adjustTolerance(int delta);
    Q_INVOKABLE void cycleTolerance();
    Q_INVOKABLE void finishCut(bool punch);
    Q_INVOKABLE void setPivotAt(double x, double y);

    // The free view: in and out of it, orbiting about what it looks at,
    // panning, coming closer, and looking at the selected piece. The window
    // tells the size of its area.
    Q_INVOKABLE void toggleFreeView();
    Q_INVOKABLE void orbitView(double dx, double dy);
    Q_INVOKABLE void panView(double dx, double dy);
    Q_INVOKABLE void zoomView(double factor);
    Q_INVOKABLE void frameSelected();
    Q_INVOKABLE void setViewportSize(double width, double height);
    // Picks what's under a view point: layers drawn over a world first (in
    // the shot), then the world's pieces in 3D.
    Q_INVOKABLE void pickAt(double x, double y, bool add = false);
    // The handle within reach of a view point (as handle above), or -1.
    Q_INVOKABLE int handleAt(double x, double y, double reach) const;
    // Moving the selected piece of a world (or the camera) by a handle, or
    // grabbed (handle 0): one undo step, or none when cancelled. axis
    // changes what a grab keeps to while it's under way.
    Q_INVOKABLE void beginHandle(int handle, double x, double y);
    // With snap, moves go in steps of 10 cm and turns of 15°.
    Q_INVOKABLE void handleTo(double x, double y, bool snap = false);
    Q_INVOKABLE void setHandleAxis(int axis);
    Q_INVOKABLE void endHandle(bool keep);

    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();

    Q_INVOKABLE void exportAs(const QString &format, double scale = 1);
    Q_INVOKABLE void cancelExport();
    Q_INVOKABLE void openRenders();
    Q_INVOKABLE void dismissError();
    // Writes any pending save now; called before quitting.
    Q_INVOKABLE void flush();

signals:
    void projectChanged();
    void errorChanged();
    void noticeChanged();
    void sceneChanged();
    void selectedChanged();
    void valuesChanged();
    void timeChanged();
    void playingChanged();
    void historyChanged();
    void toolChanged();
    void exportChanged();
    void keySelectionChanged();
    void busyChanged();
    void audioChanged();
    void showMotionChanged();
    void aiPromptChanged();
    void downloadChanged();
    void viewChanged();
    void agentChanged();

private:
    void setError(const QString &error);
    void setNotice(const QString &notice);
    // Records the scene before an edit, unless a gesture already has.
    void checkpoint();
    // After any change to the scene: repaint, refresh panels, save soon.
    void changed(bool save = true);
    void save();
    void reloadFromDisk();
    void rearmWatcher();
    void tick();
    void reportMissing();
    QString selectedName() const;
    // What edits act on: the selected layer, or the camera.
    int target() const { return m_cameraSelected ? Scene::CameraIndex : m_selected; }
    const Layer *targetLayer() const { return m_scene.at(target()); }
    // The camera zooms where a layer scales.
    QString propertyFor(const QString &property) const {
        return m_cameraSelected && property == "scale" ? QString("zoom") : property;
    }
    void restoreSelection(const QString &name);
    void setKeySelection(const QList<KeyRef> &refs);
    void pasteClip(QList<KeyClip> clip);
    // The selected columns, or the selected layer's keys at the playhead.
    QList<KeyRef> targetKeys() const;

    FilePicker *m_picker = nullptr;
    QString m_directory;
    Scene m_scene;
    AssetCache m_assets;
    WorldSource m_world;
    QList<QJsonObject> m_undo;
    QList<QJsonObject> m_redo;
    bool m_inGesture = false;
    int m_selected = -1;
    bool m_cameraSelected = false;
    double m_time = 0;
    bool m_playing = false;
    QElapsedTimer m_clock;
    QTimer m_playTimer;
    QTimer m_saveTimer;
    QTimer m_reloadTimer;
    QTimer m_noticeTimer;
    QFileSystemWatcher m_watcher;
    QByteArray m_written;
    bool m_diskBroken = false;
    QString m_error;
    QString m_notice;
    int m_revision = 0;
    QString m_tool = "move";
    QList<QPointF> m_cut;
    QString m_reshaping;
    double m_feather = 1;
    int m_fill = -1;  // 0 off, 1 smooth, 2 AI; -1 until a tool picks the default.
    int defaultFill();
    int m_tolerance = -1;
    int m_usedTolerance = 0;
    QList<QPointF> m_seeds;
    QList<QImage> m_seedLevels;
    void reselect();

    int m_pinDragging = -1;
    QPointF m_pinDragStart;
    QPointF m_pinOffsetStart;
    QString m_brushPiece;
    QImage m_brushMask;     // Alpha8, the source image's size.
    QImage m_brushPreview;  // ARGB, small, white where the shape is.
    double m_brushPreviewScale = 1;
    QTransform m_brushToCanvas;
    double m_brushRadius = 24;  // In canvas pixels.
    bool m_brushErase = false;
    bool m_brushing = false;
    QPointF m_brushLast;
    void paintBrush(const QPointF &from, const QPointF &to);

    // Slow work (the AI, cutting big pictures) runs one job at a time off the
    // window's thread, with its own image cache, so the window stays responsive.
    bool needAi();
    void runAiSelect();
    void setBusy(const QString &busy);
    QString m_busy;
    bool m_showMotion = false;
    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audioOutput = nullptr;
    QString m_playerSource;
    qint64 m_pendingSeek = -1;  // A seek waiting for a new file to load.
    QList<double> m_peaks;
    QString m_peaksFor;
    QFutureWatcher<QList<double>> m_peaksJob;
    // Puts the player on the scene's sound, at the playhead, playing or not.
    void syncAudio(bool reseek);
    QString m_aiPrompt;
    QString m_wandMode = "ai";
    QList<QPointF> m_aiTake;
    QList<QPointF> m_aiLeave;
    int m_aiChoice = 0;
    int m_aiChoices = 0;
    bool m_aiAgain = false;
    int m_aiGeneration = 0;
    QJsonObject m_gestureStart;
    AssetCache m_workAssets;
    QFutureWatcher<void> m_job;
    ModelDownload m_download;
    double m_downloadProgress = 0;
    void worldJob(const QString &busy, const QString &done, std::function<bool(Scene &, AssetCache &, QString *)> work);
    QPointF m_dragStart;
    QPointF m_dragOrigin;
    // A drag in a world: what's dragged's world properties when it started,
    // how far away it is, and the lens in canvas pixels.
    bool m_worldDrag = false;
    bool m_dragAlong = false;
    QHash<QString, double> m_worldOrigin;
    double m_dragDistance = 1;
    double m_dragFocal = 1;
    bool m_dragging = false;
    QList<KeyRef> m_keySelection;
    QList<KeyRef> m_keyDragFrom;
    Scene m_keyDragBase;
    bool m_keyDragging = false;
    QList<KeyClip> m_clipboard;
    QProcess *m_agent = nullptr;
    Scene m_agentBefore;
    int m_agentUndoMark = 0;
    bool m_agentReview = false;
    bool m_agentCancelled = false;
    QString m_agentReply;
    QStringList m_agentChanges;
    void agentFinished();
    bool m_freeView = false;
    // The free view orbits a point: from yaw and pitch (degrees) at a
    // distance (meters).
    QVector3D m_orbitTarget;
    double m_orbitYaw = 0, m_orbitPitch = 0, m_orbitDistance = 10;
    QSizeF m_viewportSize{1280, 720};
    void syncView();
    int m_handle = -1;
    QPointF m_handleStart;
    QHash<int, QHash<QString, double>> m_handleOrigins;
    QVector3D m_handlePivot;
    // What a handle moves: the selected world piece, or the camera; and with
    // it, the other world pieces selected.
    int handleTarget() const;
    QList<int> handleTargets() const;
    QList<int> m_also;
    QColor m_drawColour = QColor("#ffffff");
    bool m_stroking = false;
    QHash<int, QPointF> m_dragOrigins;
    Exporter m_exporter;
    double m_exportProgress = 0;
    QString m_exportTarget;
};
