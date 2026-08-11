#pragma once

#include "Core/MediaId.h"

#include <QStringList>

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

	// The caller owns the initial deletion confirmation. An absent path succeeds. A Trash failure reports QFile's
	// available diagnostic and offers an explicit, default-cancelled permanent-deletion fallback.
	[[nodiscard]] bool removePathTrashFirstInteractive(const QString& path, QWidget* dialogParent);

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
