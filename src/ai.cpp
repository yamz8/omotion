#include "ai.h"

#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTextStream>
#include <QFile>
#include <QFileInfo>
#include <QLibrary>
#include <QMutexLocker>
#include <QRect>
#include <QStandardPaths>
#include <QThread>
#include <algorithm>
#include <cmath>
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
    };
    return list;
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
        for (OrtSession *session : {depth, encoder, decoder})
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
    return true;
}

bool Ai::runtimeLoaded() {
    QMutexLocker lock(&m_mutex);
    return load(nullptr);
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
        if (std::count(inputs.begin(), inputs.end(), nullptr) ||
            !rt.run(decoder, {"image_embeddings", "point_coords", "point_labels", "mask_input", "has_mask_input", "orig_im_size"},
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
