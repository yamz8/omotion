QT += core gui gui-private quick3d qml quick quickcontrols2 dbus network concurrent multimedia

CONFIG += c++17 release
TARGET = omotion
TEMPLATE = app

HEADERS += \
    src/ai.h \
    src/backend.h \
    src/exporter.h \
    src/filepicker.h \
    src/portalfilepicker.h \
    src/project.h \
    src/scene.h \
    src/sceneview.h \
    src/select.h \
    src/steps.h \
    src/theme.h \
    src/world.h \
    src/worldview.h

SOURCES += \
    src/main.cpp \
    src/ai.cpp \
    src/backend.cpp \
    src/exporter.cpp \
    src/portalfilepicker.cpp \
    src/project.cpp \
    src/scene.cpp \
    src/sceneview.cpp \
    src/select.cpp \
    src/steps.cpp \
    src/theme.cpp \
    src/world.cpp \
    src/worldview.cpp

RESOURCES += src/resources.qrc

# The AI features need ONNX Runtime's headers to build; the library itself is
# loaded at run time, so omotion runs (without AI) where it isn't installed.
exists(/usr/include/onnxruntime/onnxruntime_c_api.h) {
    DEFINES += OMOTION_AI
} else {
    message("ONNX Runtime's headers aren't installed; building without AI.")
}
