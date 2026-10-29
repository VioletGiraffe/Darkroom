#pragma once

#include "Windows/PathDeletion.h"
#include "fileoperations/fileoperationtypes.h"

#include <functional>

class ImageViewerWindow;
class PhotoCompareWindow;
class QMimeData;
class QWidget;
class VideoPlayerWindow;

namespace FileOperations
{
	// Puts paths on the clipboard as file URLs. cut adds the markers file managers read as "move on paste".
	// Leaves the clipboard unchanged when paths is empty.
	void putPathsOnClipboard(const QStringList& paths, bool cut);

	// The local files of mime's URLs; other URLs are dropped.
	[[nodiscard]] QStringList localFilePaths(const QMimeData& mime);
	// Whether mime carries a cut marker from this app or a file manager.
	[[nodiscard]] bool isMarkedAsCut(const QMimeData& mime);

	// Copies or moves sources into destinationFolder in a progress dialog that blocks parent's window while in the foreground.
	// Before a move, open players and viewers release the files moved.
	void startTransfer(TransferKind kind, const QStringList& sources, const QString& destinationFolder, QWidget* parent);
	// Whether moving source into destinationFolder is predicted to be a rename; false when that cannot be determined.
	// Reads both paths' filesystem metadata.
	[[nodiscard]] bool isOnSameVolume(const QString& source, const QString& destinationFolder);

	// Asks first when deleting permanently or when paths include a folder; files go to Trash unasked.
	// Returns the paths that are gone afterwards.
	[[nodiscard]] QSet<QString> deleteWithConfirmation(const QStringList& paths, PathDeletion::Mode mode, QWidget* dialogParent);

	// Menu wording for deleteWithConfirmation in the given mode.
	[[nodiscard]] QString deleteActionText(PathDeletion::Mode mode);

	// Give the window its removal actions: Del is Trash, Shift+Del is permanent. onDeleted receives each deleted path.
	void installDeleteActions(ImageViewerWindow& viewer, std::function<void(const QString& path)> onDeleted = {});
	void installDeleteActions(VideoPlayerWindow& player, std::function<void(const QString& path)> onDeleted = {});
	// The same for the comparison's pane menu, with Trash only.
	void installDeleteAction(PhotoCompareWindow& compareWindow, std::function<void(const QString& path)> onDeleted = {});
}
