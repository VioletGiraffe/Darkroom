#pragma once

#include "Core/MediaId.h"
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
	[[nodiscard]] QString itemDisplayName(const Catalog& catalog, const MediaId& id);
	// Bulleted itemDisplayName lines for a message box, in the given order, capped with an "... and N more" line.
	[[nodiscard]] QString bulletedItemNameList(const Catalog& catalog, const std::vector<MediaId>& items);

	struct DeleteResult
	{
		// Items whose filesystem deletion completed and whose Catalog records were removed, in selection order.
		std::vector<MediaId> deletedItems;
		// A failed deletion can partially alter storage without changing the Catalog.
		bool storageRefreshRequired = false;
		QStringList affectedFrameFolders;
	};

	[[nodiscard]] DeleteResult deleteItemsInteractive(
		Catalog& catalog, const std::vector<MediaId>& selection, QWidget* dialogParent);
	void removeItemsFromLibraryInteractive(Catalog& catalog, const std::vector<MediaId>& selection, QWidget* dialogParent);
}
