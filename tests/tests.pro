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
	../Darkroom/src \
	../Shared/src \
	../qtutils \
	../cpputils \
	../cpp-template-utils

HEADERS += \
	TestHelpers.h \
	../Darkroom/src/Core/Library.h

# The Darkroom sources under test are compiled in: they belong to the app, not to a library.
SOURCES += \
	main.cpp \
	PersistenceTests.cpp \
	CatalogTests.cpp \
	CatalogIntegrityTests.cpp \
	UtilsTests.cpp \
	OscillationCurveTests.cpp \
	MJpegParserTests.cpp \
	../Darkroom/src/Core/Catalog.cpp \
	../Darkroom/src/Core/CatalogIntegrity.cpp \
	../Darkroom/src/Core/Library.cpp \
	../Darkroom/src/Core/MetadataStore.cpp

# GNU ld resolves left to right: shared before the libraries it uses, qtutils before cpputils
LIBS += -L$${DESTDIR} -lshared -lqtutils -lcpputils
# Unix only: MSBuild tracks link inputs itself
mac*|linux*|freebsd: PRE_TARGETDEPS += $${DESTDIR}/libshared.a $${DESTDIR}/libqtutils.a $${DESTDIR}/libcpputils.a
include(../cpputils/dependencies.pri)

win*{
	QMAKE_CXXFLAGS += /MP /wd4251
	QMAKE_CXXFLAGS += /std:c++latest /permissive- /Zc:__cplusplus /FS
	QMAKE_CXXFLAGS_WARN_ON = /W4
	DEFINES += WIN32_LEAN_AND_MEAN NOMINMAX _SCL_SECURE_NO_WARNINGS

	Debug:QMAKE_LFLAGS += /DEBUG:FASTLINK /INCREMENTAL

	LIBS += -lUser32
}

linux*|mac*|freebsd {
	QMAKE_CXXFLAGS_WARN_ON = -Wall -Wextra -Wno-missing-field-initializers # omitted aggregate fields take their defaults by design

	Release:DEFINES += NDEBUG=1
	Debug:DEFINES += _DEBUG
}
