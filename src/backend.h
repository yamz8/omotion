#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QFileSystemWatcher>
#include <QJsonObject>
#include <QObject>
#include <QPointF>
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

// Everything the window shows and does, around one open project. Every edit
// is saved straight to scene.json, and edits made to scene.json from outside
// (an editor, a script, an agent) show up live, so the file is always the
// truth and there's never unsaved work.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool hasProject READ hasProject NOTIFY projectChanged)
    Q_PROPERTY(QString projectPath READ projectPath NOTIFY projectChanged)
    Q_PROPERTY(QString projectName READ projectName NOTIFY projectChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged)
    Q_PROPERTY(int revision READ revision NOTIFY sceneChanged)
    Q_PROPERTY(QVariantMap sceneInfo READ sceneInfo NOTIFY sceneChanged)
    Q_PROPERTY(QVariantList layers READ layers NOTIFY sceneChanged)
    Q_PROPERTY(int selected READ selected WRITE select NOTIFY selectedChanged)
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

    const Scene &scene() const { return m_scene; }
    AssetCache &assets() { return m_assets; }
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
    Q_INVOKABLE void keyAll();
    Q_INVOKABLE void removeKeys();
    Q_INVOKABLE void cycleEase();
    // Gives the keyframes at the playhead (or the picked ones) a custom
    // easing curve, with handles at (x1,y1) and (x2,y2). Drags call it many
    // times between beginGesture and endGesture, for one undo step.
    Q_INVOKABLE void setCurve(double x1, double y1, double x2, double y2);
    Q_INVOKABLE void setSceneValue(const QString &name, const QVariant &value);

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
    Q_INVOKABLE void beginDrag(double x, double y);
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
    // Gives the selected layer a ready-made motion (see Project::presets).
    Q_INVOKABLE void applyPreset(const QString &preset);
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
    // The selected columns, or the selected layer's keys at the playhead.
    QList<KeyRef> targetKeys() const;

    FilePicker *m_picker = nullptr;
    QString m_directory;
    Scene m_scene;
    AssetCache m_assets;
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
    QPointF m_dragStart;
    QPointF m_dragOrigin;
    bool m_dragging = false;
    QList<KeyRef> m_keySelection;
    QList<KeyRef> m_keyDragFrom;
    Scene m_keyDragBase;
    bool m_keyDragging = false;
    QList<KeyClip> m_clipboard;
    Exporter m_exporter;
    double m_exportProgress = 0;
    QString m_exportTarget;
};
