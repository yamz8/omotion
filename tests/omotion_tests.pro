QT += core gui qml quick quickcontrols2 testlib dbus multimedia
CONFIG += c++17 testcase
TARGET = omotion_tests
TEMPLATE = app

include(../src/shared.pri)

SOURCES += omotion_tests.cpp
