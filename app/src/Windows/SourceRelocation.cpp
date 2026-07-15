#include "Windows/SourceRelocation.h"
#include "Core/Library.h"
#include "Core/MediaId.h"
#include "Utils.h"
#include "Windows/VideoPlayerWindow.h"

#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

using SourceRelocation::Mode;

// Performs the actual copy/move; on failure, warns and falls back to leaving
// the file at srcPath so the caller can still import it from there.
[[nodiscard]] QString copyOrMove(QWidget* dialogParent, const QString& srcPath, const QString& destPath, bool isMove)
{
	const bool ok = isMove ? QFile{ srcPath }.rename(destPath) : QFile::copy(srcPath, destPath);
	if (!ok)
	{
		QMessageBox::warning(dialogParent, QObject::tr("Error"), QObject::tr("Failed to %1:\n%2\nto:\n%3")
			.arg(isMove ? QObject::tr("move") : QObject::tr("copy"), srcPath, destPath));
		return srcPath;
	}
	return destPath;
}

// ============================================================================
// FileCollisionDialog - shown when the relocation destination already has a
// file with the same name as the one being added.
//
// Not a plain QMessageBox because the "files differ" case offers Play buttons
// that must NOT close the dialog (they let the user preview both files before
// deciding), alongside the decision buttons that do.
// ============================================================================

class FileCollisionDialog final : public QDialog
{
public:
	enum class Result { Overwrite, Skip, SkipAndDelete, Cancel };

	FileCollisionDialog(Library& library, const QString& stagedPath, const QString& destPath, bool isDuplicate, QWidget* parent)
		: QDialog(parent)
	{
		setWindowTitle(isDuplicate ? tr("Duplicate File Found") : tr("File Already Exists"));

		// WindowModal (not the exec()-default ApplicationModal) blocks only this
		// dialog's parent (ImportDialog) and up - it deliberately leaves sibling
		// top-level windows, such as the VideoPlayerWindow opened by the Play
		// buttons below, interactive.
		setWindowModality(Qt::WindowModal);

		QVBoxLayout* layout = new QVBoxLayout(this);

		QLabel* message = new QLabel(isDuplicate
			? tr("An identical file is already at the destination:\n\n%1\n\nIt won't be imported again. You can optionally delete the redundant staged copy:\n\n%2").arg(destPath, stagedPath)
			: tr("A different file with the same name already exists at the destination:\n\n%1\n\nOverwrite it with the staged file, skip importing this one, or cancel to leave it staged and decide later:\n\n%2").arg(destPath, stagedPath),
			this);
		message->setWordWrap(true);
		layout->addWidget(message);

		QHBoxLayout* buttonRow = new QHBoxLayout;

		if (!isDuplicate)
		{
			// Play buttons open a preview parented to the outer ImportDialog
			// (this dialog's parent), not to this dialog - this dialog is
			// destroyed as soon as Overwrite/Skip is clicked, which would
			// otherwise kill an in-progress preview along with it.
			QWidget* previewParent = parent;
			Library* const playerLibrary = &library;

			QPushButton* playStaged = new QPushButton(tr("Play Staged File"), this);
			connect(playStaged, &QPushButton::clicked, this, [playerLibrary, previewParent, stagedPath] {
				VideoPlayerWindow::createPlayerWindow(*playerLibrary, stagedPath, previewParent);
			});
			buttonRow->addWidget(playStaged);

			QPushButton* playExisting = new QPushButton(tr("Play Existing File"), this);
			connect(playExisting, &QPushButton::clicked, this, [playerLibrary, previewParent, destPath] {
				VideoPlayerWindow::createPlayerWindow(*playerLibrary, destPath, previewParent);
			});
			buttonRow->addWidget(playExisting);
		}

		buttonRow->addStretch(1);

		if (isDuplicate)
		{
			QPushButton* skip = new QPushButton(tr("Skip"), this);
			connect(skip, &QPushButton::clicked, this, [this] { m_result = Result::Skip; accept(); });
			buttonRow->addWidget(skip);

			QPushButton* skipDelete = new QPushButton(tr("Skip and Delete Duplicate"), this);
			connect(skipDelete, &QPushButton::clicked, this, [this] { m_result = Result::SkipAndDelete; accept(); });
			buttonRow->addWidget(skipDelete);
		}
		else
		{
			QPushButton* overwrite = new QPushButton(tr("Overwrite"), this);
			connect(overwrite, &QPushButton::clicked, this, [this] { m_result = Result::Overwrite; accept(); });
			buttonRow->addWidget(overwrite);

			QPushButton* skip = new QPushButton(tr("Skip"), this);
			connect(skip, &QPushButton::clicked, this, [this] { m_result = Result::Skip; accept(); });
			buttonRow->addWidget(skip);

			// Defer: import nothing, touch no file, and leave the entry staged so
			// the user can deal with the name clash later.
			QPushButton* cancel = new QPushButton(tr("Cancel"), this);
			connect(cancel, &QPushButton::clicked, this, [this] { m_result = Result::Cancel; accept(); });
			buttonRow->addWidget(cancel);
		}

		layout->addLayout(buttonRow);
	}

	[[nodiscard]] Result result() const { return m_result; }

private:
	// Cancel is the default so dismissing the dialog (Escape / window close) defers
	// rather than taking any action - the safe no-op for either flavor.
	Result m_result = Result::Cancel;
};

// Outcome of relocating one file. importPath empty => don't import it; of those, keepStaged true => the
// user deferred (Cancel), so leave it staged for a later retry, while keepStaged false => the collision was
// resolved as "don't import" (Skip / Skip and Delete), so the entry should be cleared from staging.
struct RelocationOutcome
{
	QString importPath;
	bool keepStaged = false;
};

// Resolves relocation (including any naming collision) for a single file.
// On a collision "Skip"/"Skip and Delete" the destination is treated as the
// already-catalogued copy and this item is not imported; "Cancel" additionally
// keeps it staged. File-operation *failures* fall back to importing from the
// original path, so an I/O error never silently drops a file the user wanted.
[[nodiscard]] RelocationOutcome performRelocation(Library& library, QWidget* dialogParent, const QString& path, Mode mode,
	const QString& destFolder)
{
	const QString destPath = destFolder + "/" + QFileInfo(path).fileName();
	const bool isMove = (mode == Mode::Move);

	// Already at the destination - e.g. a retry after an earlier Copy/Move whose import was declined (the
	// staged entry follows the file, see ImportDialog::runImport) - so there is nothing to relocate and,
	// critically, no "collision": without this the file would be compared against itself and offered up as a duplicate.
	if (QFileInfo(path) == QFileInfo(destPath))
		return { path, false };

	if (!QFile::exists(destPath))
		return { copyOrMove(dialogParent, path, destPath, isMove), false };

	// A name+size match (MediaId) is the cheap first gate; on that collision a full byte comparison confirms
	// a genuine duplicate, so the astronomically-rare same-name/same-size/different-content case is still
	// classified as "files differ". The && short-circuits the byte read when the MediaIds (sizes) differ.
	const bool isDuplicate = (MediaId::fromFile(path) == MediaId::fromFile(destPath)) && filesAreIdentical(path, destPath);
	FileCollisionDialog collisionDialog(library, path, destPath, isDuplicate, dialogParent);
	collisionDialog.exec();

	switch (collisionDialog.result())
	{
	case FileCollisionDialog::Result::Overwrite:
		if (!QFile::remove(destPath))
		{
			QMessageBox::warning(dialogParent, QObject::tr("Error"), QObject::tr("Failed to overwrite existing file:\n%1").arg(destPath));
			return { path, false }; // I/O failure - fall back to importing from the original
		}
		return { copyOrMove(dialogParent, path, destPath, isMove), false };

	case FileCollisionDialog::Result::SkipAndDelete:
		if (!QFile::remove(path))
			QMessageBox::warning(dialogParent, QObject::tr("Error"), QObject::tr("Failed to delete duplicate file:\n%1").arg(path));
		return { {}, false }; // not imported, removed from staging

	case FileCollisionDialog::Result::Skip:
		return { {}, false }; // not imported, removed from staging

	case FileCollisionDialog::Result::Cancel:
		break;
	}

	return { {}, true }; // deferred - not imported, kept staged
}

} // namespace

SourceRelocation::BatchResult SourceRelocation::relocateIfNeeded(Library& library, QWidget* dialogParent, const QStringList& paths,
	Mode mode, const QString& destFolder)
{
	if (mode == Mode::LeaveInPlace)
		return { paths, {} };

	if (destFolder.isEmpty())
	{
		QMessageBox::warning(dialogParent, QObject::tr("Error"), QObject::tr("No destination folder is set for relocating source files - they will be left in their original location."));
		return { paths, {} };
	}
	if (!QDir{}.mkpath(destFolder))
	{
		QMessageBox::warning(dialogParent, QObject::tr("Error"), QObject::tr("Could not create or access destination folder:\n%1").arg(destFolder));
		return { paths, {} };
	}

	BatchResult result;
	result.toImport.reserve(paths.size());
	for (const QString& path : paths)
	{
		const RelocationOutcome outcome = performRelocation(library, dialogParent, path, mode, destFolder);
		if (!outcome.importPath.isEmpty())
		{
			result.toImport.append(outcome.importPath);
			if (outcome.importPath != path)
				result.relocatedTo.insert(path, outcome.importPath);
		}
		else if (outcome.keepStaged)
			result.keepStaged.append(path);
		else
			result.skipped.append(path);
	}
	return result;
}
