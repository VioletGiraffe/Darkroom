TEMPLATE = app
TARGET   = Darkroom
VERSION  = 1.0.0

QMAKE_TARGET_PRODUCT     = Darkroom
QMAKE_TARGET_COMPANY     = VioletGiraffe
QMAKE_TARGET_COPYRIGHT   = Copyright (C) 2026 VioletGiraffe
QMAKE_TARGET_DESCRIPTION = Video frame extractor and organizer

QT = core gui widgets multimedia multimediawidgets svg

CONFIG += strict_c++ c++latest
CONFIG -= flat

mac* | linux* | freebsd {
	CONFIG(release, debug|release):CONFIG *= Release optimize_full
	CONFIG(debug, debug|release):CONFIG *= Debug
}

Release:OUTPUT_DIR=release/
Debug:OUTPUT_DIR=debug/

DESTDIR  = ../bin/$${OUTPUT_DIR}
OBJECTS_DIR = ../build/$${OUTPUT_DIR}/$${TARGET}
MOC_DIR     = ../build/$${OUTPUT_DIR}/$${TARGET}
UI_DIR      = ../build/$${OUTPUT_DIR}/$${TARGET}
RCC_DIR     = ../build/$${OUTPUT_DIR}/$${TARGET}

###################################################
#               INCLUDEPATH
###################################################

INCLUDEPATH += \
	src \
	../qtutils \
	../cpputils \
	../cpp-template-utils \
	../magic-alignment/src

###################################################
#                 SOURCES
###################################################

HEADERS += \
	$$files(src/*.h, true) \
	$$files(src/*.hpp, true)


SOURCES += $$files(src/*.cpp, true)

RESOURCES += res/UI/resources.qrc

###################################################
#                 LIBS
###################################################

LIBS += -L$${DESTDIR} -lqtutils -lcpputils -lmagic-alignment

###################################################
#    Platform-specific compiler options and libs
###################################################

win*{
	#LIBS += -lole32 -lShell32 -lUser32
	QMAKE_CXXFLAGS += /MP /wd4251 /Zi
	QMAKE_CXXFLAGS += /std:c++latest /permissive- /Zc:__cplusplus /FS
	QMAKE_CXXFLAGS_WARN_ON = /W4
	DEFINES += WIN32_LEAN_AND_MEAN NOMINMAX _SCL_SECURE_NO_WARNINGS

	Debug:QMAKE_LFLAGS += /DEBUG:FASTLINK /INCREMENTAL

	Release:QMAKE_CXXFLAGS += /GL
	Release:QMAKE_LFLAGS += /DEBUG:FULL /OPT:REF /OPT:ICF /TIME /LTCG:INCREMENTAL

	LIBS += -lUser32
	RC_ICONS = res/icon.ico
}

mac*{
	LIBS += -framework AppKit

	QMAKE_POST_LINK = cp -f -p $${DESTDIR}/*.dylib $${DESTDIR}/$${TARGET}.app/Contents/MacOS/ || true
}

###################################################
#      Generic stuff for Linux and Mac
###################################################

linux*|mac*|freebsd {
	QMAKE_CXXFLAGS_WARN_ON = -Wall -Wextra

	Release:DEFINES += NDEBUG=1
	Debug:DEFINES += _DEBUG
}
