QT += core gui qml quick quickcontrols2 dbus multimedia

CONFIG += c++17 release
TARGET = omotion
TEMPLATE = app

include(src/shared.pri)

HEADERS += \
    src/cli.h \
    src/portalfilepicker.h

SOURCES += \
    src/main.cpp \
    src/cli.cpp \
    src/portalfilepicker.cpp
