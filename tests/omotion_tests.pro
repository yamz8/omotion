QT += core gui quick quickcontrols2 testlib dbus
CONFIG += c++17 testcase
TARGET = omotion_tests
TEMPLATE = app

INCLUDEPATH += ../src

HEADERS += ../src/backend.h ../src/exporter.h ../src/filepicker.h ../src/project.h ../src/scene.h ../src/select.h ../src/theme.h
SOURCES += omotion_tests.cpp ../src/backend.cpp ../src/exporter.cpp ../src/project.cpp ../src/scene.cpp ../src/select.cpp ../src/theme.cpp
