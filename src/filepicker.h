#pragma once

#include <QObject>
#include <QUrl>

// What the backend asks of a file chooser, so tests can hand it a fake.
class FilePicker : public QObject {
    Q_OBJECT

public:
    enum class Purpose { OpenImage, OpenFolder, ImportImage, ImportAudio };

    explicit FilePicker(QObject *parent = nullptr) : QObject(parent) {}
    virtual ~FilePicker() = default;

    virtual void choose(Purpose purpose) = 0;

signals:
    void selected(FilePicker::Purpose purpose, const QUrl &url);
    void failed(const QString &message);
};
