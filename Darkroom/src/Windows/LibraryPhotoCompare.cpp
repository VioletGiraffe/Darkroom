#include "Windows/LibraryPhotoCompare.h"
#include "Core/Catalog.h"
#include "Core/Library.h"
#include "Core/MediaItemId.h"
#include "Windows/MediaItemManagement.h"
#include "Windows/PathDeletion.h"
#include "Windows/PhotoCompareWindow.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QDir>
#include <QMessageBox>
#include <QObject>
#include <QString>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <utility>

namespace {

// Returns whether the file is gone.
[[nodiscard]] bool deletePhotoInteractive(Catalog& catalog, const QString& filePath, QWidget* dialogParent)
{
	if (const MediaItemId trackedId = catalog.mediaItemAtSourcePath(filePath); trackedId != MediaItemId::None)
	{
		const MediaItemManagement::RemovalResult result = MediaItemManagement::deleteItemsInteractive(catalog, { trackedId }, dialogParent);
		return std::ranges::contains(result.removedItems, trackedId);
	}

	const QString question = QObject::tr("Move this photo to Trash?\n\n%1").arg(QDir::toNativeSeparators(filePath));
	if (QMessageBox::warning(dialogParent, QObject::tr("Delete photo"), question,
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
		return false;

	return PathDeletion::removePathTrashFirstInteractive(filePath, dialogParent);
}

} // namespace

void LibraryPhotoCompare::installDeleteAction(PhotoCompareWindow& compareWindow, Library& library, std::function<void(const QString& path)> onDeleted)
{
	// Library is stable across library switches; its catalog is not.
	compareWindow.setRemovalAction({ QObject::tr("Delete this photo from disk"),
		[&compareWindow, &library, onDeleted = std::move(onDeleted)](const QString& path) {
			const bool deleted = deletePhotoInteractive(library.catalog(), path, &compareWindow);
			if (deleted && onDeleted)
				onDeleted(path);
			return deleted;
		} });
}
