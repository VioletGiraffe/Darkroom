#include "App/Application.h"
#include "Windows/MainWindow.h"
#include "SharedSettings.h"
#include "compiler/compiler_warnings_control.h"
#include "utility/macro_utils.h"

DISABLE_COMPILER_WARNINGS
#include <QApplication>
RESTORE_COMPILER_WARNINGS

#ifndef DARKROOM_VERSION
#error "DARKROOM_VERSION is not defined; set it in Darkroom.pro"
#endif

int main(int argc, char* argv[])
{
	const Application app{ argc, argv, DarkroomApplicationName, STRINGIFY_EXPANDED_ARGUMENT(DARKROOM_VERSION), ":/darkroom.svg" };

	// Destroyed before app stops the pools, so its state is saved before they wait out a read stuck on unreachable storage
	MainWindow window;
	if (!window.isLibraryLoaded())
		return 0;

	window.show();
	return QApplication::exec();
}
