#include "Windows/MediaItemManagement.h"
#include "Core/Catalog.h"

#include "compiler/compiler_warnings_control.h"
#include "dialogs/messagebox.h"

DISABLE_COMPILER_WARNINGS
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QObject>
#include <QPushButton>
RESTORE_COMPILER_WARNINGS

#include <algorithm>

namespace {

[[nodiscard]] bool pathExistsOrIsSymlink(const QString& path)
{
	const QFileInfo info(path);
	return info.exists() || info.isSymLink();
}

bool permanentlyRemovePath(const QString& path, QString* error)
{
	const QFileInfo info(path);
	if (!info.exists() && !info.isSymLink())
		return true;

	if (info.isDir() && !info.isSymLink())
	{
		if (QDir(path).removeRecursively())
			return true;
		if (error)
			*error = QObject::tr("The folder could not be removed.");
		return false;
	}

	QFile file(path);
	if (file.remove())
		return true;
	if (error)
		*error = file.errorString().trimmed();
	return false;
}

} // namespace

QString MediaItemManagement::itemDisplayName(const Catalog& catalog, const MediaId& id)
{
	return catalog.mediaType(id) == Catalog::MediaType::Photo ? id.name() : catalog.displayName(id);
}

QString MediaItemManagement::bulletedItemNameList(const Catalog& catalog, const std::vector<MediaId>& items)
{
	QString list;
	constexpr size_t maxListed = 15;
	for (size_t i = 0; i < std::min(maxListed, items.size()); ++i)
		list += "\n• " + itemDisplayName(catalog, items[i]);
	if (items.size() > maxListed)
		list += "\n" + QObject::tr("... and %1 more").arg(items.size() - maxListed);
	return list;
}

bool MediaItemManagement::removePathTrashFirstInteractive(const QString& path, QWidget* dialogParent)
{
	if (path.isEmpty())
		return false;
	if (!pathExistsOrIsSymlink(path))
		return true;

	QFile file(path);
	if (file.moveToTrash())
		return true;

	QString text = QObject::tr("This item could not be moved to Trash:\n\n%1\n\nPermanently delete it instead? This cannot be undone.")
		.arg(QDir::toNativeSeparators(path));
	const QString trashError = file.errorString().trimmed();
	if (!trashError.isEmpty())
		text += "\n\n" + trashError;

	QMessageBox fallback(QMessageBox::Warning, QObject::tr("Move to Trash failed"), text, QMessageBox::NoButton, dialogParent);
	QPushButton* permanentlyDelete = fallback.addButton(QObject::tr("Delete Permanently"), QMessageBox::DestructiveRole);
	QPushButton* cancel = fallback.addButton(QMessageBox::Cancel);
	fallback.setDefaultButton(cancel);
	fallback.setEscapeButton(cancel);
	fallback.exec();
	if (fallback.clickedButton() != permanentlyDelete)
		return false;

	QString permanentError;
	if (permanentlyRemovePath(path, &permanentError))
		return true;

	MessageBox::notice(dialogParent, QObject::tr("Permanent deletion failed"),
		QObject::tr("The item could not be permanently deleted:"),
		QDir::toNativeSeparators(path) + (permanentError.isEmpty() ? QString() : "\n" + permanentError), QMessageBox::Critical);
	return false;
}

MediaItemManagement::DeleteResult MediaItemManagement::deleteItemsInteractive(
	Catalog& catalog, const std::vector<MediaId>& selection, QWidget* dialogParent)
{
	if (selection.empty())
		return {};

	QString message;
	if (selection.size() == 1)
	{
		const MediaId& id = selection.front();
		const QString sourcePath = catalog.sourcePathForMediaItem(id);
		if (catalog.mediaType(id) == Catalog::MediaType::Photo)
		{
			message = QObject::tr("Move this photo to Trash?\n\n• %1").arg(sourcePath);
		}
		else
		{
			message = QObject::tr("Move this video's frame folder and source file to Trash?\n\n• %1").arg(catalog.folderForMediaItem(id));
			if (!sourcePath.isEmpty())
				message += "\n• " + sourcePath;
		}
	}
	else
	{
		bool anyVideo = false;
		bool anyPhoto = false;
		for (const MediaId& id : selection)
		{
			if (catalog.mediaType(id) == Catalog::MediaType::Video)
				anyVideo = true;
			else
				anyPhoto = true;
		}

		QStringList deletedKinds;
		if (anyVideo)
			deletedKinds << QObject::tr("each video's frame folder and source file");
		if (anyPhoto)
			deletedKinds << QObject::tr("each photo's file");
		message = QObject::tr("Move %1 items to Trash - %2:\n")
			.arg(selection.size()).arg(deletedKinds.join(", "));
		message += bulletedItemNameList(catalog, selection);
	}

	if (QMessageBox::warning(dialogParent, QObject::tr("Delete"), message,
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
		return {};

	Catalog::ChangeBatchScope catalogChanges(catalog);
	DeleteResult result;
	QStringList failedItems;
	{
		// Photo folders are shared by siblings and must never be deleted here.
		Catalog::BatchScope batch(catalog);
		for (const MediaId& id : selection)
		{
			const QString sourcePath = catalog.sourcePathForMediaItem(id);
			QStringList failedParts;
			if (catalog.mediaType(id) == Catalog::MediaType::Photo)
			{
				if (!removePathTrashFirstInteractive(sourcePath, dialogParent))
				{
					failedParts << (sourcePath.isEmpty()
						? QObject::tr("• Photo file path is missing.")
						: QObject::tr("• Photo file: %1").arg(sourcePath));
				}
			}
			else
			{
				const QString folderPath = catalog.folderForMediaItem(id);
				if (!folderPath.isEmpty())
					result.affectedFrameFolders << folderPath;

				if (!removePathTrashFirstInteractive(folderPath, dialogParent))
				{
					failedParts << (folderPath.isEmpty()
						? QObject::tr("• Frame folder path is missing.")
						: QObject::tr("• Frame folder: %1").arg(folderPath));
					if (!sourcePath.isEmpty())
						failedParts << QObject::tr("• Source file not attempted: %1").arg(sourcePath);
				}
				else if (!sourcePath.isEmpty() && !removePathTrashFirstInteractive(sourcePath, dialogParent))
				{
					failedParts << QObject::tr("• Source file: %1").arg(sourcePath);
				}
			}

			if (failedParts.empty())
			{
				catalog.removeMediaItem(id);
				result.deletedItems.push_back(id);
			}
			else
			{
				result.storageRefreshRequired = true;
				failedItems << QObject::tr("%1:\n%2").arg(id.name(), failedParts.join("\n"));
			}
		}
	}

	if (!failedItems.empty())
	{
		MessageBox::notice(dialogParent, QObject::tr("Delete incomplete"),
			QObject::tr("Some items could not be fully deleted. Their catalog records were kept:"),
			failedItems.join("\n\n"), QMessageBox::Critical);
	}
	return result;
}

void MediaItemManagement::removeItemsFromLibraryInteractive(
	Catalog& catalog, const std::vector<MediaId>& selection, QWidget* dialogParent)
{
	if (selection.empty())
		return;

	QString message;
	if (selection.size() == 1)
	{
		const MediaId& id = selection.front();
		message = QObject::tr("This will remove the item from the library:\n");
		if (catalog.mediaType(id) == Catalog::MediaType::Video)
			message += "\n• " + catalog.folderForMediaItem(id);
		const QString sourcePath = catalog.sourcePathForMediaItem(id);
		if (!sourcePath.isEmpty())
			message += "\n• " + sourcePath;
	}
	else
	{
		message = QObject::tr("This will remove %1 items from the library:\n").arg(selection.size());
		message += bulletedItemNameList(catalog, selection);
	}
	message += "\n\n" + QObject::tr("No files will be deleted, but labels and other catalog metadata will be discarded. Continue?");

	if (QMessageBox::question(dialogParent, QObject::tr("Remove from library"), message,
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
		return;

	Catalog::ChangeBatchScope catalogChanges(catalog);
	Catalog::BatchScope batch(catalog);
	for (const MediaId& id : selection)
		catalog.removeMediaItem(id);
}
