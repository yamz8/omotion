QT += core gui qml quick quickcontrols2 testlib dbus multimedia
CONFIG += c++17 testcase
TARGET = omotion_tests
TEMPLATE = app

INCLUDEPATH += ../src

HEADERS += ../src/backend.h ../src/filepicker.h ../src/scene.h ../src/theme.h ../src/translator.h
SOURCES += omotion_tests.cpp ../src/backend.cpp ../src/scene.cpp ../src/theme.cpp ../src/translator.cpp
RESOURCES += ../src/resources.qrc
