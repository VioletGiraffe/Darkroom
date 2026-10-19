# Code used by both Darkroom and Quickroom, compiled once for both.
TEMPLATE = lib
TARGET   = shared
CONFIG += staticlib

QT = core gui widgets multimedia multimediawidgets svg
QT += core-private   # Theme/Style.cpp includes qtutils theme/cthemeiconhandler.h (private QAbstractFileEngineHandler)

CONFIG += strict_c++
CONFIG -= flat

mac* | linux* | freebsd {
	CONFIG(release, debug|release):CONFIG *= Release optimize_full
	CONFIG(debug, debug|release):CONFIG *= Debug
}

exists(../global.pri){
	include(../global.pri)
} else {
	CONFIG += c++2b
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
	../image-processing

###################################################
#                 SOURCES
###################################################

HEADERS += \
	$$files(src/*.h, true) \
	$$files(src/*.hpp, true)


SOURCES += $$files(src/*.cpp, true)

# A static library's resources need Q_INIT_RESOURCE(shared) in each app's main().
RESOURCES += res/shared.qrc

###################################################
#    Platform-specific compiler options
###################################################

win*{
	QMAKE_CXXFLAGS += /MP /wd4251
	QMAKE_CXXFLAGS += /std:c++latest /permissive- /Zc:__cplusplus /FS
	QMAKE_CXXFLAGS_WARN_ON = /W4
	DEFINES += WIN32_LEAN_AND_MEAN NOMINMAX _SCL_SECURE_NO_WARNINGS
}

linux*|freebsd{
	QT += dbus   # revealInFileManager() talks to org.freedesktop.FileManager1 here (Utils.cpp)
}

###################################################
#      Generic stuff for Linux and Mac
###################################################

linux*|mac*|freebsd {
	QMAKE_CXXFLAGS_WARN_ON = -Wall -Wextra -Wno-missing-field-initializers # omitted aggregate fields take their defaults by design

	Release:DEFINES += NDEBUG=1
	Debug:DEFINES += _DEBUG
}
