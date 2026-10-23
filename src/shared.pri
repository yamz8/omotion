# The sources the app and both test suites build, so a new one is added once.
INCLUDEPATH += $$PWD

HEADERS += \
    $$PWD/backend.h \
    $$PWD/engine.h \
    $$PWD/filepicker.h \
    $$PWD/scene.h \
    $$PWD/studio.h \
    $$PWD/theme.h \
    $$PWD/translator.h

SOURCES += \
    $$PWD/backend.cpp \
    $$PWD/engine.cpp \
    $$PWD/scene.cpp \
    $$PWD/studio.cpp \
    $$PWD/theme.cpp \
    $$PWD/translator.cpp

RESOURCES += $$PWD/resources.qrc
