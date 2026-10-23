QT += core gui qml quick quickcontrols2 testlib dbus multimedia
CONFIG += c++17 testcase
TARGET = app_tests
TEMPLATE = app

include(../src/shared.pri)

SOURCES += app_tests.cpp
