#pragma once

// The optional AI side: depth from a single picture (Depth Anything V2
// Small) and click-to-select (MobileSAM), run on the CPU with ONNX Runtime.
// The runtime is loaded when first needed rather than linked, so omotion
// works without it and says what's missing when an AI feature is asked for.

#include <QImage>
#include <QObject>
#include <QList>
#include <QMutex>
#include <QPointF>
#include <QString>
#include <memory>

class Ai {
public:
    struct Model {
        QString file;
        QString url;
        qint64 size;
        QString sha256;
    };

    static Ai &instance();

    static QString modelDirectory();
    static const QList<Model> &models();
    static qint64 modelBytes();
    // The models that still need downloading.
    static QList<Model> missingModels();
    // Points at another library or model folder; for tests.
    static void setPaths(const QString &library, const QString &models);

    // Empty when the AI is ready; otherwise what's missing, in words.
    QString problem();
    bool runtimeLoaded();
    // Whether the runtime itself is what's missing (rather than the models).
    bool runtimeMissing();
    // Looks for the runtime again next time, say after it's been installed.
    void retry();

    // Relative depth for a picture, larger nearer, as a 16-bit grey image at
    // the size the model worked at (around 518 pixels on the short side).
    QImage depth(const QImage &image, QString *error);

    // Masks for the thing under the points (image pixels; label 1 to take,
    // 0 to leave out), as Alpha8 images the size of image, best first. The
    // picture's analysis is kept under key, so the next click is quick.
    QList<QImage> segment(const QImage &image, const QString &key, const QList<QPointF> &points,
                          const QList<int> &labels, QString *error);

    ~Ai();

private:
    Ai() = default;
    struct Runtime;
    std::unique_ptr<Runtime> m_runtime;
    QMutex m_mutex;
    QString m_loadError;
    bool m_tried = false;

    bool load(QString *error);
};

class QNetworkAccessManager;
class QNetworkReply;
class QCryptographicHash;
class QFile;

// Downloads the missing models one after another into the model folder,
// checking each against its hash before putting it in place.
class ModelDownload : public QObject {
    Q_OBJECT

public:
    explicit ModelDownload(QObject *parent = nullptr);
    ~ModelDownload() override;

    void start();
    void cancel();
    bool busy() const { return m_reply != nullptr; }

    // Runs a download to the end on a local event loop; for the command line.
    static bool run(QString *error, bool verbose);

signals:
    void progress(qint64 received, qint64 total);
    void finished(bool ok, const QString &message);

private:
    void next();
    void fail(const QString &message);

    QNetworkAccessManager *m_network = nullptr;
    QNetworkReply *m_reply = nullptr;
    QFile *m_file = nullptr;
    QCryptographicHash *m_hash = nullptr;
    QList<Ai::Model> m_queue;
    qint64 m_done = 0;
    qint64 m_total = 0;
};
