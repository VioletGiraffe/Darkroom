#include "Core/IoThreadPool.h"
#include "Windows/MainWindow.h"
#include "Settings.h"
#include "Theme/Style.h"
#include "crashhandler/CCrashHandler.h"
#include "utility/macro_utils.h"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QIcon>
#include <QImageReader>
#include <QSettings>
#include <QStyleHints>

#ifndef DARKROOM_VERSION
#error "DARKROOM_VERSION is not defined; set it in app.pro"
#endif

int main(int argc, char* argv[])
{
	QApplication app(argc, argv);
	app.setOrganizationName("VioletGiraffe");
	app.setApplicationName("Darkroom");
	app.setApplicationVersion(STRINGIFY_EXPANDED_ARGUMENT(DARKROOM_VERSION));  // from VERSION in app.pro; surfaced in Help > About
	app.setWindowIcon(QIcon(":/icon.svg"));  // default window icon; Windows rides on RC_ICONS, this is for Linux/macOS

	CCrashHandler::setMinidumpsStorageFolderPath(QDir::tempPath().toStdString());
	CCrashHandler crashHandler([](const wchar_t* msg) {
		qInfo() << QString::fromWCharArray(msg);
	});

	QImageReader::setAllocationLimit(2048);  // raise Qt's 256 MB decode cap; 67 MP at 3x8 bits already exceeds it

	const int scheme = QSettings{}.value(Settings::ColorScheme, Defaults::ColorScheme).toInt();
	app.styleHints()->setColorScheme(static_cast<Qt::ColorScheme>(scheme));

	Style::install();  // app-wide non-stock style; re-applies on a light/dark switch

	MainWindow window;
	if (!window.isLibraryLoaded())
		return 0;  // the user cancelled the library picker; the window was never built
	window.show();

	const int result = app.exec();
	IoThreadPool::finishAllThreads();
	return result;
}
