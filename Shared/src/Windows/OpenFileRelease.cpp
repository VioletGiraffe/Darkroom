#include "Windows/OpenFileRelease.h"
#include "Windows/ImageViewerWindow.h"
#include "Windows/VideoPlayerWindow.h"
#include "compiler/compiler_warnings_control.h"
#include "fileoperations/centrypath.h"
#include "fileoperations/inlinerename.h"

DISABLE_COMPILER_WARNINGS
#include <QDir>
#include <QFile>
#include <QFileInfo>
RESTORE_COMPILER_WARNINGS

void releaseMediaFilesHeldOpen(const QStringList& paths)
{
	const auto isAtOrUnderAPath = [&paths](const QString& filePath) { return isPathAtOrUnder(filePath, paths); };
	VideoPlayerWindow::releaseFilesMatching(isAtOrUnderAPath);
	ImageViewerWindow::releaseFilesMatching(isAtOrUnderAPath);
}

void followMovedMediaFiles(const QStringList& releasedPaths, const std::vector<PathMove>& completedMoves)
{
	const auto wasReleased = [&releasedPaths](const QString& filePath) { return isPathAtOrUnder(filePath, releasedPaths); };
	const auto newLocation = [&completedMoves](const QString& path) { return pathAfterMoves(path, completedMoves); };
	VideoPlayerWindow::followMovedFiles(wasReleased, newLocation);
	ImageViewerWindow::followMovedFiles(wasReleased, newLocation);
}

InlineRenameResult renameEntryReleasingOpenFiles(const CEntryPath& source, const QString& newName, const InlineRenameReplacement fileReplacement)
{
	const QString& path = source.value();
	const QString newPath = QFileInfo(path).dir().filePath(newName);
	QStringList released{ path };
	if (fileReplacement == InlineRenameReplacement::Replace)
		released.push_back(newPath);
	releaseMediaFilesHeldOpen(released);

	InlineRenameResult result = inlineRename(source, newName, fileReplacement);
	followMovedMediaFiles(released, result.status == InlineRenameStatus::Renamed ? std::vector<PathMove>{ { path, newPath } } : std::vector<PathMove>{});
	return result;
}

bool moveReleasingOpenFiles(const QString& from, const QString& to)
{
	releaseMediaFilesHeldOpen({ from });
	const bool renamed = QFile::rename(from, to);
	followMovedMediaFiles({ from }, renamed ? std::vector<PathMove>{ { from, to } } : std::vector<PathMove>{});
	return renamed;
}
