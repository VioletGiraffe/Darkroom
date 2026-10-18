#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QString>
RESTORE_COMPILER_WARNINGS

class Library;
class QWidget;

// Interactive single-frame extraction and optional import into the library.
namespace SingleFrameExtraction
{
	enum class LastDestination
	{
		None,
		Library,
		Folder,
	};

	[[nodiscard]] LastDestination lastDestination();
	[[nodiscard]] QString lastFolder();

	// Synchronous: the GUI thread blocks for the ffmpeg call.
	void extractToFolderInteractive(const QString& videoPath, qint64 timestampMs, const QString& folder, QWidget* dialogParent);
	void extractToLibraryInteractive(Library& library, const QString& videoPath, qint64 timestampMs, QWidget* dialogParent);
}
