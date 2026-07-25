#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

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
