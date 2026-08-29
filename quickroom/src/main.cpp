#include "BrowserWindow.h"
#include "Core/CpuThreadPool.h"
#include "Core/IoThreadPool.h"
#include "Theme/Style.h"
#include "Utils.h"
#include "Windows/ImageViewerWindow.h"
#include "Windows/VideoPlayerWindow.h"
#include "crashhandler/CCrashHandler.h"
#include "compiler/compiler_warnings_control.h"
#include "logger/cloggerinmemory.h"
#include "utility/macro_utils.h"
#include "utils/naturalsorting/cnaturalsorterqcollator.h"

DISABLE_COMPILER_WARNINGS
#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QImageReader>
#include <QString>
#include <QStringList>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <memory>
#include <utility>

#ifndef QUICKROOM_VERSION
#error "QUICKROOM_VERSION is not defined; set it in quickroom.pro"
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

// Full paths of the images directly in folder, ordered as the browser lists them.
QStringList imageFilesInFolder(const QString& folder)
{
	const QDir dir(folder);
	QStringList names;
	for (const QString& name : dir.entryList(QDir::Files, QDir::Unsorted))
		if (isSupportedImageFile(name))
			names.push_back(name);

	std::sort(names.begin(), names.end(), NaturalSort::lessCaseInsensitive);

	QStringList paths;
	paths.reserve(names.size());
	for (const QString& name : std::as_const(names))
		paths.push_back(dir.absoluteFilePath(name));
	return paths;
}

// Opens the viewer on path, browsing the images beside it. False when the image cannot be shown at all.
bool showImageViewer(const QString& path)
{
	QStringList images = imageFilesInFolder(QFileInfo(path).absolutePath());

	// pathComparisonKey, not ==: the command line may spell the path with different case or separators.
	const QString startKey = pathComparisonKey(path);
	int index = -1;
	for (qsizetype i = 0; i < images.size() && index < 0; ++i)
		if (pathComparisonKey(images.at(i)) == startKey)
			index = static_cast<int>(i);

	if (index < 0) // gone from the folder since it was listed: show it alone
	{
		images = { path };
		index = 0;
	}

	const auto currentPath = std::make_shared<QString>(images.at(index));
	ImageViewerWindow* const viewer = ImageViewerWindow::showForImages(nullptr, images, index, nullptr,
		[currentPath, images](int browsedIndex) { *currentPath = images.at(browsedIndex); });
	if (!viewer)
		return false;

	// No browser exists yet in this mode, so leaving fullscreen opens one on the image being viewed.
	viewer->setExitFullScreenHandler([viewer, currentPath] {
		// The browser opens before the viewer closes: closing the only window would quit the app.
		BrowserWindow::showForFolder(QFileInfo(*currentPath).absolutePath(), *currentPath);
		viewer->close();
		return false;   // do not un-fullscreen a closing window
	});
	return true;
}

// Opens the window the command line asks for. Always leaves one open: an unusable path falls back to the
// browser, which resolves what to show.
void openStartupWindow(const QString& path)
{
	if (path.isEmpty())
	{
		BrowserWindow::showForFolder();
		return;
	}

	const QFileInfo info(path);
	if (info.isDir())
	{
		BrowserWindow::showForFolder(info.absoluteFilePath());
		return;
	}

	if (info.isFile())
	{
		if (isSupportedImageFile(path) && showImageViewer(info.absoluteFilePath()))
			return;
		if (isSupportedVideoFile(path))
		{
			VideoPlayerWindow::createPlayerWindow(nullptr, info.absoluteFilePath(), nullptr);
			return;
		}
	}
	else
		reportMissingFile(nullptr, path);

	// An unsupported file leaves the browser on its folder, where that file is simply not listed.
	BrowserWindow::showForFolder(info.absolutePath());
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
	app.setApplicationName("Quickroom");
	app.setApplicationVersion(STRINGIFY_EXPANDED_ARGUMENT(QUICKROOM_VERSION));
	app.setWindowIcon(QIcon(":/quickroom.svg"));

	CCrashHandler::setMinidumpsStorageFolderPath(QDir::tempPath().toStdString());
	CCrashHandler crashHandler([](const wchar_t* msg) {
		qInfo() << QString::fromWCharArray(msg);
	});

	QImageReader::setAllocationLimit(2048);  // raise Qt's 256 MB decode cap; 67 MP at 3x8 bits already exceeds it

	Style::install();

	// Several paths: the first one wins.
	openStartupWindow(QDir::fromNativeSeparators(app.arguments().value(1)));

	const int result = app.exec();
	// I/O first: a read completing there hands its decode to the compute pool.
	IoThreadPool::finishAllThreads();
	cpuThreadPool().finishAllThreads();
	return result;
}
