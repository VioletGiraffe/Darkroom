#include "Core/IoThreadPool.h"
#include "Windows/MainWindow.h"
#include "Theme/Style.h"
#include "crashhandler/CCrashHandler.h"
#include "compiler/compiler_warnings_control.h"
#include "logger/cloggerinmemory.h"
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
#error "DARKROOM_VERSION is not defined; set it in app.pro"
#endif

namespace {

QString formatQtMessage(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
	const char* level = "Info";
	switch (type)
	{
		case QtDebugMsg:    level = "Debug";    break;
		case QtInfoMsg:     level = "Info";     break;
		case QtWarningMsg:  level = "Warning";  break;
		case QtCriticalMsg: level = "Critical"; break;
		case QtFatalMsg:    level = "Fatal";    break;
	}

	QString text = QStringLiteral("[%1] %2").arg(QLatin1String(level), message);
	if (context.file)
		text += QStringLiteral(" (%1:%2)").arg(QLatin1String(context.file)).arg(context.line);
	return text;
}

QtMessageHandler g_previousMessageHandler = nullptr;

void memoryLogMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
	loggerInstance<CLoggerInMemory>().log(formatQtMessage(type, context, message));
	if (g_previousMessageHandler)
		g_previousMessageHandler(type, context, message);
}

} // namespace

int main(int argc, char* argv[])
{
	// Installed before QApplication so plugin/platform diagnostics emitted during its construction are captured too.
	g_previousMessageHandler = qInstallMessageHandler(memoryLogMessageHandler);

	AdvancedAssert::setLoggingFunc([](const char* msg) {
		::memoryLogMessageHandler(QtWarningMsg, QMessageLogContext{}, msg);
	});

	QApplication app(argc, argv);
	app.setOrganizationName("VioletGiraffe");
	app.setApplicationName("Darkroom");
	app.setApplicationVersion(STRINGIFY_EXPANDED_ARGUMENT(DARKROOM_VERSION));
	app.setWindowIcon(QIcon(":/icon.svg"));

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
	IoThreadPool::finishAllThreads();
	return result;
}
