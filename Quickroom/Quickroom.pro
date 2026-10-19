TEMPLATE = app
TARGET   = Quickroom
VERSION  = 0.1.0
# Surface the version to C++ as a bare token (stringized in main.cpp -> QApplication::applicationVersion()).
DEFINES += QUICKROOM_VERSION=$$VERSION

!versionAtLeast(QT_VERSION, 6.8.0): error("Quickroom requires Qt 6.8 or newer.")

QMAKE_TARGET_PRODUCT     = Quickroom
QMAKE_TARGET_COMPANY     = VioletGiraffe
QMAKE_TARGET_COPYRIGHT   = Copyright (C) 2026 VioletGiraffe

QT = core gui widgets multimedia multimediawidgets svg

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
	../Shared/src \
	../qtutils \
	../cpputils \
	../cpp-template-utils \
	../file-operations \
	../image-processing \
	../thin_io/src

###################################################
#                 SOURCES
###################################################

HEADERS += $$files(src/*.h, true)

SOURCES += $$files(src/*.cpp, true)

# The copy/move engine and its dialogs; not config.pri, which would reset QT and the build settings.
include(../file-operations/fileoperations/fileoperations.pri)
include(../file-operations/fileoperationsui/fileoperationsui.pri)

RESOURCES += res/quickroom.qrc

###################################################
#                 LIBS
###################################################

# GNU ld resolves left to right: shared before the libraries it uses
LIBS += -L$${DESTDIR} -lshared -lqtutils -lcpputils -limage-processing -lthin_io
include(../cpputils/dependencies.pri)

###################################################
#    Platform-specific compiler options and libs
###################################################

win*{
	QMAKE_CXXFLAGS += /MP /wd4251
	QMAKE_CXXFLAGS += /std:c++latest /permissive- /Zc:__cplusplus /FS
	QMAKE_CXXFLAGS_WARN_ON = /W4
	DEFINES += WIN32_LEAN_AND_MEAN NOMINMAX _SCL_SECURE_NO_WARNINGS

	Debug:QMAKE_LFLAGS += /DEBUG:FASTLINK /INCREMENTAL

	LIBS += -lUser32
	RC_ICONS = res/quickroom.ico
}

mac*{
	LIBS += -framework AppKit
	ICON = res/quickroom.icns
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
