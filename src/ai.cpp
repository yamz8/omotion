#include "ai.h"

#include <QCryptographicHash>
#include <QDir>
#include <QHash>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTextStream>
#include <QFile>
#include <QFileInfo>
#include <QLibrary>
#include <QMutexLocker>
#include <QPainter>
#include <QRect>
#include <QStandardPaths>
#include <QScopeGuard>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#ifdef OMOTION_AI
#include <onnxruntime/onnxruntime_c_api.h>
#endif

namespace {
QString libraryPath;
QString modelPath;

// The oldest runtime whose API has everything used here.
constexpr uint32_t ApiVersion = 16;
}

QString Ai::modelDirectory() {
    return modelPath.isEmpty()
               ? QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/omotion/models"
               : modelPath;
}

const QList<Ai::Model> &Ai::models() {
    // Pinned to the revisions these were checked against, with their hashes,
    // so a changed upload can't slip in unnoticed.
    static const QList<Model> list{
        {"depth-anything-v2-small.onnx",
         "https://huggingface.co/onnx-community/depth-anything-v2-small/resolve/4472b7362082ad9968fee890ca0f1e5aca36b93d/onnx/model.onnx", 99060839,
         "afb6a5c28f3b6bf1618c6e43f02073ef9dfdc70e937502d51603e57b0a1df10c"},
        {"mobile-sam-encoder.onnx", "https://huggingface.co/Acly/MobileSAM/resolve/0d3b403339b4674a82493d5e97964dd78089ddc8/mobile_sam_image_encoder.onnx",
         28157093, "580f5fb648ea1062c0aabc26217aed56921985f03f0cbbd852bba81d760cc749"},
        {"mobile-sam-decoder.onnx", "https://huggingface.co/Acly/MobileSAM/resolve/0d3b403339b4674a82493d5e97964dd78089ddc8/sam_mask_decoder_multi.onnx",
         16496559, "8976b90a87ba50a6a72217a5ff994f7d25ce16f2229fcc1ed259e1294c622ffe"},
        {"lama.onnx", "https://huggingface.co/Carve/LaMa-ONNX/resolve/c3c0c9e468934d62e79c329e35d82dd09ff8c444/lama_fp32.onnx",
         208044816, "1faef5301d78db7dda502fe59966957ec4b79dd64e16f03ed96913c7a4eb68d6"},
        {"triposr-encoder-int8.onnx",
         "https://huggingface.co/fernandotonon/QtMeshEditor-triposr-onnx/resolve/01b2f1add36601f964a89fd8a3159919ced4aa40/triposr_encoder_int8.onnx",
         436065556, "0439dd35121fde195e73f93b0d81113be67ef9c1caa1db6498ae6c16e12fe089"},
        {"triposr-decoder.onnx",
         "https://huggingface.co/fernandotonon/QtMeshEditor-triposr-onnx/resolve/01b2f1add36601f964a89fd8a3159919ced4aa40/triposr_decoder.onnx",
         180692, "68ab06fb5ed4646c6dc15293ec83de62ba11d50294030795e860bea96cd81706"},
        {"moge-2-vits-normal.onnx",
         "https://huggingface.co/Ruicheng/moge-2-vits-normal-onnx/resolve/e50ffda41565591092adea54c6ac83d6212e1e23/model.onnx",
         140852051, "24eacb5dc7a2c54c7bc98f7de085ffbed79ad006ea5b664c2c2cdc02ff3a52f0"},
    };
    return list;
}

bool Ai::solveLens(Geometry &g, double scale, const QSize &picture) {
    // The model's view plane: the half-diagonal spans 1, so a cell's place
    // on it is (u, v), and a point (x, y, z + shift) seen through a lens f
    // lands at f (x, y) / (z + shift). For each shift the best f has a
    // closed form; the shift is the one that leaves the least error.
    const int w = g.width, h = g.height;
    if (w < 2 || h < 2 || !(scale > 0))
        return false;
    const double aspect = double(w) / h, sx = aspect / std::sqrt(1 + aspect * aspect), sy = 1 / std::sqrt(1 + aspect * aspect);
    const int step = std::max(1, int(std::sqrt(double(w) * h / 20000)));
    std::vector<double> u, v, x, y, z;
    for (int row = 0; row < h; row += step)
        for (int col = 0; col < w; col += step) {
            const size_t i = size_t(row) * w + col;
            if (!g.valid[i])
                continue;
            u.push_back(sx * (2 * (col + .5) / w - 1));
            v.push_back(sy * (2 * (row + .5) / h - 1));
            x.push_back(g.points[i * 3]);
            y.push_back(g.points[i * 3 + 1]);
            z.push_back(g.points[i * 3 + 2]);
        }
    if (z.size() < 50)
        return false;
    const double nearest = *std::min_element(z.begin(), z.end());
    auto fit = [&](double shift, double *focal) {
        double qu = 0, qq = 0;
        for (size_t i = 0; i < z.size(); ++i) {
            const double d = z[i] + shift, px = x[i] / d, py = y[i] / d;
            qu += px * u[i] + py * v[i];
            qq += px * px + py * py;
        }
        const double f = qq > 0 ? qu / qq : 0;
        double error = 0;
        for (size_t i = 0; i < z.size(); ++i) {
            const double d = z[i] + shift, eu = f * x[i] / d - u[i], ev = f * y[i] / d - v[i];
            error += eu * eu + ev * ev;
        }
        if (focal)
            *focal = f;
        return error;
    };
    // Every point stays in front of the camera; scanned finely near that
    // limit, coarsely further out, then narrowed down around the best.
    const double lo = -nearest + 1e-3;
    double best = lo, least = std::numeric_limits<double>::infinity();
    for (int i = 0; i <= 600; ++i) {
        const double shift = i <= 300 ? lo + 5.0 * i / 300 : lo + 5 + 195.0 * (i - 300) / 300;
        const double error = fit(shift, nullptr);
        if (error < least)
            least = error, best = shift;
    }
    double a = std::max(lo, best - 1), b = best + 1;
    for (int i = 0; i < 60; ++i) {
        const double m1 = a + (b - a) / 3, m2 = b - (b - a) / 3;
        if (fit(m1, nullptr) < fit(m2, nullptr))
            b = m2;
        else
            a = m1;
    }
    best = (a + b) / 2;
    double focal = 0;
    fit(best, &focal);
    if (!(focal > 0))
        return false;
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        g.points[i * 3 + 2] += float(best);
        for (int c = 0; c < 3; ++c)
            g.points[i * 3 + c] *= float(scale);
    }
    g.focal = focal / 2 * std::hypot(picture.width(), picture.height());
    return true;
}

Ai::Shape Ai::surface(const std::vector<float> &field, int n, double r) {
    Shape shape;
    if (n < 2 || field.size() != size_t(n) * n * n)
        return shape;
    auto at = [&](int x, int y, int z) { return field[(size_t(x) * n + y) * n + z]; };
    auto where = [&](double i) { return -r + 2 * r * i / (n - 1); };
    // A vertex in every cell the surface passes through: the average of
    // where it crosses the cell's edges.
    std::vector<qint32> vertexOf(size_t(n - 1) * (n - 1) * (n - 1), -1);
    static const int corner[8][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1}};
    static const int edge[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (int x = 0; x + 1 < n; ++x)
        for (int y = 0; y + 1 < n; ++y)
            for (int z = 0; z + 1 < n; ++z) {
                float v[8];
                int inside = 0;
                for (int c = 0; c < 8; ++c) {
                    v[c] = at(x + corner[c][0], y + corner[c][1], z + corner[c][2]);
                    inside += v[c] > 0;
                }
                if (inside == 0 || inside == 8)
                    continue;
                double sum[3] = {0, 0, 0};
                int crossings = 0;
                for (const auto &e : edge) {
                    const float a = v[e[0]], b = v[e[1]];
                    if ((a > 0) == (b > 0))
                        continue;
                    const double t = a / (a - b);
                    for (int k = 0; k < 3; ++k)
                        sum[k] += corner[e[0]][k] + t * (corner[e[1]][k] - corner[e[0]][k]);
                    ++crossings;
                }
                vertexOf[(size_t(x) * (n - 1) + y) * (n - 1) + z] = qint32(shape.positions.size() / 3);
                shape.positions.push_back(float(where(x + sum[0] / crossings)));
                shape.positions.push_back(float(where(y + sum[1] / crossings)));
                shape.positions.push_back(float(where(z + sum[2] / crossings)));
            }
    // A quad across every grid edge the surface crosses, joining the four
    // cells around that edge, wound to face out of the inside.
    auto cell = [&](int x, int y, int z) -> qint32 {
        if (x < 0 || y < 0 || z < 0 || x >= n - 1 || y >= n - 1 || z >= n - 1)
            return -1;
        return vertexOf[(size_t(x) * (n - 1) + y) * (n - 1) + z];
    };
    for (int x = 0; x < n; ++x)
        for (int y = 0; y < n; ++y)
            for (int z = 0; z < n; ++z)
                for (int axis = 0; axis < 3; ++axis) {
                    const int dx = axis == 0, dy = axis == 1, dz = axis == 2;
                    if (x + dx >= n || y + dy >= n || z + dz >= n)
                        continue;
                    const bool a = at(x, y, z) > 0, b = at(x + dx, y + dy, z + dz) > 0;
                    if (a == b)
                        continue;
                    // The four cells sharing this edge, around it.
                    qint32 q[4];
                    if (axis == 0)
                        q[0] = cell(x, y - 1, z - 1), q[1] = cell(x, y, z - 1), q[2] = cell(x, y, z), q[3] = cell(x, y - 1, z);
                    else if (axis == 1)
                        q[0] = cell(x - 1, y, z - 1), q[1] = cell(x - 1, y, z), q[2] = cell(x, y, z), q[3] = cell(x, y, z - 1);
                    else
                        q[0] = cell(x - 1, y - 1, z), q[1] = cell(x, y - 1, z), q[2] = cell(x, y, z), q[3] = cell(x - 1, y, z);
                    if (q[0] < 0 || q[1] < 0 || q[2] < 0 || q[3] < 0)
                        continue;
                    if (!a)
                        std::swap(q[1], q[3]);
                    shape.triangles.insert(shape.triangles.end(), {quint32(q[0]), quint32(q[1]), quint32(q[2]),
                                                                   quint32(q[0]), quint32(q[2]), quint32(q[3])});
                }
    return shape;
}

qint64 Ai::modelBytes() {
    qint64 total = 0;
    for (const Model &model : models())
        total += model.size;
    return total;
}

QList<Ai::Model> Ai::missingModels() {
    QList<Model> missing;
    for (const Model &model : models()) {
        const QFileInfo info(QDir(modelDirectory()).filePath(model.file));
        if (!info.exists() || info.size() != model.size)
            missing.append(model);
    }
    return missing;
}

void Ai::setPaths(const QString &library, const QString &models) {
    libraryPath = library;
    modelPath = models;
}

ModelDownload::ModelDownload(QObject *parent) : QObject(parent) {}

ModelDownload::~ModelDownload() { cancel(); }

void ModelDownload::start() {
    if (busy())
        return;
    m_queue = Ai::missingModels();
    m_done = 0;
    m_total = 0;
    for (const Ai::Model &model : m_queue)
        m_total += model.size;
    if (!QDir().mkpath(Ai::modelDirectory()))
        return fail(QString("Couldn't create %1.").arg(Ai::modelDirectory()));
    if (!m_network)
        m_network = new QNetworkAccessManager(this);
    next();
}

void ModelDownload::next() {
    if (m_queue.isEmpty()) {
        emit finished(true, "The AI models are ready.");
        return;
    }
    const Ai::Model model = m_queue.first();
    const QString path = QDir(Ai::modelDirectory()).filePath(model.file);
    // Into a partial file beside the target, renamed once it checks out.
    m_file = new QFile(path + ".part", this);
    if (!m_file->open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail(QString("Couldn't write %1: %2").arg(m_file->fileName(), m_file->errorString()));
    m_hash = new QCryptographicHash(QCryptographicHash::Sha256);
    QNetworkRequest request{QUrl(model.url)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    m_reply = m_network->get(request);
    connect(m_reply, &QNetworkReply::readyRead, this, [this] {
        const QByteArray data = m_reply->readAll();
        m_file->write(data);
        m_hash->addData(data);
    });
    connect(m_reply, &QNetworkReply::downloadProgress, this,
            [this](qint64 received, qint64) { emit progress(m_done + received, m_total); });
    connect(m_reply, &QNetworkReply::finished, this, [this, model, path] {
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        reply->deleteLater();
        const QByteArray rest = reply->readAll();
        m_file->write(rest);
        m_hash->addData(rest);
        m_file->close();
        const QString partial = m_file->fileName();
        const QString digest = QString::fromLatin1(m_hash->result().toHex());
        delete m_hash;
        m_hash = nullptr;
        m_file->deleteLater();
        m_file = nullptr;
        if (reply->error() != QNetworkReply::NoError) {
            QFile::remove(partial);
            return fail(reply->error() == QNetworkReply::OperationCanceledError
                            ? QString("Download cancelled.")
                            : QString("Couldn't download %1: %2").arg(model.file, reply->errorString()));
        }
        if (digest != model.sha256) {
            QFile::remove(partial);
            return fail(QString("%1 didn't match its checksum, so it wasn't kept.").arg(model.file));
        }
        QFile::remove(path);
        if (!QFile::rename(partial, path))
            return fail(QString("Couldn't put %1 in place.").arg(path));
        m_done += model.size;
        m_queue.removeFirst();
        next();
    });
}

void ModelDownload::cancel() {
    if (m_reply)
        m_reply->abort();
}

void ModelDownload::fail(const QString &message) {
    m_queue.clear();
    emit finished(false, message);
}

bool ModelDownload::run(QString *error, bool verbose) {
    ModelDownload download;
    bool ok = false;
    QEventLoop loop;
    int lastPercent = -1;
    QObject::connect(&download, &ModelDownload::progress, &loop, [&](qint64 received, qint64 total) {
        const int percent = total > 0 ? int(received * 100 / total) : 0;
        if (verbose && percent / 10 != lastPercent / 10)
            QTextStream(stdout) << "  " << percent << "%\n";
        lastPercent = percent;
    });
    QObject::connect(&download, &ModelDownload::finished, &loop, [&](bool done, const QString &message) {
        ok = done;
        if (!done && error)
            *error = message;
        loop.quit();
    });
    download.start();
    if (download.busy())
        loop.exec();
    else if (Ai::missingModels().isEmpty())
        ok = true;
    return ok;
}

Ai &Ai::instance() {
    static Ai ai;
    return ai;
}

#ifndef OMOTION_AI

struct Ai::Runtime {};
Ai::~Ai() = default;
bool Ai::load(QString *error) {
    if (error)
        *error = "This omotion was built without AI support; rebuild it with onnxruntime-cpu installed.";
    return false;
}
bool Ai::runtimeLoaded() { return false; }
bool Ai::runtimeMissing() { return true; }
void Ai::retry() {}
QString Ai::problem() {
    QString error;
    load(&error);
    return error;
}
QImage Ai::depth(const QImage &, QString *error) {
    load(error);
    return {};
}
QList<QImage> Ai::segment(const QImage &, const QString &, const QList<QPointF> &, const QList<int> &, QString *error) {
    load(error);
    return {};
}
QImage Ai::inpaint(const QImage &, const QImage &, QString *error) {
    load(error);
    return {};
}
Ai::Geometry Ai::geometry(const QImage &, QString *error) {
    load(error);
    return {};
}
Ai::Shape Ai::objectShape(const QImage &, QString *error) {
    load(error);
    return {};
}
bool Ai::canInpaint() { return false; }

#else

struct Ai::Runtime {
    QLibrary library;
    const OrtApi *api = nullptr;
    OrtEnv *env = nullptr;
    OrtSessionOptions *options = nullptr;
    OrtMemoryInfo *memory = nullptr;
    OrtSession *depth = nullptr;
    OrtSession *encoder = nullptr;
    OrtSession *decoder = nullptr;
    OrtSession *inpainter = nullptr;
    OrtSession *shaper = nullptr;
    OrtSession *objectEncoder = nullptr;
    OrtSession *objectDecoder = nullptr;

    // Fills already worked out, by their input, so redrawing a layer's
    // pieces doesn't paint the same hole again.
    QHash<QByteArray, QImage> fills;
    QList<QByteArray> fillOrder;

    // The last pictures analysed for selecting, so further clicks only decode.
    struct Encoded {
        QString key;
        std::vector<float> embedding;
        double scale = 1;
    };
    QList<Encoded> encoded;

    ~Runtime() {
        if (!api)
            return;
        for (OrtSession *session : {depth, encoder, decoder, inpainter, shaper, objectEncoder, objectDecoder})
            if (session)
                api->ReleaseSession(session);
        if (memory)
            api->ReleaseMemoryInfo(memory);
        if (options)
            api->ReleaseSessionOptions(options);
        if (env)
            api->ReleaseEnv(env);
    }

    bool ok(OrtStatus *status, QString *error) {
        if (!status)
            return true;
        if (error)
            *error = QString("The AI failed: %1").arg(QString::fromUtf8(api->GetErrorMessage(status)));
        api->ReleaseStatus(status);
        return false;
    }

    OrtSession *session(OrtSession *&slot, const QString &file, QString *error) {
        if (slot)
            return slot;
        const QString path = QDir(Ai::modelDirectory()).filePath(file);
        if (!QFileInfo::exists(path)) {
            if (error)
                *error = QString("The AI model %1 isn't downloaded.").arg(file);
            return nullptr;
        }
        if (!ok(api->CreateSession(env, QFile::encodeName(path).constData(), options, &slot), error))
            slot = nullptr;
        return slot;
    }

    OrtValue *tensor(std::vector<float> &data, const std::vector<int64_t> &shape, QString *error) {
        OrtValue *value = nullptr;
        if (!ok(api->CreateTensorWithDataAsOrtValue(memory, data.data(), data.size() * sizeof(float), shape.data(),
                                                   shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &value),
                error))
            return nullptr;
        return value;
    }

    OrtValue *scalar(int64_t &data, QString *error) {
        OrtValue *value = nullptr;
        if (!ok(api->CreateTensorWithDataAsOrtValue(memory, &data, sizeof(int64_t), nullptr, 0,
                                                   ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &value),
                error))
            return nullptr;
        return value;
    }

    // Runs a model, giving back its outputs; the caller releases them.
    bool run(OrtSession *session, const std::vector<const char *> &inputNames, const std::vector<OrtValue *> &inputs,
             const std::vector<const char *> &outputNames, std::vector<OrtValue *> &outputs, QString *error) {
        outputs.assign(outputNames.size(), nullptr);
        std::vector<const OrtValue *> in(inputs.begin(), inputs.end());
        const bool done = ok(api->Run(session, nullptr, inputNames.data(), in.data(), in.size(), outputNames.data(),
                                      outputNames.size(), outputs.data()),
                             error);
        for (OrtValue *value : inputs)
            if (value)
                api->ReleaseValue(value);
        return done;
    }

    // An output's shape, or nothing if it can't be read.
    std::vector<int64_t> shape(OrtValue *value) {
        OrtTensorTypeAndShapeInfo *info = nullptr;
        std::vector<int64_t> dims;
        if (!ok(api->GetTensorTypeAndShape(value, &info), nullptr))
            return dims;
        size_t count = 0;
        if (ok(api->GetDimensionsCount(info, &count), nullptr)) {
            dims.resize(count);
            if (!ok(api->GetDimensions(info, dims.data(), count), nullptr))
                dims.clear();
        }
        api->ReleaseTensorTypeAndShapeInfo(info);
        return dims;
    }

    float *data(OrtValue *value) {
        float *pointer = nullptr;
        if (!ok(api->GetTensorMutableData(value, reinterpret_cast<void **>(&pointer)), nullptr))
            return nullptr;
        return pointer;
    }
};

Ai::~Ai() = default;

bool Ai::load(QString *error) {
    if (m_runtime)
        return true;
    if (m_tried) {
        if (error)
            *error = m_loadError;
        return false;
    }
    m_tried = true;
    auto runtime = std::make_unique<Runtime>();
    if (libraryPath.isEmpty())
        runtime->library.setFileNameAndVersion("onnxruntime", 1);
    else
        runtime->library.setFileName(libraryPath);
    using GetApiBase = const OrtApiBase *(*)();
    const auto getBase = reinterpret_cast<GetApiBase>(runtime->library.resolve("OrtGetApiBase"));
    if (!getBase) {
        m_loadError = "The AI needs ONNX Runtime, which isn't installed. Install it with: omarchy-pkg-add onnxruntime-cpu";
        if (error)
            *error = m_loadError;
        return false;
    }
    runtime->api = getBase()->GetApi(ApiVersion);
    if (!runtime->api) {
        m_loadError = QString("ONNX Runtime %1 is too old for omotion's AI; it needs 1.16 or newer.")
                          .arg(QString::fromUtf8(getBase()->GetVersionString()));
        if (error)
            *error = m_loadError;
        return false;
    }
    QString failure;
    if (!runtime->ok(runtime->api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "omotion", &runtime->env), &failure) ||
        !runtime->ok(runtime->api->CreateSessionOptions(&runtime->options), &failure) ||
        !runtime->ok(runtime->api->SetIntraOpNumThreads(runtime->options, std::max(1, QThread::idealThreadCount())), &failure) ||
        !runtime->ok(runtime->api->SetSessionGraphOptimizationLevel(runtime->options, ORT_ENABLE_ALL), &failure) ||
        !runtime->ok(runtime->api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &runtime->memory), &failure)) {
        m_loadError = failure;
        if (error)
            *error = m_loadError;
        return false;
    }
    m_runtime = std::move(runtime);
    m_loaded = true;
    return true;
}

bool Ai::runtimeLoaded() {
    QMutexLocker lock(&m_mutex);
    return load(nullptr);
}

bool Ai::runtimeMissing() { return !runtimeLoaded(); }

void Ai::retry() {
    QMutexLocker lock(&m_mutex);
    if (!m_runtime)
        m_tried = false;
}

QString Ai::problem() {
    QMutexLocker lock(&m_mutex);
    QString error;
    if (!load(&error))
        return error;
    const QList<Model> missing = missingModels();
    if (!missing.isEmpty())
        return QString("The AI models aren't downloaded yet (%1 MB).").arg(modelBytes() / 1000000);
    return {};
}

QImage Ai::depth(const QImage &picture, QString *error) {
    QMutexLocker lock(&m_mutex);
    if (!load(error))
        return {};
    Runtime &rt = *m_runtime;
    OrtSession *session = rt.session(rt.depth, "depth-anything-v2-small.onnx", error);
    if (!session)
        return {};

    // The model wants sides in multiples of 14, about 518 on the short one,
    // keeping the picture's shape (but not too long a long one).
    const QImage image = picture.convertToFormat(QImage::Format_RGB888);
    double scale = 518.0 / std::min(image.width(), image.height());
    scale = std::min(scale, 1036.0 / std::max(image.width(), image.height()));
    const int w = std::max(14, int(std::lround(image.width() * scale / 14)) * 14);
    const int h = std::max(14, int(std::lround(image.height() * scale / 14)) * 14);
    const QImage small = image.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    std::vector<float> input(size_t(3) * w * h);
    const float mean[3] = {.485f, .456f, .406f}, deviation[3] = {.229f, .224f, .225f};
    for (int y = 0; y < h; ++y) {
        const uchar *line = small.constScanLine(y);
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c)
                input[size_t(c) * w * h + size_t(y) * w + x] = (line[x * 3 + c] / 255.f - mean[c]) / deviation[c];
    }
    OrtValue *value = rt.tensor(input, {1, 3, h, w}, error);
    std::vector<OrtValue *> outputs;
    if (!value || !rt.run(session, {"pixel_values"}, {value}, {"predicted_depth"}, outputs, error))
        return {};

    const std::vector<int64_t> dims = rt.shape(outputs[0]);
    const float *depth = rt.data(outputs[0]);
    QImage result;
    if (dims.size() == 3 && depth) {
        const int ow = int(dims[2]), oh = int(dims[1]);
        const auto [lo, hi] = std::minmax_element(depth, depth + size_t(ow) * oh);
        const float range = std::max(1e-6f, *hi - *lo);
        result = QImage(ow, oh, QImage::Format_Grayscale16);
        for (int y = 0; y < oh; ++y) {
            quint16 *line = reinterpret_cast<quint16 *>(result.scanLine(y));
            for (int x = 0; x < ow; ++x)
                line[x] = quint16(std::lround((depth[size_t(y) * ow + x] - *lo) / range * 65535));
        }
    } else if (error) {
        *error = "The depth model gave back something unexpected.";
    }
    rt.api->ReleaseValue(outputs[0]);
    return result;
}

Ai::Geometry Ai::geometry(const QImage &picture, QString *error) {
    QMutexLocker lock(&m_mutex);
    if (!load(error))
        return {};
    Runtime &rt = *m_runtime;
    OrtSession *session = rt.session(rt.shaper, "moge-2-vits-normal.onnx", error);
    if (!session)
        return {};

    // Any size works; around 960 across keeps it to a few seconds here.
    const QImage image = picture.convertToFormat(QImage::Format_RGB888);
    const double fit = std::min(1.0, 960.0 / std::max(image.width(), image.height()));
    const int w = std::max(14, int(std::lround(image.width() * fit))), h = std::max(14, int(std::lround(image.height() * fit)));
    const QImage small = image.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    std::vector<float> input(size_t(3) * w * h);
    for (int y = 0; y < h; ++y) {
        const uchar *line = small.constScanLine(y);
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c)
                input[size_t(c) * w * h + size_t(y) * w + x] = line[x * 3 + c] / 255.f;
    }
    int64_t tokens = 1800;
    OrtValue *image0 = rt.tensor(input, {1, 3, h, w}, error);
    OrtValue *tokens0 = image0 ? rt.scalar(tokens, error) : nullptr;
    std::vector<OrtValue *> outputs;
    if (!image0 || !tokens0 ||
        !rt.run(session, {"image", "num_tokens"}, {image0, tokens0}, {"points", "normal", "mask", "scale"}, outputs, error))
        return {};
    auto release = qScopeGuard([&] {
        for (OrtValue *value : outputs)
            if (value)
                rt.api->ReleaseValue(value);
    });
    const std::vector<int64_t> dims = rt.shape(outputs[0]);
    const float *points = rt.data(outputs[0]), *normals = rt.data(outputs[1]), *mask = rt.data(outputs[2]),
                *scale = rt.data(outputs[3]);
    if (dims.size() != 4 || dims[3] != 3 || !points || !normals || !mask || !scale) {
        if (error)
            *error = "The shape model gave back something unexpected.";
        return {};
    }
    Geometry geometry;
    geometry.width = int(dims[2]);
    geometry.height = int(dims[1]);
    const size_t cells = size_t(geometry.width) * geometry.height;
    geometry.points.assign(points, points + cells * 3);
    geometry.normals.assign(normals, normals + cells * 3);
    geometry.valid.resize(cells);
    for (size_t i = 0; i < cells; ++i)
        geometry.valid[i] = mask[i] > .5f;
    if (!solveLens(geometry, *scale, picture.size())) {
        if (error)
            *error = "The shape model couldn't make out a lens in this picture.";
        return {};
    }
    return geometry;
}

Ai::Shape Ai::objectShape(const QImage &cutout, QString *error) {
    QMutexLocker lock(&m_mutex);
    if (!load(error))
        return {};
    Runtime &rt = *m_runtime;
    OrtSession *encoder = rt.session(rt.objectEncoder, "triposr-encoder-int8.onnx", error);
    OrtSession *decoder = encoder ? rt.session(rt.objectDecoder, "triposr-decoder.onnx", error) : nullptr;
    if (!decoder)
        return {};

    // As the model was taught: the thing over grey, centred in a square it
    // fills 85% of, 512 pixels a side.
    const QImage rgba = cutout.convertToFormat(QImage::Format_ARGB32);
    QRect box;
    for (int y = 0; y < rgba.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(rgba.constScanLine(y));
        for (int x = 0; x < rgba.width(); ++x)
            if (qAlpha(line[x]) > 16)
                box |= QRect(x, y, 1, 1);
    }
    if (box.isEmpty()) {
        if (error)
            *error = "There's nothing in the cut-out to give a shape to.";
        return {};
    }
    const int side = int(std::max(box.width(), box.height()) / .85);
    QImage square(side, side, QImage::Format_ARGB32);
    square.fill(QColor(128, 128, 128));
    {
        QPainter painter(&square);
        painter.drawImage(QPoint((side - box.width()) / 2, (side - box.height()) / 2), rgba.copy(box));
    }
    const QImage input = square.scaled(512, 512, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB888);
    std::vector<float> pixels(size_t(3) * 512 * 512);
    for (int y = 0; y < 512; ++y) {
        const uchar *line = input.constScanLine(y);
        for (int x = 0; x < 512; ++x)
            for (int c = 0; c < 3; ++c)
                pixels[size_t(c) * 512 * 512 + size_t(y) * 512 + x] = line[x * 3 + c] / 255.f;
    }
    OrtValue *image = rt.tensor(pixels, {1, 3, 512, 512}, error);
    std::vector<OrtValue *> encoded;
    if (!image || !rt.run(encoder, {"image"}, {image}, {"scene_codes"}, encoded, error))
        return {};
    const std::vector<int64_t> codeShape = rt.shape(encoded[0]);
    size_t codeCount = 1;
    for (const int64_t d : codeShape)
        codeCount *= size_t(d);
    std::vector<float> codes(rt.data(encoded[0]), rt.data(encoded[0]) + codeCount);
    rt.api->ReleaseValue(encoded[0]);

    // The field's density and colour at points, a chunk at a time.
    auto query = [&](const std::vector<float> &points, std::vector<float> *density, std::vector<float> *color) {
        const size_t count = points.size() / 3;
        for (size_t start = 0; start < count; start += 65536) {
            const size_t chunk = std::min<size_t>(65536, count - start);
            std::vector<float> part(points.begin() + start * 3, points.begin() + (start + chunk) * 3);
            OrtValue *codeValue = rt.tensor(codes, codeShape, error);
            OrtValue *pointValue = codeValue ? rt.tensor(part, {1, int64_t(chunk), 3}, error) : nullptr;
            std::vector<OrtValue *> out;
            if (!pointValue || !rt.run(decoder, {"scene_codes", "points"}, {codeValue, pointValue}, {"density", "color"}, out, error))
                return false;
            const float *d = rt.data(out[0]), *c = rt.data(out[1]);
            if (density)
                density->insert(density->end(), d, d + chunk);
            if (color)
                color->insert(color->end(), c, c + chunk * 3);
            for (OrtValue *value : out)
                rt.api->ReleaseValue(value);
        }
        return true;
    };
    constexpr int N = 128;
    constexpr double R = .87;
    std::vector<float> grid;
    grid.reserve(size_t(N) * N * N * 3);
    for (int x = 0; x < N; ++x)
        for (int y = 0; y < N; ++y)
            for (int z = 0; z < N; ++z)
                grid.insert(grid.end(), {float(-R + 2 * R * x / (N - 1)), float(-R + 2 * R * y / (N - 1)), float(-R + 2 * R * z / (N - 1))});
    std::vector<float> density;
    if (!query(grid, &density, nullptr))
        return {};
    for (float &d : density)
        d -= 25;  // Where the model's density passes 25 is the surface.
    Shape shape = surface(density, N, R);
    if (shape.isNull()) {
        if (error)
            *error = "The shape model saw no solid thing in the cut-out.";
        return {};
    }
    if (!query(shape.positions, nullptr, &shape.colors))
        return {};
    for (float &c : shape.colors)
        c = std::clamp(c, 0.f, 1.f);
    // The model's space stood up: turned -90° about x, then 90° about y.
    for (size_t i = 0; i < shape.positions.size(); i += 3) {
        const float x = shape.positions[i], y = shape.positions[i + 1], z = shape.positions[i + 2];
        const float x1 = x, y1 = z, z1 = -y;  // -90° about x.
        shape.positions[i] = z1, shape.positions[i + 1] = y1, shape.positions[i + 2] = -x1;  // 90° about y.
    }
    return shape;
}

bool Ai::canInpaint() {
    if (!m_loaded) {
        QMutexLocker lock(&m_mutex);
        if (!load(nullptr))
            return false;
    }
    const QFileInfo model(QDir(modelDirectory()).filePath("lama.onnx"));
    for (const Model &known : models())
        if (known.file == "lama.onnx")
            return model.exists() && model.size() == known.size;
    return false;
}

QImage Ai::inpaint(const QImage &picture, const QImage &holes, QString *error) {
    QMutexLocker lock(&m_mutex);
    if (!load(error))
        return {};
    Runtime &rt = *m_runtime;
    const QImage image = picture.convertToFormat(QImage::Format_RGB888);
    const QImage mask = holes.convertToFormat(QImage::Format_Alpha8);
    if (image.isNull() || mask.size() != image.size()) {
        if (error)
            *error = "Nothing to fill.";
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha1);
    for (int y = 0; y < image.height(); ++y) {
        hash.addData(QByteArrayView(reinterpret_cast<const char *>(image.constScanLine(y)), image.width() * 3));
        hash.addData(QByteArrayView(reinterpret_cast<const char *>(mask.constScanLine(y)), mask.width()));
    }
    const QByteArray key = hash.result();
    if (rt.fills.contains(key))
        return rt.fills.value(key);
    // Kept on disk too, so redrawing a layer's pieces later (another cut, a
    // reshape, the command line) doesn't paint the same hole twice.
    const QString cacheDirectory = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/omotion/fills";
    const QString cached = QDir(cacheDirectory).filePath(QString::fromLatin1(key.toHex()) + ".png");
    {
        const QImage kept(cached);
        if (kept.size() == image.size()) {
            rt.fills.insert(key, kept.convertToFormat(QImage::Format_RGB888));
            rt.fillOrder.append(key);
            while (rt.fillOrder.size() > 16)
                rt.fills.remove(rt.fillOrder.takeFirst());
            return rt.fills.value(key);
        }
    }
    OrtSession *session = rt.session(rt.inpainter, "lama.onnx", error);
    if (!session)
        return {};

    // The model works at 512 square, with what's under the hole blanked so
    // none of what was cut out shows through.
    constexpr int Side = 512;
    const QImage small = image.scaled(Side, Side, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const QImage smallMask = mask.scaled(Side, Side, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    std::vector<float> pixels(size_t(3) * Side * Side), hole(size_t(Side) * Side);
    for (int y = 0; y < Side; ++y) {
        const uchar *line = small.constScanLine(y);
        const uchar *m = smallMask.constScanLine(y);
        for (int x = 0; x < Side; ++x) {
            const bool masked = m[x] > 0;
            hole[size_t(y) * Side + x] = masked ? 1.f : 0.f;
            for (int c = 0; c < 3; ++c)
                pixels[size_t(c) * Side * Side + size_t(y) * Side + x] = masked ? 0.f : line[x * 3 + c] / 255.f;
        }
    }
    OrtValue *imageValue = rt.tensor(pixels, {1, 3, Side, Side}, error);
    OrtValue *maskValue = rt.tensor(hole, {1, 1, Side, Side}, error);
    if (!imageValue || !maskValue) {
        for (OrtValue *value : {imageValue, maskValue})
            if (value)
                rt.api->ReleaseValue(value);
        return {};
    }
    std::vector<OrtValue *> outputs;
    if (!rt.run(session, {"image", "mask"}, {imageValue, maskValue}, {"output"}, outputs, error))
        return {};
    const float *filled = rt.data(outputs[0]);
    const std::vector<int64_t> dims = rt.shape(outputs[0]);
    QImage result;
    if (filled && dims == std::vector<int64_t>{1, 3, Side, Side}) {
        QImage out(Side, Side, QImage::Format_RGB888);
        for (int y = 0; y < Side; ++y) {
            uchar *line = out.scanLine(y);
            for (int x = 0; x < Side; ++x)
                for (int c = 0; c < 3; ++c)
                    line[x * 3 + c] = uchar(std::clamp(filled[size_t(c) * Side * Side + size_t(y) * Side + x], 0.f, 255.f));
        }
        result = out.scaled(image.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    } else if (error) {
        *error = "The fill model gave back something unexpected.";
    }
    rt.api->ReleaseValue(outputs[0]);
    if (!result.isNull()) {
        if (QDir().mkpath(cacheDirectory)) {
            result.save(cached, "PNG");
            // A few hundred fills are plenty; the oldest go.
            QDir dir(cacheDirectory);
            const QFileInfoList files = dir.entryInfoList({"*.png"}, QDir::Files, QDir::Time);
            for (int i = 300; i < files.size(); ++i)
                QFile::remove(files.at(i).absoluteFilePath());
        }
        rt.fills.insert(key, result);
        rt.fillOrder.append(key);
        while (rt.fillOrder.size() > 16)
            rt.fills.remove(rt.fillOrder.takeFirst());
    }
    return result;
}

namespace {

struct Decoded {
    QImage mask;
    float score = 0;
    qint64 area = 0;
};

}

QList<QImage> Ai::segment(const QImage &picture, const QString &key, const QList<QPointF> &points,
                          const QList<int> &labels, QString *error) {
    QMutexLocker lock(&m_mutex);
    if (!load(error))
        return {};
    Runtime &rt = *m_runtime;
    OrtSession *encoder = rt.session(rt.encoder, "mobile-sam-encoder.onnx", error);
    OrtSession *decoder = encoder ? rt.session(rt.decoder, "mobile-sam-decoder.onnx", error) : nullptr;
    if (!decoder || points.isEmpty())
        return {};
    const QImage image = picture.convertToFormat(QImage::Format_RGB888);

    // Analyses (part of) the picture at 1024 pixels on its long side, or
    // finds that analysis kept from before.
    auto encode = [&](const QRect &area, const QString &cacheKey) -> const Runtime::Encoded * {
        for (const Runtime::Encoded &kept : rt.encoded)
            if (kept.key == cacheKey)
                return &kept;
        const double scale = 1024.0 / std::max(area.width(), area.height());
        const QImage small = image.copy(area).scaled(std::max(1, int(std::lround(area.width() * scale))),
                                                     std::max(1, int(std::lround(area.height() * scale))),
                                                     Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        std::vector<float> input(size_t(3) * small.width() * small.height());
        for (int y = 0; y < small.height(); ++y) {
            const uchar *line = small.constScanLine(y);
            for (int x = 0; x < small.width() * 3; ++x)
                input[size_t(y) * small.width() * 3 + x] = line[x];
        }
        OrtValue *value = rt.tensor(input, {small.height(), small.width(), 3}, error);
        std::vector<OrtValue *> outputs;
        if (!value || !rt.run(encoder, {"input_image"}, {value}, {"image_embeddings"}, outputs, error))
            return nullptr;
        Runtime::Encoded kept;
        kept.key = cacheKey;
        kept.scale = scale;
        const float *embedding = rt.data(outputs[0]);
        const std::vector<int64_t> dims = rt.shape(outputs[0]);
        const bool expected = embedding && dims == std::vector<int64_t>{1, 256, 64, 64};
        if (expected)
            kept.embedding.assign(embedding, embedding + 256 * 64 * 64);
        rt.api->ReleaseValue(outputs[0]);
        if (!expected) {
            if (error)
                *error = "The select model gave back something unexpected.";
            return nullptr;
        }
        rt.encoded.prepend(std::move(kept));
        while (rt.encoded.size() > 3)
            rt.encoded.removeLast();
        return &rt.encoded.first();
    };

    // Masks for the points within an area of the picture, as full-size masks.
    auto decode = [&](const QRect &area, const Runtime::Encoded &kept) -> QList<Decoded> {
        std::vector<float> coords, pointLabels;
        for (int i = 0; i < points.size(); ++i) {
            coords.push_back(float((points.at(i).x() - area.x()) * kept.scale));
            coords.push_back(float((points.at(i).y() - area.y()) * kept.scale));
            pointLabels.push_back(labels.value(i, 1) > 0 ? 1.f : 0.f);
        }
        // Without a box, SAM wants a padding point.
        coords.push_back(0);
        coords.push_back(0);
        pointLabels.push_back(-1);
        std::vector<float> embedding = kept.embedding, noMask(256 * 256, 0.f), hasMask{0.f},
                           size{float(area.height()), float(area.width())};
        const int64_t count = int64_t(pointLabels.size());
        std::vector<OrtValue *> inputs{rt.tensor(embedding, {1, 256, 64, 64}, error),
                                       rt.tensor(coords, {1, count, 2}, error),
                                       rt.tensor(pointLabels, {1, count}, error),
                                       rt.tensor(noMask, {1, 1, 256, 256}, error),
                                       rt.tensor(hasMask, {1}, error),
                                       rt.tensor(size, {2}, error)};
        std::vector<OrtValue *> outputs;
        if (std::count(inputs.begin(), inputs.end(), nullptr)) {
            for (OrtValue *value : inputs)
                if (value)
                    rt.api->ReleaseValue(value);
            return {};
        }
        if (!rt.run(decoder, {"image_embeddings", "point_coords", "point_labels", "mask_input", "has_mask_input", "orig_im_size"},
                    inputs, {"masks", "iou_predictions"}, outputs, error))
            return {};
        const std::vector<int64_t> dims = rt.shape(outputs[0]);
        const float *masks = rt.data(outputs[0]), *scores = rt.data(outputs[1]);
        QList<Decoded> decoded;
        if (masks && scores && dims.size() == 4 && dims[2] == area.height() && dims[3] == area.width()) {
            for (int k = 0; k < int(dims[1]); ++k) {
                Decoded one;
                one.score = scores[k];
                one.mask = QImage(image.size(), QImage::Format_Alpha8);
                one.mask.fill(0);
                const float *plane = masks + size_t(k) * area.width() * area.height();
                for (int y = 0; y < area.height(); ++y) {
                    uchar *line = one.mask.scanLine(area.y() + y) + area.x();
                    for (int x = 0; x < area.width(); ++x)
                        if (plane[size_t(y) * area.width() + x] > 0) {
                            line[x] = 255;
                            ++one.area;
                        }
                }
                decoded.append(one);
            }
        }
        for (OrtValue *value : outputs)
            rt.api->ReleaseValue(value);
        return decoded;
    };

    const QRect whole = image.rect();
    const Runtime::Encoded *kept = encode(whole, key);
    if (!kept)
        return {};
    QList<Decoded> found = decode(whole, *kept);
    if (found.isEmpty()) {
        if (error && error->isEmpty())
            *error = "The select model gave back something unexpected.";
        return {};
    }

    // One point: SAM's three guesses (part, thing, whole) are the choices,
    // best scored first; more points: its single answer leads.
    auto ordered = [&](QList<Decoded> list) {
        if (list.size() != 4)
            return list;
        const Decoded single = list.takeFirst();
        std::stable_sort(list.begin(), list.end(), [](const Decoded &a, const Decoded &b) { return a.score > b.score; });
        if (points.size() > 1)
            list.prepend(single);
        return list;
    };
    found = ordered(found);

    // Small things get a second, close look: the picture is analysed at
    // 1024 pixels across, where a deer far away is a dozen pixels.
    QRect bounds;
    for (int y = 0; y < found.first().mask.height(); ++y) {
        const uchar *line = found.first().mask.constScanLine(y);
        for (int x = 0; x < found.first().mask.width(); ++x)
            if (line[x])
                bounds |= QRect(x, y, 1, 1);
    }
    const qint64 imageArea = qint64(image.width()) * image.height();
    if (!bounds.isEmpty() && qint64(bounds.width()) * bounds.height() < imageArea / 25) {
        const int side = std::max({bounds.width() * 3, bounds.height() * 3, std::min(image.width(), image.height()) / 6, 64});
        QRect crop(0, 0, std::min(side, image.width()), std::min(side, image.height()));
        crop.moveCenter(bounds.center());
        crop.moveLeft(std::clamp(crop.left(), 0, image.width() - crop.width()));
        crop.moveTop(std::clamp(crop.top(), 0, image.height() - crop.height()));
        bool inside = true;
        for (const QPointF &point : points)
            inside &= QRectF(crop).contains(point);
        const Runtime::Encoded *close =
            inside ? encode(crop, QString("%1#%2,%3,%4,%5").arg(key).arg(crop.x()).arg(crop.y()).arg(crop.width()).arg(crop.height()))
                   : nullptr;
        if (close) {
            const QList<Decoded> closer = ordered(decode(crop, *close));
            if (!closer.isEmpty())
                found = closer;
        }
    }

    QList<QImage> masks;
    for (const Decoded &one : found)
        if (one.area > 0)
            masks.append(one.mask);
    if (masks.isEmpty() && error)
        *error = "The AI didn't find anything there.";
    return masks;
}

#endif
