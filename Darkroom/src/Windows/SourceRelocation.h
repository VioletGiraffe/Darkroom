#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QHash>
#include <QString>
#include <QStringList>
RESTORE_COMPILER_WARNINGS

class QWidget;
class Library;

// Optional per-file import relocation with interactive, byte-aware destination-collision handling.
namespace SourceRelocation
{
	enum class Mode { LeaveInPlace, Copy, Move };

	// Each original path enters at most one outcome; relocatedTo keeps retry state aligned with the actual file.
	struct BatchResult
	{
		QStringList toImport;
		QStringList keepStaged;
		QStringList skipped;
		QHash<QString, QString> relocatedTo;
	};

	// LeaveInPlace is a no-op. File-operation failures fall back to the original path rather than dropping it.
	[[nodiscard]] BatchResult relocateIfNeeded(Library& library, QWidget* dialogParent, const QStringList& paths, Mode mode,
		const QString& destFolder);
}
