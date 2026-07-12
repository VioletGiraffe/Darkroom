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

Result renameVideo(const MediaId& oldId, const QString& newFolderPath, QWidget* parent)
{
	const QString dialogTitle = QObject::tr("Rename media file");
	Catalog& catalog = Catalog::instance();
	const QString oldFolderPath = catalog.folderForMediaItem(oldId);
	const QString oldSourcePath = catalog.sourcePathForMediaItem(oldId);

	// --- Step 1: rename the source video file (if one is recorded and present) ---
	QString newSourcePath;
	bool sourceWasRenamed = false;
	MediaId newId = oldId;  // identity only changes if the source file itself is renamed (below)
	if (!oldSourcePath.isEmpty())
	{
		const QFileInfo oldSourceInfo{ oldSourcePath };
		newSourcePath = oldSourceInfo.absolutePath() + "/" + QFileInfo(newFolderPath).fileName() + "." + oldSourceInfo.suffix();

		if (QFile::exists(oldSourcePath))
		{
			if (!QFile::rename(oldSourcePath, newSourcePath))
			{
				QMessageBox::critical(parent, dialogTitle, QObject::tr("Failed to rename the source file:\n%1\n→ %2").arg(oldSourcePath, newSourcePath));
				return {};
			}
			sourceWasRenamed = true;
			newId = MediaId::fromFile(newSourcePath);  // renaming the file changes its name and thus its MediaId
		}
	}

	// --- Step 2: rename the frame folder ---
	if (!QFile::rename(oldFolderPath, newFolderPath))
	{
		if (sourceWasRenamed)
			QFile::rename(newSourcePath, oldSourcePath);
		QMessageBox::critical(parent, dialogTitle, QObject::tr("Failed to rename the frame folder:\n%1\n→ %2").arg(oldFolderPath, newFolderPath));
		return {};
	}

	// --- Step 3: update the catalog: carry the metadata record (loop intervals, labels incl. Best) to the new
	// identity and record the new source path + frame folder.
	// Refused when the new name+size collides with another tracked item - undo the disk renames then, so disk
	// and catalog stay in sync (a collision requires a changed id, so the source file was renamed here too).
	if (!catalog.applyRename(oldId, newId, newSourcePath, newFolderPath))
	{
		QFile::rename(newFolderPath, oldFolderPath);
		if (sourceWasRenamed)
			QFile::rename(newSourcePath, oldSourcePath);
		QMessageBox::critical(parent, dialogTitle,
			QObject::tr("An item with the same name and file size is already tracked under a different label:\n%1").arg(newSourcePath));
		return {};
	}

	return { .renamed = true, .oldFolderPath = oldFolderPath, .newFolderPath = newFolderPath };
}

Result renameVideoInteractive(const MediaId& id, QWidget* parent)
{
	// Videos only - the dispatcher routes photos elsewhere. The rename below derives video-shaped paths; on an
	// owned photo, folderForMediaItem is the SHARED Photos/<label> dir and would pass the exists-check.
	assert_r(Catalog::instance().mediaType(id) == Catalog::MediaType::Video);

	const QString originalFolderPath = Catalog::instance().folderForMediaItem(id);

	// The frame folder must exist
	if (originalFolderPath.isEmpty() || !QDir(originalFolderPath).exists())
	{
		QMessageBox::critical(parent, QObject::tr("Rename media file"), QObject::tr("Frame folder does not exist:\n%1").arg(originalFolderPath));
		return {};
	}

	const QString oldSourcePath = Catalog::instance().sourcePathForMediaItem(id);
	const bool sourceExists = !oldSourcePath.isEmpty() && QFile::exists(oldSourcePath);

	const QString oldName = QFileInfo(originalFolderPath).fileName();
	const QString parentPath = QFileInfo(originalFolderPath).absolutePath();

	// Ask for the new name
	const QString newName = QInputDialog::getText(parent, QObject::tr("Rename media file"), QObject::tr("New name:"), QLineEdit::Normal, oldName).trimmed();

	if (newName.isEmpty() || newName == oldName)
		return {};

	if (const QChar bad = invalidFilenameChar(newName); !bad.isNull())
	{
		QMessageBox::warning(parent, QObject::tr("Rename media file"), QObject::tr("Name contains an invalid character: '%1'").arg(bad));
		return {};
	}

	// Make sure the destination folder does not already exist
	const QString newFolderPath = parentPath + "/" + newName;
	if (QDir(newFolderPath).exists())
	{
		QMessageBox::warning(parent, QObject::tr("Rename media file"), QObject::tr("A folder with that name already exists:\n%1").arg(newFolderPath));
		return {};
	}

	// Build the new source path (same directory and extension, new base name)
	QString newSourcePath;
	if (!oldSourcePath.isEmpty())
	{
		const QFileInfo oldSourceInfo{ oldSourcePath };
		newSourcePath = oldSourceInfo.absolutePath() + "/" + newName + "." + oldSourceInfo.suffix();

		// Make sure we would not overwrite a different existing file
		if (sourceExists && QFile::exists(newSourcePath))
		{
			QMessageBox::warning(parent, QObject::tr("Rename media file"), QObject::tr("A file with that name already exists:\n%1").arg(newSourcePath));
			return {};
		}
	}

	// Build a confirmation message that spells out every change
	QString message = QObject::tr("Rename “%1” to “%2”?\n\n").arg(oldName, newName);
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

	if (Catalog::instance().mediaItemHasLabel(id, Catalog::BestLabelId))
		message += QObject::tr("\n\n• Best label reference will be updated.");

	if (QMessageBox::question(parent, QObject::tr("Rename media file"), message,
		QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) != QMessageBox::Yes)
		return {};

	return renameVideo(id, newFolderPath, parent);
}

Result renamePhoto(const MediaId& oldId, const QString& newSourcePath, QWidget* parent)
{
	const QString title = QObject::tr("Rename photo");
	Catalog& catalog = Catalog::instance();
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
		QFile::rename(newSourcePath, oldSourcePath);
		QMessageBox::critical(parent, title,
			QObject::tr("An item with the same name and file size is already tracked in the library:\n%1").arg(newSourcePath));
		return {};
	}

	return { .renamed = true };
}

Result renamePhotoInteractive(const MediaId& id, QWidget* parent)
{
	// A photo has no frame folder; its file IS the item, and its identity is that file's name + size. Renaming
	// here means renaming just the file's base name in place (owned: inside Photos/<label>; referenced: in the
	// user's own folder), keeping the extension - never renaming the folder the way the video path does.
	Catalog& catalog = Catalog::instance();
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

	const QString newFileName   = suffix.isEmpty() ? newBaseName : newBaseName + "." + suffix;
	const QString newSourcePath = oldInfo.absolutePath() + "/" + newFileName;

	// Refuse to clobber a different file already at the destination. A pure case change reuses the same file on a
	// case-insensitive filesystem, so it is allowed through.
	if (newSourcePath.compare(oldSourcePath, Qt::CaseInsensitive) != 0 && QFile::exists(newSourcePath))
	{
		QMessageBox::warning(parent, title, QObject::tr("A file with that name already exists:\n%1").arg(newSourcePath));
		return {};
	}

	QString message = QObject::tr("Rename “%1” to “%2”?\n\n").arg(oldInfo.fileName(), newFileName);
	message += catalog.isReferenced(id)
		? QObject::tr("• This photo is referenced in place - its file below, outside the library, will be renamed:\n  %1\n  → %2").arg(oldSourcePath, newSourcePath)
		: QObject::tr("• File:\n  %1\n  → %2").arg(oldSourcePath, newSourcePath);
	if (Catalog::instance().mediaItemHasLabel(id, Catalog::BestLabelId))
		message += QObject::tr("\n\n• Best label reference will be updated.");

	if (QMessageBox::question(parent, title, message, QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) != QMessageBox::Yes)
		return {};

	return renamePhoto(id, newSourcePath, parent);
}

} // namespace

MediaRename::Result MediaRename::renameItemInteractive(const MediaId& id, QWidget* dialogParent)
{
	if (Catalog::instance().mediaType(id) == Catalog::MediaType::Photo)
		return renamePhotoInteractive(id, dialogParent);
	return renameVideoInteractive(id, dialogParent);
}
