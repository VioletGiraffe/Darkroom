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

bool caseOnlyRenameAllowed([[maybe_unused]] QWidget* parent, [[maybe_unused]] const QString& title)
{
#if defined Q_OS_WIN || defined Q_OS_MACOS
	return true;
#else
	QMessageBox::warning(parent, title, QObject::tr("Changing only the letter case of the name is not supported on a case-sensitive file system."));
	return false;
#endif
}

[[nodiscard]] QString undoRenameOrDescribeFailure(const QString& currentPath, const QString& originalPath)
{
	if (QFile::rename(currentPath, originalPath))
		return {};
	return "\n\n" + QObject::tr("Additionally, this rename could not be undone:\n%1\n→ %2").arg(currentPath, originalPath);
}

// Rename disk objects before the catalog, rolling them back if a later step fails.
Result renameVideo(Catalog& catalog, const MediaId& oldId, const MediaId& newId,
	const QString& newName, const QString& newSourcePath, const QString& newFolderPath, QWidget* parent)
{
	const QString dialogTitle = QObject::tr("Rename media file");
	const QString oldFolderPath = catalog.folderForMediaItem(oldId);
	const QString oldSourcePath = catalog.sourcePathForMediaItem(oldId);

	const bool renameSourceFile = !oldSourcePath.isEmpty() && QFile::exists(oldSourcePath);
	if (renameSourceFile && !QFile::rename(oldSourcePath, newSourcePath))
	{
		QMessageBox::critical(parent, dialogTitle, QObject::tr("Failed to rename the source file:\n%1\n→ %2").arg(oldSourcePath, newSourcePath));
		return {};
	}

	if (!QFile::rename(oldFolderPath, newFolderPath))
	{
		QString message = QObject::tr("Failed to rename the frame folder:\n%1\n→ %2").arg(oldFolderPath, newFolderPath);
		if (renameSourceFile)
			message += undoRenameOrDescribeFailure(newSourcePath, oldSourcePath);
		QMessageBox::critical(parent, dialogTitle, message);
		return {};
	}

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
	assert_r(catalog.mediaType(id) == Catalog::MediaType::Video);

	const QString originalFolderPath = catalog.folderForMediaItem(id);

	if (originalFolderPath.isEmpty() || !QDir(originalFolderPath).exists())
	{
		QMessageBox::critical(parent, QObject::tr("Rename media file"), QObject::tr("Frame folder does not exist:\n%1").arg(originalFolderPath));
		return {};
	}

	const QString oldSourcePath = catalog.sourcePathForMediaItem(id);
	const bool sourceExists = !oldSourcePath.isEmpty() && QFile::exists(oldSourcePath);

	const QString oldBaseName = catalog.displayName(id);
	const QString parentPath  = QFileInfo(originalFolderPath).absolutePath();

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

	QString newSourcePath;
	MediaId newId = id;
	if (!oldSourcePath.isEmpty())
	{
		const QFileInfo oldSourceInfo{ oldSourcePath };
		const QString newFileName = oldSourceInfo.suffix().isEmpty() ? newBaseName : newBaseName + "." + oldSourceInfo.suffix();
		newSourcePath = oldSourceInfo.absolutePath() + "/" + newFileName;
		if (sourceExists)
			newId = MediaId::fromNameAndSize(newFileName, id.size());

		if (!caseChangeOnly && sourceExists && QFile::exists(newSourcePath))
		{
			QMessageBox::warning(parent, QObject::tr("Rename media file"), QObject::tr("A file with that name already exists:\n%1").arg(newSourcePath));
			return {};
		}
	}

	const QString newFolderPath = parentPath + "/" + Catalog::frameFolderName(newBaseName, newId);
	if (!caseChangeOnly && QDir(newFolderPath).exists())
	{
		QMessageBox::warning(parent, QObject::tr("Rename media file"), QObject::tr("A folder with that name already exists:\n%1").arg(newFolderPath));
		return {};
	}

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
	const QString folderAbs     = catalog.folderForMediaItem(oldId);

	if (!QFile::rename(oldSourcePath, newSourcePath))
	{
		QMessageBox::critical(parent, title, QObject::tr("Failed to rename the file:\n%1\n→ %2").arg(oldSourcePath, newSourcePath));
		return {};
	}

	const MediaId newId = MediaId::fromFile(newSourcePath);

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
	assert_r(catalog.mediaType(id) == Catalog::MediaType::Photo);

	const QString title = QObject::tr("Rename photo");
	const QString oldSourcePath = catalog.sourcePathForMediaItem(id);

	if (oldSourcePath.isEmpty() || !QFile::exists(oldSourcePath))
	{
		QMessageBox::warning(parent, title, QObject::tr("The photo file no longer exists, so it cannot be renamed:\n%1").arg(oldSourcePath));
		return {};
	}

	const QFileInfo oldInfo{ oldSourcePath };
	const QString oldBaseName = oldInfo.completeBaseName();
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
