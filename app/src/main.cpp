#include "Windows/MainWindow.h"
#include "Settings.h"
#include "Theme/Style.h"
#include "crashhandler/CCrashHandler.h"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QSettings>
#include <QStyleHints>

int main(int argc, char* argv[])
{
	QApplication app(argc, argv);
	app.setOrganizationName("VioletGiraffe");
	app.setApplicationName("Darkroom");

	CCrashHandler::setMinidumpsStorageFolderPath(QDir::tempPath().toStdString());
	CCrashHandler crashHandler([](const wchar_t* msg) {
		qInfo() << QString::fromWCharArray(msg);
	});

	const int scheme = QSettings{}.value(Settings::ColorScheme, Defaults::ColorScheme).toInt();
	app.styleHints()->setColorScheme(static_cast<Qt::ColorScheme>(scheme));

	Style::install();  // app-wide non-stock chrome; re-applies on a light/dark switch

	MainWindow window;
	window.show();

	return app.exec();
}
