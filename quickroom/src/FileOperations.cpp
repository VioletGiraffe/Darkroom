#include "FileOperations.h"
#include "Windows/ImageViewerWindow.h"
#include "Windows/VideoPlayerWindow.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QFileInfo>
#include <QMessageBox>
#include <QObject>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <utility>

namespace {

// The returned function deletes one path on behalf of window and returns whether it is gone.
[[nodiscard]] std::function<bool(const QString& path)> pathDeleter(QWidget* window, PathDeletion::Mode mode, std::function<void(const QString& path)> onDeleted)
{
	return [window, mode, onDeleted = std::move(onDeleted)](const QString& path) {
		const bool deleted = !FileOperations::deleteWithConfirmation({ path }, mode, window).empty();
		if (deleted && onDeleted)
			onDeleted(path);
		return deleted;
	};
}

} // namespace

QSet<QString> FileOperations::deleteWithConfirmation(const QStringList& paths, PathDeletion::Mode mode, QWidget* dialogParent)
{
	if (paths.empty())
		return {};

	const bool permanent = mode == PathDeletion::Mode::Permanent;
	const bool anyFolder = std::any_of(paths.begin(), paths.end(), [](const QString& path) { return QFileInfo(path).isDir(); });
	if (permanent || anyFolder)
	{
		QString message;
		if (paths.size() == 1)
			message = permanent ? QObject::tr("Permanently delete this item? This cannot be undone.") : QObject::tr("Move this folder to Trash?");
		else
			message = (permanent ? QObject::tr("Permanently delete %1 items? This cannot be undone.") : QObject::tr("Move %1 items to Trash?")).arg(paths.size());
		message += "\n" + PathDeletion::bulletedPathList(paths);
		if (anyFolder)
			message += "\n\n" + QObject::tr("Folders are deleted with all their contents.");

		if (QMessageBox::warning(dialogParent, QObject::tr("Delete"), message, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
			return {};
	}

	return PathDeletion::removePathsInteractive(paths, mode, dialogParent);
}

QString FileOperations::deleteActionText(PathDeletion::Mode mode)
{
	return mode == PathDeletion::Mode::Trash ? QObject::tr("Move to Trash") : QObject::tr("Delete permanently");
}

void FileOperations::installDeleteActions(ImageViewerWindow& viewer, const QStringList& imagePaths, std::function<void(const QString& path)> onDeleted)
{
	const auto removalAction = [&viewer, &imagePaths, &onDeleted](PathDeletion::Mode mode) {
		return ImageViewerWindow::RemovalAction{ deleteActionText(mode),
			[imagePaths, deletePath = pathDeleter(&viewer, mode, onDeleted)](int index) { return deletePath(imagePaths[index]); } };
	};
	viewer.setRemovalActions(removalAction(PathDeletion::Mode::Trash), removalAction(PathDeletion::Mode::Permanent));
}

void FileOperations::installDeleteActions(VideoPlayerWindow& player, std::function<void(const QString& path)> onDeleted)
{
	const auto removalAction = [&player, &onDeleted](PathDeletion::Mode mode) {
		return VideoPlayerWindow::RemovalAction{ deleteActionText(mode),
			[deletePath = pathDeleter(&player, mode, onDeleted)](const MediaId&, const QString& videoPath) { return deletePath(videoPath); } };
	};
	player.setRemovalActions(removalAction(PathDeletion::Mode::Trash), removalAction(PathDeletion::Mode::Permanent));
}
