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
		// A confirmed deletion attempt can partially alter storage even when no catalog record is removed.
		bool refreshRequired = false;
		QStringList affectedFrameFolders;
	};

	[[nodiscard]] DeleteResult deleteItemsInteractive(
		Catalog& catalog, const std::vector<MediaId>& selection, QWidget* dialogParent);
	[[nodiscard]] bool removeItemsFromLibraryInteractive(
		Catalog& catalog, const std::vector<MediaId>& selection, QWidget* dialogParent);
}
