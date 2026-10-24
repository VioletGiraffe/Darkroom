#pragma once

#include "Core/MediaItemId.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QStringList>
RESTORE_COMPILER_WARNINGS

#include <vector>

class Catalog;
class QWidget;

// Interactive removal/deletion workflows. Catalog owns metadata mutations; this module owns confirmation,
// filesystem deletion, failure reporting, and the item naming those dialogs present.
namespace MediaItemManagement
{
	// A photo keeps its extension; a video is named by Catalog::displayName, which drops it.
	[[nodiscard]] QString itemDisplayName(const Catalog& catalog, MediaItemId id);
	// Bulleted itemDisplayName lines for a message box, in the given order, capped with an "... and N more" line.
	[[nodiscard]] QString bulletedItemNameList(const Catalog& catalog, const std::vector<MediaItemId>& items);

	struct RemovalResult
	{
		// Items whose filesystem removal completed and whose Catalog records were removed, in selection order.
		std::vector<MediaItemId> removedItems;
		// A failed removal can partially alter storage without changing the Catalog.
		bool storageRefreshRequired = false;
		QStringList affectedFrameFolders;
	};

	// Moves each video's frame folder and source file, and each photo's file, to Trash.
	[[nodiscard]] RemovalResult deleteItemsInteractive(
		Catalog& catalog, const std::vector<MediaItemId>& selection, QWidget* dialogParent);
	// Moves each video's frame folder to Trash; source files are kept.
	[[nodiscard]] RemovalResult removeItemsFromLibraryInteractive(Catalog& catalog, const std::vector<MediaItemId>& selection, QWidget* dialogParent);
	// removeItemsFromLibraryInteractive without the confirmation.
	[[nodiscard]] RemovalResult removeItemsFromLibrary(Catalog& catalog, const std::vector<MediaItemId>& items, QWidget* dialogParent);
}
