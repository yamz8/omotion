QT += core gui qml quick quickcontrols2 dbus

CONFIG += c++17 release
TARGET = omotion
TEMPLATE = app

HEADERS += \
    src/backend.h \
    src/exporter.h \
    src/filepicker.h \
    src/portalfilepicker.h \
    src/project.h \
    src/scene.h \
    src/sceneview.h \
    src/select.h \
    src/theme.h

SOURCES += \
    src/main.cpp \
    src/backend.cpp \
    src/exporter.cpp \
    src/portalfilepicker.cpp \
    src/project.cpp \
    src/scene.cpp \
    src/sceneview.cpp \
    src/select.cpp \
    src/theme.cpp

RESOURCES += src/resources.qrc
