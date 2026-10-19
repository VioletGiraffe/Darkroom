#include "Windows/OpenFileRelease.h"
#include "Windows/ImageViewerWindow.h"
#include "Windows/VideoPlayerWindow.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QFile>
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

bool renameReleasingOpenFiles(const QString& from, const QString& to)
{
	releaseMediaFilesHeldOpen({ from });
	const bool renamed = QFile::rename(from, to);
	followMovedMediaFiles({ from }, renamed ? std::vector<PathMove>{ { from, to } } : std::vector<PathMove>{});
	return renamed;
}
