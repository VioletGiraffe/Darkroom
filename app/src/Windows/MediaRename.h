#pragma once

#include "Core/MediaId.h"

#include <QString>

class QWidget;
class Catalog;

// The interactive rename flows for catalog media items. Each flow prompts for the new name, validates it,
// spells out every resulting change in a confirmation dialog, applies the on-disk rename(s), and re-keys the
// catalog record onto the new identity (a MediaId is name+size, so renaming the file changes it) - rolling
// the disk renames back if the catalog refuses. Every failure is reported to the user here; the caller only
// gets the outcome, for its own UI fixups (view refresh, frame viewer).
namespace MediaRename
{
	struct Result
	{
		bool renamed = false;  // false: cancelled, failed validation, or the rename failed (already reported)
		// The frame folder before/after the rename - set only for a video (a photo rename moves no folder).
		QString oldFolderPath;
		QString newFolderPath;
		QString newName;       // the new display (base) name - set only for a video; the folder leaf now hides it
	};

	// Dispatches to the video- or photo-specific flow by the item's type. dialogParent hosts every prompt.
	Result renameItemInteractive(Catalog& catalog, const MediaId& id, QWidget* dialogParent);
}
