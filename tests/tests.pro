TEMPLATE = app
TARGET   = DarkroomTests

QT = core gui widgets

CONFIG += strict_c++ console
CONFIG -= app_bundle
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

INCLUDEPATH += \
	../app/src \
	../qtutils \
	../cpputils \
	../cpp-template-utils

HEADERS += TestHelpers.h

# The app sources under test are compiled in directly (the app is not a library). Utils.cpp is here because
# Catalog.cpp calls its path helpers; it is what pulls in the widgets dependency.
SOURCES += \
	main.cpp \
	MediaIdTests.cpp \
	PersistenceTests.cpp \
	CatalogTests.cpp \
	../app/src/Core/Catalog.cpp \
	../app/src/Core/JsonPersistence.cpp \
	../app/src/Core/Library.cpp \
	../app/src/Core/MediaId.cpp \
	../app/src/Core/MetadataStore.cpp \
	../app/src/Utils.cpp

LIBS += -L$${DESTDIR} -lcpputils
include(../cpputils/dependencies.pri)

win*{
	QMAKE_CXXFLAGS += /MP /wd4251 /Zi
	QMAKE_CXXFLAGS += /std:c++latest /permissive- /Zc:__cplusplus /FS
	QMAKE_CXXFLAGS_WARN_ON = /W4
	DEFINES += WIN32_LEAN_AND_MEAN NOMINMAX _SCL_SECURE_NO_WARNINGS

	Debug:QMAKE_LFLAGS += /DEBUG:FASTLINK /INCREMENTAL

	LIBS += -lUser32
}

linux*|freebsd{
	QT += dbus   # Utils.cpp's revealInFileManager talks to org.freedesktop.FileManager1 here
}

linux*|mac*|freebsd {
	QMAKE_CXXFLAGS_WARN_ON = -Wall -Wextra

	Release:DEFINES += NDEBUG=1
	Debug:DEFINES += _DEBUG
}
