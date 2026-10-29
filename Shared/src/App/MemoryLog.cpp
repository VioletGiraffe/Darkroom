#include "App/MemoryLog.h"
#include "assert/advanced_assert.h"
#include "compiler/compiler_warnings_control.h"
#include "logger/cloggerinmemory.h"
#include "timing/profiler.h"

DISABLE_COMPILER_WARNINGS
#include <QString>
#include <QtLogging>
RESTORE_COMPILER_WARNINGS

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

void installMemoryLog()
{
	g_previousMessageHandler = qInstallMessageHandler(memoryLogMessageHandler);

	AdvancedAssert::setLoggingFunc([](const char* msg) {
		::memoryLogMessageHandler(QtWarningMsg, QMessageLogContext{}, msg);
	});

#ifdef PROFILING_ENABLED
	Profiler::setLoggingFunc([](const char* msg) {
		::memoryLogMessageHandler(QtInfoMsg, QMessageLogContext{}, msg);
	});
#endif
}
