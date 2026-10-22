QT += core gui qml quick quickcontrols2 dbus multimedia

CONFIG += c++17 release
TARGET = omotion
TEMPLATE = app

HEADERS += \
    src/backend.h \
    src/cli.h \
    src/filepicker.h \
    src/portalfilepicker.h \
    src/scene.h \
    src/theme.h \
    src/translator.h

SOURCES += \
    src/main.cpp \
    src/backend.cpp \
    src/cli.cpp \
    src/portalfilepicker.cpp \
    src/scene.cpp \
    src/theme.cpp \
    src/translator.cpp

RESOURCES += src/resources.qrc
