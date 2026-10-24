#include "Windows/MediaItemManagement.h"
#include "Core/Catalog.h"
#include "Windows/PathDeletion.h"

#include "compiler/compiler_warnings_control.h"
#include "dialogs/messagedialog.h"

DISABLE_COMPILER_WARNINGS
#include <QFileInfo>
#include <QMessageBox>
#include <QObject>
#include <QSet>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <vector>

namespace {

enum class SourceFiles { Keep, Trash };

// A video's frame folder always goes to Trash; a photo's folder is shared by siblings and is never touched.
MediaItemManagement::RemovalResult removeItems(
	Catalog& catalog, const std::vector<MediaItemId>& items, SourceFiles sourceFiles, QWidget* dialogParent)
{
	Catalog::ChangeBatchScope catalogChanges(catalog);
	const bool trashSources = sourceFiles == SourceFiles::Trash;

	struct ItemPaths
	{
		MediaItemId id;
		bool isPhoto;
		QString sourcePath;
		QString frameFolder; // empty for a photo
	};
	std::vector<ItemPaths> itemPaths;
	QStringList photoFilesAndFrameFolders;
	for (const MediaItemId id : items)
	{
		const bool isPhoto = catalog.mediaType(id) == Catalog::MediaType::Photo;
		itemPaths.push_back({ id, isPhoto, catalog.sourcePathForMediaItem(id), isPhoto ? QString() : catalog.folderForMediaItem(id) });
		if (!isPhoto || trashSources)
			photoFilesAndFrameFolders.push_back(isPhoto ? itemPaths.back().sourcePath : itemPaths.back().frameFolder);
	}
	QSet<QString> removedPaths = PathDeletion::removePathsInteractive(photoFilesAndFrameFolders, PathDeletion::Mode::Trash, dialogParent);

	if (trashSources)
	{
		// A video's source file is attempted only once its frame folder is gone.
		QStringList videoSourceFiles;
		for (const ItemPaths& item : itemPaths)
		{
			if (!item.isPhoto && removedPaths.contains(item.frameFolder))
				videoSourceFiles.push_back(item.sourcePath);
		}
		removedPaths.unite(PathDeletion::removePathsInteractive(videoSourceFiles, PathDeletion::Mode::Trash, dialogParent));
	}

	MediaItemManagement::RemovalResult result;
	QStringList failedItems;
	{
		Catalog::BatchScope batch(catalog);
		for (const ItemPaths& item : itemPaths)
		{
			QStringList failedParts;
			if (item.isPhoto)
			{
				if (trashSources && !removedPaths.contains(item.sourcePath))
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
					if (trashSources && !item.sourcePath.isEmpty())
						failedParts << QObject::tr("• Source file not attempted: %1").arg(item.sourcePath);
				}
				else if (trashSources && !item.sourcePath.isEmpty() && !removedPaths.contains(item.sourcePath))
				{
					failedParts << QObject::tr("• Source file: %1").arg(item.sourcePath);
				}
			}

			if (failedParts.empty())
			{
				catalog.removeMediaItem(item.id);
				result.removedItems.push_back(item.id);
			}
			else
			{
				result.storageRefreshRequired = true;
				failedItems << QObject::tr("%1:\n%2").arg(MediaItemManagement::itemDisplayName(catalog, item.id), failedParts.join("\n"));
			}
		}
	}

	if (!failedItems.empty())
	{
		MessageDialog::notice(dialogParent, trashSources ? QObject::tr("Delete incomplete") : QObject::tr("Remove incomplete"),
			trashSources ? QObject::tr("Some items could not be fully deleted. Their catalog records were kept:")
			             : QObject::tr("Some frame folders could not be moved to Trash. Their catalog records were kept:"),
			failedItems.join("\n\n"), QMessageBox::Critical);
	}
	return result;
}

} // namespace

QString MediaItemManagement::itemDisplayName(const Catalog& catalog, MediaItemId id)
{
	return catalog.mediaType(id) == Catalog::MediaType::Photo ? QFileInfo(catalog.sourcePathForMediaItem(id)).fileName() : catalog.displayName(id);
}

QString MediaItemManagement::bulletedItemNameList(const Catalog& catalog, const std::vector<MediaItemId>& items)
{
	QString list;
	constexpr size_t maxListed = 15;
	for (size_t i = 0; i < std::min(maxListed, items.size()); ++i)
		list += "\n• " + itemDisplayName(catalog, items[i]);
	if (items.size() > maxListed)
		list += "\n" + QObject::tr("... and %1 more").arg(items.size() - maxListed);
	return list;
}

MediaItemManagement::RemovalResult MediaItemManagement::deleteItemsInteractive(
	Catalog& catalog, const std::vector<MediaItemId>& selection, QWidget* dialogParent)
{
	if (selection.empty())
		return {};

	QString message;
	if (selection.size() == 1)
	{
		const MediaItemId id = selection.front();
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
		for (const MediaItemId id : selection)
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

	return removeItems(catalog, selection, SourceFiles::Trash, dialogParent);
}

MediaItemManagement::RemovalResult MediaItemManagement::removeItemsFromLibraryInteractive(
	Catalog& catalog, const std::vector<MediaItemId>& selection, QWidget* dialogParent)
{
	if (selection.empty())
		return {};

	QString message;
	if (selection.size() == 1)
	{
		const MediaItemId id = selection.front();
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
	const bool anyVideo = std::ranges::any_of(selection, [&catalog](MediaItemId id) { return catalog.mediaType(id) == Catalog::MediaType::Video; });
	message += "\n\n" + (anyVideo
		? QObject::tr("Video frames and previews will be moved to Trash. Source files are kept, but labels and other catalog metadata will be discarded. Continue?")
		: QObject::tr("No files will be deleted, but labels and other catalog metadata will be discarded. Continue?"));

	if (QMessageBox::question(dialogParent, QObject::tr("Remove from library"), message,
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
		return {};

	return removeItemsFromLibrary(catalog, selection, dialogParent);
}

MediaItemManagement::RemovalResult MediaItemManagement::removeItemsFromLibrary(
	Catalog& catalog, const std::vector<MediaItemId>& items, QWidget* dialogParent)
{
	return removeItems(catalog, items, SourceFiles::Keep, dialogParent);
}
