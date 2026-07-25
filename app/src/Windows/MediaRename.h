#pragma once

#include "Core/MediaId.h"

#include <QString>

class QWidget;
class Catalog;

// Transactional disk and catalog rename flow; failures are reported internally.
namespace MediaRename
{
	struct Result
	{
		bool renamed = false;
		QString oldFolderPath;
		QString newFolderPath;
		QString newName;
	};

	Result renameItemInteractive(Catalog& catalog, const MediaId& id, QWidget* dialogParent);
}
