#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QProcess>
#include <QSize>
#include <QStringList>
#include <QTimer>
#include <QUrl>

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

    bool createFilm(const QString &folder);

signals:
    void sceneChanged();
    void warningChanged();
    void exportChanged();
    void messageChanged();

private:
    void watch();
    void filesChanged();
    QString signature() const;
    void setMessage(const QString &message, bool error, const QStringList &outputs = {});
    void readExportOutput();
    void exportFinished(int code, QProcess::ExitStatus status);
    void removePartials();

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

    QString m_message;
    bool m_messageIsError = false;
    QStringList m_outputs;
};
