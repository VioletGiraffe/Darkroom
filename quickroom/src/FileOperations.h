#pragma once

#include "Windows/PathDeletion.h"

#include <functional>

class ImageViewerWindow;
class QWidget;
class VideoPlayerWindow;

namespace FileOperations
{
	// Asks first when deleting permanently or when paths include a folder; files go to Trash unasked.
	// Returns the paths that are gone afterwards.
	[[nodiscard]] QSet<QString> deleteWithConfirmation(const QStringList& paths, PathDeletion::Mode mode, QWidget* dialogParent);

	// Menu wording for deleteWithConfirmation in the given mode.
	[[nodiscard]] QString deleteActionText(PathDeletion::Mode mode);

	// Give the window its removal actions: Del is Trash, Shift+Del is permanent. onDeleted receives each deleted path.
	// imagePaths is the list the viewer was opened with.
	void installDeleteActions(ImageViewerWindow& viewer, const QStringList& imagePaths, std::function<void(const QString& path)> onDeleted = {});
	void installDeleteActions(VideoPlayerWindow& player, std::function<void(const QString& path)> onDeleted = {});
}
