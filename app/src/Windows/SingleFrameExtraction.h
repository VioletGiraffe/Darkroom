#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QString>
RESTORE_COMPILER_WARNINGS

#include <functional>

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

	// These workflows are synchronous and do not retain the callback. extractionFinished runs immediately after
	// the blocking ffmpeg call, before any result UI or import work.
	void extractToFolderInteractive(const QString& videoPath, qint64 timestampMs, const QString& folder,
		const std::function<void()>& extractionFinished, QWidget* dialogParent);
	void extractToLibraryInteractive(Library& library, const QString& videoPath, qint64 timestampMs,
		const std::function<void()>& extractionFinished, QWidget* dialogParent);
}
