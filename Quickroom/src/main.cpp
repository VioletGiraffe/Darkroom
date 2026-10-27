#include "BrowserWindow.h"
#include "Core/CpuThreadPool.h"
#include "Core/IoThreadPool.h"
#include "FileOperations.h"
#include "FolderListing.h"
#include "MemoryLog.h"
#include "SharedSettings.h"
#include "Theme/Style.h"
#include "UiComponents/SortControl.h"
#include "Utils.h"
#include "Windows/ImageViewerWindow.h"
#include "Windows/VideoPlayerWindow.h"
#include "crashhandler/CCrashHandler.h"
#include "compiler/compiler_warnings_control.h"
#include "utility/macro_utils.h"

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
#include <utility>
#include <vector>

#ifndef QUICKROOM_VERSION
#error "QUICKROOM_VERSION is not defined; set it in Quickroom.pro"
#endif

namespace {

// Full paths of the files of kind directly in folder, ordered as the browser lists them.
QStringList filesInFolder(const QString& folder, EntryKind kind)
{
	std::vector<FolderEntry> entries = listFolder(folder).value_or(std::vector<FolderEntry>{});
	std::erase_if(entries, [kind](const FolderEntry& entry) { return entry.kind != kind; });

	const SortOrder order = SortControl::savedOrder(BROWSER_SORT_SETTINGS_GROUP, {}, BROWSER_SORT_OPTIONS);
	std::ranges::sort(entries, [&order](const FolderEntry& a, const FolderEntry& b) { return precedesInOrder(a, b, order); });

	QStringList paths;
	paths.reserve(static_cast<qsizetype>(entries.size()));
	for (const FolderEntry& entry : entries)
		paths.push_back(entry.path);
	return paths;
}

// Opens the viewer on path, browsing the images beside it. False when the image cannot be shown at all.
bool showImageViewer(const QString& path)
{
	QStringList images = filesInFolder(QFileInfo(path).absolutePath(), EntryKind::Image);

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

	ImageViewerWindow* const viewer = ImageViewerWindow::showForImages(std::move(images), index, nullptr);
	if (!viewer)
		return false;

	FileOperations::installDeleteActions(*viewer);

	// No browser exists yet in this mode, so leaving fullscreen opens one on the image being viewed.
	viewer->setExitFullScreenHandler([viewer] {
		// The browser opens before the viewer closes: closing the only window would quit the app.
		const QString currentPath = viewer->currentImagePath();
		BrowserWindow::showForFolder(QFileInfo(currentPath).absolutePath(), currentPath);
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
		BrowserWindow::showForFolder(info.absoluteFilePath(), {}, BrowserWindow::PathFieldHistory::Record);
		return;
	}

	if (info.isFile())
	{
		if (isSupportedImageFile(path) && showImageViewer(info.absoluteFilePath()))
			return;
		if (isSupportedVideoFile(path))
		{
			VideoPlayerWindow* const player = VideoPlayerWindow::createPlayerWindow(info.absoluteFilePath(), nullptr);
			FileOperations::installDeleteActions(*player);
			player->setNavigation(filesInFolder(info.absolutePath(), EntryKind::Video), {});
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
	installMemoryLog();

	Q_INIT_RESOURCE(shared);

	QApplication app(argc, argv);
	app.setOrganizationName("VioletGiraffe");
	app.setApplicationName(QuickroomApplicationName);
	app.setApplicationVersion(STRINGIFY_EXPANDED_ARGUMENT(QUICKROOM_VERSION));
	app.setWindowIcon(QIcon(":/quickroom.svg"));

	CCrashHandler::setMinidumpsStorageFolderPath(QDir::tempPath().toStdString());
	CCrashHandler crashHandler([](const wchar_t* msg) {
		qInfo() << QString::fromWCharArray(msg);
	});

	QImageReader::setAllocationLimit(2048);  // raise Qt's 256 MB decode cap; 67 MP at 3x8 bits already exceeds it

	initSharedSettings();
	Style::install();

	// Take the first argument as a path to open, if any
	QString startupPath = app.arguments().value(1, /*default:*/{});
#ifdef Q_OS_WIN
	// Windows argv parsing reads \" as an escaped quote: a quoted drive root "C:\" arrives as C:"
	if (startupPath.endsWith('"'))
		startupPath.back() = '\\';
#endif
	openStartupWindow(QDir::fromNativeSeparators(startupPath));

	const int result = app.exec();
	// Image reads transition from the IO pool to the CPU pool, so IO has to be shut down first
	IoThreadPool::finishAllThreads();
	cpuThreadPool().finishAllThreads();
	return result;
}
