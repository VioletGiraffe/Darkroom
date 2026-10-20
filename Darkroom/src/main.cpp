#include "Core/CpuThreadPool.h"
#include "Core/IoThreadPool.h"
#include "Windows/MainWindow.h"
#include "MemoryLog.h"
#include "SharedSettings.h"
#include "Theme/Style.h"
#include "crashhandler/CCrashHandler.h"
#include "compiler/compiler_warnings_control.h"
#include "utility/macro_utils.h"

DISABLE_COMPILER_WARNINGS
#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QIcon>
#include <QImageReader>
#include <QString>
RESTORE_COMPILER_WARNINGS

#ifndef DARKROOM_VERSION
#error "DARKROOM_VERSION is not defined; set it in Darkroom.pro"
#endif

int main(int argc, char* argv[])
{
	installMemoryLog();

	Q_INIT_RESOURCE(shared);

	QApplication app(argc, argv);
	app.setOrganizationName("VioletGiraffe");
	app.setApplicationName(DarkroomApplicationName);
	app.setApplicationVersion(STRINGIFY_EXPANDED_ARGUMENT(DARKROOM_VERSION));
	app.setWindowIcon(QIcon(":/darkroom.svg"));

	CCrashHandler::setMinidumpsStorageFolderPath(QDir::tempPath().toStdString());
	CCrashHandler crashHandler([](const wchar_t* msg) {
		qInfo() << QString::fromWCharArray(msg);
	});

	QImageReader::setAllocationLimit(2048);  // raise Qt's 256 MB decode cap; 67 MP at 3x8 bits already exceeds it

	Style::install();

	MainWindow window;
	if (!window.isLibraryLoaded())
		return 0;
	window.show();

	const int result = app.exec();
	// Image reads transition from the IO pool to the CPU pool, so IO has to be shut down first
	IoThreadPool::finishAllThreads();
	cpuThreadPool().finishAllThreads();
	return result;
}
