#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QProcess>
#include <QSize>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

class FilePicker;
class QQmlEngine;

// The window's state: which scene is open, reloading it when its files change,
// and exports, which run as "omotion render" in a child process so the preview
// stays responsive and the export renders exactly what the CLI would.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString scenePath READ scenePath NOTIFY sceneChanged)
    Q_PROPERTY(QString sceneName READ sceneName NOTIFY sceneChanged)
    Q_PROPERTY(QUrl sceneUrl READ sceneUrl NOTIFY sceneChanged)
    Q_PROPERTY(int revision READ revision NOTIFY sceneChanged)
    Q_PROPERTY(QString warning READ warning NOTIFY warningChanged)
    Q_PROPERTY(bool exporting READ exporting NOTIFY exportChanged)
    Q_PROPERTY(double exportProgress READ exportProgress NOTIFY exportChanged)
    Q_PROPERTY(QString exportFormat READ exportFormat NOTIFY exportChanged)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)
    Q_PROPERTY(bool messageIsError READ messageIsError NOTIFY messageChanged)
    Q_PROPERTY(QStringList outputs READ outputs NOTIFY messageChanged)
    Q_PROPERTY(QStringList formatNames READ formatNames CONSTANT)
    // The studio the scene belongs to, read from its files as the agent works.
    Q_PROPERTY(QString studioRoot READ studioRoot NOTIFY studioChanged)
    Q_PROPERTY(QString studioName READ studioName NOTIFY studioChanged)
    // One signal each, sent only when that list really changed, so a line the
    // agent adds to activity.md doesn't rebuild the notes the person is reading.
    Q_PROPERTY(QVariantList gates READ gates NOTIFY gatesChanged)
    Q_PROPERTY(QVariantList assets READ assets NOTIFY assetsChanged)
    Q_PROPERTY(QStringList missingAssets READ missingAssets NOTIFY assetsChanged)
    Q_PROPERTY(QVariantList evidence READ evidence NOTIFY evidenceChanged)
    Q_PROPERTY(QStringList activity READ activity NOTIFY activityChanged)
    Q_PROPERTY(QVariantList notes READ notes NOTIFY notesChanged)

public:
    // renderer is the program that runs "render"; this binary by default.
    explicit Backend(FilePicker *picker, const QString &renderer = {}, QObject *parent = nullptr);
    ~Backend() override;

    QString scenePath() const { return m_path; }
    QString sceneName() const;
    QUrl sceneUrl() const;
    int revision() const { return m_revision; }
    QString warning() const { return m_warning; }
    bool exporting() const { return m_export.state() != QProcess::NotRunning; }
    double exportProgress() const { return m_progress; }
    QString exportFormat() const { return m_exportFormat; }
    QString message() const { return m_message; }
    bool messageIsError() const { return m_messageIsError; }
    QStringList outputs() const { return m_outputs; }
    QStringList formatNames() const;
    QString studioRoot() const { return m_root; }
    QString studioName() const;
    QVariantList gates() const { return m_gates; }
    QVariantList assets() const { return m_assets; }
    QStringList missingAssets() const { return m_missing; }
    QVariantList evidence() const { return m_evidence; }
    QStringList activity() const { return m_activity; }
    QVariantList notes() const { return m_notes; }

    void setEngine(QQmlEngine *engine) { m_engine = engine; }

    Q_INVOKABLE bool load(const QString &pathOrUrl);
    Q_INVOKABLE void openScene();
    Q_INVOKABLE void newFilm();
    Q_INVOKABLE void reload();
    // Lets go of compiled components nothing uses any more, so the next load
    // compiles the scene's other files afresh too.
    Q_INVOKABLE void trimCache();
    Q_INVOKABLE QSize formatSize(const QString &format) const;
    Q_INVOKABLE QUrl resolve(const QString &relative) const;
    Q_INVOKABLE void exportVideo(const QStringList &formats);
    Q_INVOKABLE void cancelExport();
    Q_INVOKABLE void showOutputs();
    Q_INVOKABLE void clearMessage();
    void addWarning(const QString &warning);

    // Pins a note to a moment of the film in notes.md, and renders that frame,
    // with the point ringed, as its evidence. Returns the note's id.
    Q_INVOKABLE QString addNote(double at, const QString &format, const QString &beat, int x, int y,
                                const QString &target, const QString &text);
    // Puts a note, or a pointer to every open note, on the clipboard for the agent.
    Q_INVOKABLE void copyNote(const QString &id);
    Q_INVOKABLE void copyOpenNotes();
    // Opens a file or folder of the studio with the desktop's default app.
    Q_INVOKABLE void openStudioPath(const QString &path);

    bool createFilm(const QString &folder);

signals:
    void sceneChanged();
    void warningChanged();
    void exportChanged();
    void messageChanged();
    void studioChanged();
    void gatesChanged();
    void assetsChanged();
    void evidenceChanged();
    void activityChanged();
    void notesChanged();

private:
    void watch();
    void filesChanged();
    QString signature() const;
    void setMessage(const QString &message, bool error, const QStringList &outputs = {});
    void readExportOutput();
    void exportFinished(int code, QProcess::ExitStatus status);
    void removePartials();
    void watchStudio();
    void reloadStudio();

    FilePicker *m_picker;
    QString m_renderer;
    QQmlEngine *m_engine = nullptr;
    QString m_path;
    int m_revision = 0;
    QString m_signature;
    QString m_warning;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;

    QProcess m_export;
    QStringList m_exportFormats;
    QByteArray m_exportBuffer;
    QStringList m_exportOutputs;
    QString m_exportFormat;
    double m_progress = 0;
    bool m_canceled = false;

    QString m_root;
    QFileSystemWatcher m_studioWatcher;
    QTimer m_studioDebounce;
    QVariantList m_gates, m_assets, m_evidence, m_notes;
    QStringList m_missing, m_activity;

    QString m_message;
    bool m_messageIsError = false;
    QStringList m_outputs;
};
