#include "App/Application.h"
#include "App/MemoryLog.h"
#include "Core/CpuThreadPool.h"
#include "Core/IoThreadPool.h"
#include "SharedSettings.h"
#include "Theme/Style.h"
#include "system/cpuinfo.hpp"
#include "threading/thread_helpers.h"
#include "widgets/widgetutils.h"

DISABLE_COMPILER_WARNINGS
#include <QDebug>
#include <QDir>
#include <QIcon>
#include <QImageReader>
RESTORE_COMPILER_WARNINGS

Application::EarlySetup::EarlySetup()
{
	installMemoryLog();
	Q_INIT_RESOURCE(shared);
	WidgetUtils::tagWindowsAsSrgb();
	CCrashHandler::setMinidumpsStorageFolderPath(QDir::tempPath().toStdWString());
}

Application::Application(int& argc, char** argv, const QString& name, const QString& version, const QString& iconPath) :
	_app{ argc, argv },
	_crashHandler{ [](const wchar_t* msg) { qInfo() << QString::fromWCharArray(msg); } }
{
	QApplication::setOrganizationName("VioletGiraffe");
	QApplication::setApplicationName(name);
	QApplication::setApplicationVersion(version);
	QApplication::setWindowIcon(QIcon{ iconPath });

	const CpuCount cpuCount = CpuCount::get();
	qInfo().nospace().noquote() << name << ' ' << version << ", CPU: " << QString::fromStdString(cpuModelName()) << ", "
		<< cpuCount.physicalCoreCount() << " cores / " << cpuCount.logicalProcessorCount() << " threads";

	QImageReader::setAllocationLimit(2048);  // raise Qt's 256 MB decode cap; 67 MP at 3x8 bits already exceeds it

	initSharedSettings();
	Style::install();
}

Application::~Application()
{
	// Image reads transition from the IO pool to the CPU pool, so IO has to be shut down first
	IoThreadPool::finishAllThreads();
	cpuThreadPool().finishAllThreads();
}
