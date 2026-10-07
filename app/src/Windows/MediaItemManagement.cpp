#include "Windows/MediaItemManagement.h"
#include "Core/Catalog.h"
#include "Windows/PathDeletion.h"

#include "compiler/compiler_warnings_control.h"
#include "dialogs/messagedialog.h"

DISABLE_COMPILER_WARNINGS
#include <QMessageBox>
#include <QObject>
#include <QSet>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <vector>

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

	struct ItemPaths
	{
		MediaId id;
		bool isPhoto;
		QString sourcePath;
		QString frameFolder; // empty for a photo
	};
	std::vector<ItemPaths> items;
	QStringList photoFilesAndFrameFolders;
	for (const MediaId& id : selection)
	{
		// Photo folders are shared by siblings and must never be deleted here.
		const bool isPhoto = catalog.mediaType(id) == Catalog::MediaType::Photo;
		items.push_back({ id, isPhoto, catalog.sourcePathForMediaItem(id), isPhoto ? QString() : catalog.folderForMediaItem(id) });
		photoFilesAndFrameFolders.push_back(isPhoto ? items.back().sourcePath : items.back().frameFolder);
	}
	QSet<QString> removedPaths = PathDeletion::removePathsInteractive(photoFilesAndFrameFolders, PathDeletion::Mode::Trash, dialogParent);

	// A video's source file is attempted only once its frame folder is gone.
	QStringList videoSourceFiles;
	for (const ItemPaths& item : items)
	{
		if (!item.isPhoto && removedPaths.contains(item.frameFolder))
			videoSourceFiles.push_back(item.sourcePath);
	}
	removedPaths.unite(PathDeletion::removePathsInteractive(videoSourceFiles, PathDeletion::Mode::Trash, dialogParent));

	DeleteResult result;
	QStringList failedItems;
	{
		Catalog::BatchScope batch(catalog);
		for (const ItemPaths& item : items)
		{
			QStringList failedParts;
			if (item.isPhoto)
			{
				if (!removedPaths.contains(item.sourcePath))
				{
					failedParts << (item.sourcePath.isEmpty()
						? QObject::tr("• Photo file path is missing.")
						: QObject::tr("• Photo file: %1").arg(item.sourcePath));
				}
			}
			else
			{
				if (!item.frameFolder.isEmpty())
					result.affectedFrameFolders << item.frameFolder;

				if (!removedPaths.contains(item.frameFolder))
				{
					failedParts << (item.frameFolder.isEmpty()
						? QObject::tr("• Frame folder path is missing.")
						: QObject::tr("• Frame folder: %1").arg(item.frameFolder));
					if (!item.sourcePath.isEmpty())
						failedParts << QObject::tr("• Source file not attempted: %1").arg(item.sourcePath);
				}
				else if (!item.sourcePath.isEmpty() && !removedPaths.contains(item.sourcePath))
				{
					failedParts << QObject::tr("• Source file: %1").arg(item.sourcePath);
				}
			}

			if (failedParts.empty())
			{
				catalog.removeMediaItem(item.id);
				result.deletedItems.push_back(item.id);
			}
			else
			{
				result.storageRefreshRequired = true;
				failedItems << QObject::tr("%1:\n%2").arg(item.id.name(), failedParts.join("\n"));
			}
		}
	}

	if (!failedItems.empty())
	{
		MessageDialog::notice(dialogParent, QObject::tr("Delete incomplete"),
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
