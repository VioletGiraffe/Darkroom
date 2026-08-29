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
	../app/src \
	../qtutils \
	../cpputils \
	../cpp-template-utils \
	../image-processing

###################################################
#                 SOURCES
###################################################

# Library.h is the only shared header with Q_OBJECT, so it alone needs listing for moc:
# its metaobject must link even though Quickroom never creates a Library.
HEADERS += \
	$$files(src/*.h, true) \
	../app/src/Core/Library.h

# Shared app sources are compiled in directly (the app is not a library).
# Wholly shared directories are globbed; the rest is the hand-picked closure of the reused
# viewer and player windows. A shared file gaining an app-only include breaks this link
# while the app still builds - same failure mode as tests.pro.
SOURCES += \
	$$files(src/*.cpp, true) \
	$$files(../app/src/Core/*.cpp) \
	$$files(../app/src/Theme/*.cpp) \
	$$files(../app/src/crashhandler/*.cpp) \
	../app/src/Utils.cpp \
	../app/src/Ffmpeg.cpp \
	../app/src/Import.cpp \
	../app/src/Windows/ImageViewerWindow.cpp \
	../app/src/Windows/VideoPlayerWindow.cpp \
	../app/src/Windows/OscillatingPlayback.cpp \
	../app/src/Windows/SingleFrameExtraction.cpp \
	../app/src/UiComponents/ThumbnailWidget.cpp \
	../app/src/UiComponents/MediaGrid.cpp \
	../app/src/UiComponents/DragGestureHelper.cpp \
	../app/src/UiComponents/MarkerSlider.cpp

# Theme and the shared windows load icons from the app's resource file.
RESOURCES += ../app/res/resources.qrc

###################################################
#                 LIBS
###################################################

LIBS += -L$${DESTDIR} -lqtutils -lcpputils -limage-processing
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
	# Darkroom's icon until Quickroom has its own.
	RC_ICONS = ../app/res/icon.ico
}

mac*{
	LIBS += -framework AppKit
}

linux*|freebsd{
	QT += dbus   # revealInFileManager() talks to org.freedesktop.FileManager1 here (Utils.cpp)
}

###################################################
#      Generic stuff for Linux and Mac
###################################################

linux*|mac*|freebsd {
	QMAKE_CXXFLAGS_WARN_ON = -Wall -Wextra

	Release:DEFINES += NDEBUG=1
	Debug:DEFINES += _DEBUG
}
