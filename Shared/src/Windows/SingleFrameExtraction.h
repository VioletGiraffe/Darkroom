#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QString>
RESTORE_COMPILER_WARNINGS

class QWidget;

// Interactive single-frame extraction, and the last destination used.
// Synchronous: the GUI thread blocks for the ffmpeg call.
namespace SingleFrameExtraction
{
	enum class LastDestination
	{
		None,
		Target, // the player host's frame target
		Folder,
	};

	[[nodiscard]] LastDestination lastDestination();
	[[nodiscard]] QString lastFolder();
	void rememberTargetAsLastDestination();

	// Writes the frame under a name free in folder, creating folder when missing.
	// Returns the file's path, empty after reporting the failure.
	[[nodiscard]] QString extractFrame(const QString& videoPath, qint64 timestampMs, const QString& folder, QWidget* dialogParent);
	// extractFrame, then remembers folder as the last destination and shows where the frame went.
	void extractToFolderInteractive(const QString& videoPath, qint64 timestampMs, const QString& folder, QWidget* dialogParent);
}
