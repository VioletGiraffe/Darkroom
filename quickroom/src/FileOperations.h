#pragma once

#include "Windows/PathDeletion.h"

class QWidget;

namespace FileOperations
{
	// Asks first when deleting permanently or when paths include a folder; files go to Trash unasked.
	// Returns the paths that are gone afterwards.
	[[nodiscard]] QSet<QString> deleteWithConfirmation(const QStringList& paths, PathDeletion::Mode mode, QWidget* dialogParent);
}
