#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

class QWidget;
class Library;

// Source-file relocation for the import flow: optionally copy/move each staged source file into a chosen
// folder as it's imported, so the import proceeds from the new location instead of the original. A name
// collision at the destination is resolved interactively per file (a byte comparison tells a genuine
// duplicate from a mere name clash, with Play buttons to preview both sides of a differing pair).
namespace SourceRelocation
{
	enum class Mode { LeaveInPlace, Copy, Move };

	// Result of relocating a whole batch, reported per original path so the caller can update its staging
	// state. toImport is what to hand to import. Each original path additionally lands in at most one of:
	// keepStaged (the user deferred via Cancel - leave the entry staged untouched), skipped (collision resolved
	// as "don't import" via Skip / Skip and Delete - clear the entry from staging), or relocatedTo (the file was
	// actually copied/moved - point the staged entry at the new location, so a retry after a failed/declined
	// import starts from where the file really is; a Move deletes the original).
	struct BatchResult
	{
		QStringList toImport;
		QStringList keepStaged;
		QStringList skipped;
		QHash<QString, QString> relocatedTo;
	};

	// Batch entry point - no-op when mode is LeaveInPlace. Validates the destination folder once per batch
	// (rather than once per file) to avoid repeating the same warning for every file. dialogParent hosts the
	// collision/error dialogs. File-operation *failures* fall back to importing from the original path, so an
	// I/O error never silently drops a file the user wanted.
	[[nodiscard]] BatchResult relocateIfNeeded(Library& library, QWidget* dialogParent, const QStringList& paths, Mode mode,
		const QString& destFolder);
}
