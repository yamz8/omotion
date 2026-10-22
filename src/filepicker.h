#pragma once

#include <QObject>
#include <QUrl>

// Where open and save dialogs come from, so tests can stand in for the portal.
class FilePicker : public QObject {
    Q_OBJECT

public:
    explicit FilePicker(QObject *parent = nullptr) : QObject(parent) {}
    virtual ~FilePicker() = default;

    virtual void openScene(const QUrl &folder) = 0;
    // suggestedUrl is the folder to create; the name chosen becomes the film's folder.
    virtual void newFilm(const QUrl &suggestedUrl) = 0;

signals:
    void openSelected(const QUrl &url);
    void newSelected(const QUrl &url);
    void failed(const QString &message);
};
