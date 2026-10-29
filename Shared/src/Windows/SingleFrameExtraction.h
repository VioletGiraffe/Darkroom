#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QString>
RESTORE_COMPILER_WARNINGS

#include <functional>

class QWidget;

// Interactive single-frame extraction, and the last destination used.
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

	// Writes the frame under a name free in folder, creating folder when missing, then passes the file's path to onWritten.
	// Reports a failure instead. Runs in the background: destroying dialogParent abandons it, and onWritten never runs.
	void extractFrame(const QString& videoPath, qint64 timestampMs, const QString& folder, QWidget* dialogParent,
		std::function<void(const QString& framePath)> onWritten);
	// extractFrame, then remembers folder as the last destination and shows where the frame went.
	void extractToFolderInteractive(const QString& videoPath, qint64 timestampMs, const QString& folder, QWidget* dialogParent);
}
