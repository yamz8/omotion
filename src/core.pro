# The code the app and both test suites share, compiled once into a static
# library that each of them links.
QT += core gui qml quick quickcontrols2 dbus multimedia
CONFIG += c++17 release staticlib
TARGET = omotioncore
TEMPLATE = lib

HEADERS += \
    backend.h \
    engine.h \
    filepicker.h \
    measure.h \
    review.h \
    scene.h \
    studio.h \
    theme.h \
    translator.h

SOURCES += \
    backend.cpp \
    engine.cpp \
    measure.cpp \
    review.cpp \
    scene.cpp \
    studio.cpp \
    theme.cpp \
    translator.cpp
