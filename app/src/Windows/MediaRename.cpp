#include "Windows/MediaRename.h"
#include "Core/Catalog.h"
#include "Utils.h"
#include "assert/advanced_assert.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>

namespace {

using MediaRename::Result;

// Whether a rename changing only the letter case of the name may proceed: yes on a case-insensitive filesystem
// (Windows, macOS), where it keeps the same file; refused with a warning on a case-sensitive one, where the case
// variant is a genuinely different path and case-variant siblings are not supported.
bool caseOnlyRenameAllowed([[maybe_unused]] QWidget* parent, [[maybe_unused]] const QString& title)
{
#if defined Q_OS_WIN || defined Q_OS_MACOS
	return true;
#else
	QMessageBox::warning(parent, title, QObject::tr("Changing only the letter case of the name is not supported on a case-sensitive file system."));
	return false;
#endif
}

// Rolls back a rename that already succeeded, after a later step of the same operation failed. Returns an empty
// string when the rollback worked, otherwise a fragment naming what was left behind, to be appended to the error
// message the caller is about to show.
[[nodiscard]] QString undoRenameOrDescribeFailure(const QString& currentPath, const QString& originalPath)
{
	if (QFile::rename(currentPath, originalPath))
		return {};
	return "\n\n" + QObject::tr("Additionally, this rename could not be undone:\n%1\n→ %2").arg(currentPath, originalPath);
}

// Executes a video rename whose target names the caller already computed and validated: rename the source file (when
// one is recorded and present), then the frame folder, then re-key the catalog record - undoing the disk renames if a
// later step fails. newSourcePath is empty when no source is recorded; newId equals oldId unless the source file was
// actually renamed (identity is the source file's name+size, so it can't change when there's no file to rename).
Result renameVideo(Catalog& catalog, const MediaId& oldId, const MediaId& newId,
	const QString& newName, const QString& newSourcePath, const QString& newFolderPath, QWidget* parent)
{
	const QString dialogTitle = QObject::tr("Rename media file");
	const QString oldFolderPath = catalog.folderForMediaItem(oldId);
	const QString oldSourcePath = catalog.sourcePathForMediaItem(oldId);

	// --- Step 1: rename the source video file (if one is recorded and present) ---
	const bool renameSourceFile = !oldSourcePath.isEmpty() && QFile::exists(oldSourcePath);
	if (renameSourceFile && !QFile::rename(oldSourcePath, newSourcePath))
	{
		QMessageBox::critical(parent, dialogTitle, QObject::tr("Failed to rename the source file:\n%1\n→ %2").arg(oldSourcePath, newSourcePath));
		return {};
	}

	// --- Step 2: rename the frame folder ---
	if (!QFile::rename(oldFolderPath, newFolderPath))
	{
		QString message = QObject::tr("Failed to rename the frame folder:\n%1\n→ %2").arg(oldFolderPath, newFolderPath);
		if (renameSourceFile)
			message += undoRenameOrDescribeFailure(newSourcePath, oldSourcePath);
		QMessageBox::critical(parent, dialogTitle, message);
		return {};
	}

	// --- Step 3: update the catalog: carry the metadata record (loop intervals, labels incl. Best) to the new
	// identity and record the new source path + frame folder.
	// Refused when the new name+size collides with another tracked item - undo the disk renames then, so disk
	// and catalog stay in sync (a collision requires a changed id, so the source file was renamed here too).
	if (!catalog.applyRename(oldId, newId, newSourcePath, newFolderPath))
	{
		QString message = QObject::tr("An item with the same name and file size is already tracked under a different label:\n%1").arg(newSourcePath);
		message += undoRenameOrDescribeFailure(newFolderPath, oldFolderPath);
		if (renameSourceFile)
			message += undoRenameOrDescribeFailure(newSourcePath, oldSourcePath);
		QMessageBox::critical(parent, dialogTitle, message);
		return {};
	}

	return { .renamed = true, .oldFolderPath = oldFolderPath, .newFolderPath = newFolderPath, .newName = newName };
}

Result renameVideoInteractive(Catalog& catalog, const MediaId& id, QWidget* parent)
{
	// Videos only - the dispatcher routes photos elsewhere. The rename below derives video-shaped paths; on an
	// owned photo, folderForMediaItem is the SHARED Photos/<label> dir and would pass the exists-check.
	assert_r(catalog.mediaType(id) == Catalog::MediaType::Video);

	const QString originalFolderPath = catalog.folderForMediaItem(id);

	// The frame folder must exist
	if (originalFolderPath.isEmpty() || !QDir(originalFolderPath).exists())
	{
		QMessageBox::critical(parent, QObject::tr("Rename media file"), QObject::tr("Frame folder does not exist:\n%1").arg(originalFolderPath));
		return {};
	}

	const QString oldSourcePath = catalog.sourcePathForMediaItem(id);
	const bool sourceExists = !oldSourcePath.isEmpty() && QFile::exists(oldSourcePath);

	// The editable name is the item's base name (its source file's), not the frame-folder leaf - the leaf now carries
	// a hash suffix that must neither be shown to the user nor seed the new name.
	const QString oldBaseName = catalog.displayName(id);
	const QString parentPath  = QFileInfo(originalFolderPath).absolutePath();

	// Ask for the new name
	const QString newBaseName = QInputDialog::getText(parent, QObject::tr("Rename media file"), QObject::tr("New name:"), QLineEdit::Normal, oldBaseName).trimmed();

	if (newBaseName.isEmpty() || newBaseName == oldBaseName)
		return {};

	if (const QChar bad = invalidFilenameChar(newBaseName); !bad.isNull())
	{
		QMessageBox::warning(parent, QObject::tr("Rename media file"), QObject::tr("Name contains an invalid character: '%1'").arg(bad));
		return {};
	}

	const bool caseChangeOnly = newBaseName.compare(oldBaseName, Qt::CaseInsensitive) == 0;
	if (caseChangeOnly && !caseOnlyRenameAllowed(parent, QObject::tr("Rename media file")))
		return {};

	// Build the new source path (same directory and extension, new base name) and the identity it implies. A rename
	// preserves the file's size, so deriving the id from name+size needs no disk access and matches what the renamed
	// file would resolve to. Identity actually changes only when the source file itself is renamed.
	QString newSourcePath;
	MediaId newId = id;
	if (!oldSourcePath.isEmpty())
	{
		const QFileInfo oldSourceInfo{ oldSourcePath };
		const QString newFileName = oldSourceInfo.suffix().isEmpty() ? newBaseName : newBaseName + "." + oldSourceInfo.suffix();
		newSourcePath = oldSourceInfo.absolutePath() + "/" + newFileName;
		if (sourceExists)
			newId = MediaId::fromNameAndSize(newFileName, id.size());

		// Make sure we would not overwrite a different existing file (a case-only rename keeps the same file)
		if (!caseChangeOnly && sourceExists && QFile::exists(newSourcePath))
		{
			QMessageBox::warning(parent, QObject::tr("Rename media file"), QObject::tr("A file with that name already exists:\n%1").arg(newSourcePath));
			return {};
		}
	}

	// The frame folder is <newBaseName>_<hash>. A case-only rename leaves the identity (hence the hash) unchanged, so
	// only the prefix's case differs and the pre-existing folder is not a collision.
	const QString newFolderPath = parentPath + "/" + Catalog::frameFolderName(newBaseName, newId);
	if (!caseChangeOnly && QDir(newFolderPath).exists())
	{
		QMessageBox::warning(parent, QObject::tr("Rename media file"), QObject::tr("A folder with that name already exists:\n%1").arg(newFolderPath));
		return {};
	}

	// Build a confirmation message that spells out every change
	QString message = QObject::tr("Rename “%1” to “%2”?\n\n").arg(oldBaseName, newBaseName);
	if (sourceExists)
	{
		message += QObject::tr("• Source file:\n  %1\n  → %2\n\n").arg(oldSourcePath, newSourcePath);
	}
	else if (!oldSourcePath.isEmpty())
	{
		message += QObject::tr("• Source file not found at stored path — it will not be renamed.\n"
			"  The stored path will be updated to reflect the new name.\n\n");
	}
	message += QObject::tr("• Frame folder:\n  %1\n  → %2").arg(originalFolderPath, newFolderPath);

	if (catalog.mediaItemHasLabel(id, Catalog::BestLabelId))
		message += QObject::tr("\n\n• Best label reference will be updated.");

	if (QMessageBox::question(parent, QObject::tr("Rename media file"), message,
		QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) != QMessageBox::Yes)
		return {};

	return renameVideo(catalog, id, newId, newBaseName, newSourcePath, newFolderPath, parent);
}

Result renamePhoto(Catalog& catalog, const MediaId& oldId, const QString& newSourcePath, QWidget* parent)
{
	const QString title = QObject::tr("Rename photo");
	const QString oldSourcePath = catalog.sourcePathForMediaItem(oldId);
	const QString folderAbs     = catalog.folderForMediaItem(oldId);  // unchanged by the rename: Photos/<label> (owned) or empty (referenced)

	// Renaming the file is the entire on-disk rename for a photo. It changes the base name and thus the MediaId
	// (identity = name + size), so the catalog must re-key onto the new id.
	if (!QFile::rename(oldSourcePath, newSourcePath))
	{
		QMessageBox::critical(parent, title, QObject::tr("Failed to rename the file:\n%1\n→ %2").arg(oldSourcePath, newSourcePath));
		return {};
	}

	const MediaId newId = MediaId::fromFile(newSourcePath);

	// Carry the metadata record (labels incl. Best) to the new identity, keeping the storage folder as-is. Refused
	// when the new name + size already belongs to a different tracked item - undo the on-disk rename so disk and
	// catalog stay in sync.
	if (!catalog.applyRename(oldId, newId, newSourcePath, folderAbs))
	{
		QString message = QObject::tr("An item with the same name and file size is already tracked in the library:\n%1").arg(newSourcePath);
		message += undoRenameOrDescribeFailure(newSourcePath, oldSourcePath);
		QMessageBox::critical(parent, title, message);
		return {};
	}

	return { .renamed = true };
}

Result renamePhotoInteractive(Catalog& catalog, const MediaId& id, QWidget* parent)
{
	// A photo has no frame folder; its file IS the item, and its identity is that file's name + size. Renaming
	// here means renaming just the file's base name in place (owned: inside Photos/<label>; referenced: in the
	// user's own folder), keeping the extension - never renaming the folder the way the video path does.
	assert_r(catalog.mediaType(id) == Catalog::MediaType::Photo);

	const QString title = QObject::tr("Rename photo");
	const QString oldSourcePath = catalog.sourcePathForMediaItem(id);

	// The file is the whole item - if it is gone (owned file lost, or a referenced file moved/unmounted) there is
	// nothing to rename, so refuse rather than point a stored path at a name that exists nowhere.
	if (oldSourcePath.isEmpty() || !QFile::exists(oldSourcePath))
	{
		QMessageBox::warning(parent, title, QObject::tr("The photo file no longer exists, so it cannot be renamed:\n%1").arg(oldSourcePath));
		return {};
	}

	const QFileInfo oldInfo{ oldSourcePath };
	const QString oldBaseName = oldInfo.completeBaseName();  // the name shown on the card
	const QString suffix      = oldInfo.suffix();

	const QString newBaseName = QInputDialog::getText(parent, title, QObject::tr("New name:"), QLineEdit::Normal, oldBaseName).trimmed();
	if (newBaseName.isEmpty() || newBaseName == oldBaseName)
		return {};

	if (const QChar bad = invalidFilenameChar(newBaseName); !bad.isNull())
	{
		QMessageBox::warning(parent, title, QObject::tr("Name contains an invalid character: '%1'").arg(bad));
		return {};
	}

	const bool caseChangeOnly = newBaseName.compare(oldBaseName, Qt::CaseInsensitive) == 0;
	if (caseChangeOnly && !caseOnlyRenameAllowed(parent, title))
		return {};

	const QString newFileName   = suffix.isEmpty() ? newBaseName : newBaseName + "." + suffix;
	const QString newSourcePath = oldInfo.absolutePath() + "/" + newFileName;

	// Refuse to clobber a different file already at the destination. A pure case change reuses the same file on a
	// case-insensitive filesystem (the only kind that lets it through), so it is exempt.
	if (!caseChangeOnly && QFile::exists(newSourcePath))
	{
		QMessageBox::warning(parent, title, QObject::tr("A file with that name already exists:\n%1").arg(newSourcePath));
		return {};
	}

	QString message = QObject::tr("Rename “%1” to “%2”?\n\n").arg(oldInfo.fileName(), newFileName);
	message += catalog.isReferenced(id)
		? QObject::tr("• This photo is referenced in place - its file below, outside the library, will be renamed:\n  %1\n  → %2").arg(oldSourcePath, newSourcePath)
		: QObject::tr("• File:\n  %1\n  → %2").arg(oldSourcePath, newSourcePath);
	if (catalog.mediaItemHasLabel(id, Catalog::BestLabelId))
		message += QObject::tr("\n\n• Best label reference will be updated.");

	if (QMessageBox::question(parent, title, message, QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) != QMessageBox::Yes)
		return {};

	return renamePhoto(catalog, id, newSourcePath, parent);
}

} // namespace

MediaRename::Result MediaRename::renameItemInteractive(Catalog& catalog, const MediaId& id, QWidget* dialogParent)
{
	if (catalog.mediaType(id) == Catalog::MediaType::Photo)
		return renamePhotoInteractive(catalog, id, dialogParent);
	return renameVideoInteractive(catalog, id, dialogParent);
}
