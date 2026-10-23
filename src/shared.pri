# Links the shared code, which bin/build compiles once into build/core, and
# compiles the resources, which a static library can't carry on its own.
INCLUDEPATH += $$PWD
LIBS += -L$$PWD/../build/core -lomotioncore
PRE_TARGETDEPS += $$PWD/../build/core/libomotioncore.a

RESOURCES += $$PWD/resources.qrc
