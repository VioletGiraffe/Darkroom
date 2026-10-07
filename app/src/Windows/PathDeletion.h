#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QSet>
#include <QStringList>
RESTORE_COMPILER_WARNINGS

class QWidget;

// Interactive deletion of files and folders, folders with their whole contents.
// The caller owns the initial deletion confirmation.
// Open video players let go of the paths for the duration, see VideoPlayerWindow::FileRelease.
namespace PathDeletion
{
	enum class Mode { Trash, Permanent };

	// Bulleted native paths for a message box, capped with an "... and N more" line.
	[[nodiscard]] QString bulletedPathList(const QStringList& paths);

	// Returns the paths that are gone afterwards; an absent path counts as one, an empty path never does.
	// Trash failures are offered together for an explicit, default-cancelled permanent deletion.
	// Permanent-deletion failures are reported in one message.
	[[nodiscard]] QSet<QString> removePathsInteractive(const QStringList& paths, Mode mode, QWidget* dialogParent);

	// One path in Trash mode; a single Trash failure also reports QFile's available diagnostic.
	[[nodiscard]] bool removePathTrashFirstInteractive(const QString& path, QWidget* dialogParent);
}
