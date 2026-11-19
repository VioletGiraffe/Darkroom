#include "App/Application.h"
#include "App/TestRun.h"
#include "Core/Library.h"
#include "DarkroomSettings.h"
#include "Windows/MainWindow.h"
#include "SharedSettings.h"
#include "appimage/appimagedesktopentry.h"
#include "compiler/compiler_warnings_control.h"
#include "utility/macro_utils.h"

DISABLE_COMPILER_WARNINGS
#include <QApplication>
#include <QDir>
#include <QSettings>
#include <QString>
RESTORE_COMPILER_WARNINGS

#ifndef DARKROOM_VERSION
#error "DARKROOM_VERSION is not defined; set it in Darkroom.pro"
#endif

namespace {

// The test run's settings start empty, which would open the first-run dialog
[[nodiscard]] bool createTestRunLibrary()
{
	const QString root = QDir{ TestRun::temporaryFolder() }.filePath(QStringLiteral("Library"));
	QString error;
	if (!Library{}.setRoot(root, Library::OpenMode::CreateIfAbsent, &error))
	{
		TestRun::reportFailure(QStringLiteral("could not create a library: ") + error);
		return false;
	}

	QSettings{}.setValue(Settings::RootFolder, root);
	return true;
}

} // namespace

int main(int argc, char* argv[])
{
	const Application app{ argc, argv, DarkroomApplicationName, STRINGIFY_EXPANDED_ARGUMENT(DARKROOM_VERSION), ":/darkroom.svg" };
	if (app.isTestRun() && !createTestRunLibrary())
		return 1;

	// Destroyed before app stops the pools, so its state is saved before they wait out a read stuck on unreachable storage
	MainWindow window;
	if (!window.isLibraryLoaded())
		return 0;

	window.show();
	if (!app.isTestRun())
		AppImageDesktopEntry::offerOnce(&window);
	return QApplication::exec();
}
