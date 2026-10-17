QT += core gui gui-private quick3d quick quickcontrols2 testlib dbus network concurrent multimedia
CONFIG += c++17 testcase
TARGET = omotion_tests
TEMPLATE = app

INCLUDEPATH += ../src

HEADERS += ../src/ai.h ../src/backend.h ../src/exporter.h ../src/filepicker.h ../src/gltf.h ../src/project.h ../src/scene.h ../src/select.h ../src/steps.h ../src/theme.h ../src/world.h ../src/worldview.h
SOURCES += omotion_tests.cpp ../src/ai.cpp ../src/backend.cpp ../src/exporter.cpp ../src/gltf.cpp ../src/project.cpp ../src/scene.cpp ../src/select.cpp ../src/steps.cpp ../src/theme.cpp ../src/world.cpp ../src/worldview.cpp

# The AI features need ONNX Runtime's headers to build; the library itself is
# loaded at run time, so omotion runs (without AI) where it isn't installed.
exists(/usr/include/onnxruntime/onnxruntime_c_api.h) {
    DEFINES += OMOTION_AI
} else {
    message("ONNX Runtime's headers aren't installed; building without AI.")
}
