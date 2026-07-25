#pragma once

#include "Core/MediaId.h"

#include <QStringList>

#include <vector>

class Catalog;
class QWidget;

// Interactive removal/deletion workflows. Catalog owns metadata mutations; this module owns confirmation,
// filesystem deletion, and failure reporting.
namespace MediaItemManagement
{
	struct DeleteResult
	{
		// A failed deletion can partially alter storage without changing the Catalog.
		bool storageRefreshRequired = false;
		QStringList affectedFrameFolders;
	};

	[[nodiscard]] DeleteResult deleteItemsInteractive(
		Catalog& catalog, const std::vector<MediaId>& selection, QWidget* dialogParent);
	void removeItemsFromLibraryInteractive(Catalog& catalog, const std::vector<MediaId>& selection, QWidget* dialogParent);
}
