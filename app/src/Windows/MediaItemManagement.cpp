#include "Windows/MediaItemManagement.h"
#include "Core/Catalog.h"
#include "Utils.h"

#include "dialogs/messagebox.h"

#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QObject>

#include <algorithm>

namespace {

[[nodiscard]] bool deleteFileIfPresent(const QString& filePath)
{
	if (filePath.isEmpty())
		return false;

	const QFileInfo info(filePath);
	return (!info.exists() && !info.isSymLink()) || QFile::remove(filePath);
}

QString bulletedItemNameList(const Catalog& catalog, const std::vector<MediaId>& selection)
{
	QString list;
	constexpr size_t maxListed = 15;
	for (size_t i = 0; i < std::min(maxListed, selection.size()); ++i)
	{
		const MediaId& id = selection[i];
		list += "\n• " + (catalog.mediaType(id) == Catalog::MediaType::Photo ? id.name() : catalog.displayName(id));
	}
	if (selection.size() > maxListed)
		list += "\n" + QObject::tr("... and %1 more").arg(selection.size() - maxListed);
	return list;
}

} // namespace

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
			message = QObject::tr("This will permanently delete:\n\n• %1").arg(sourcePath);
		}
		else
		{
			message = QObject::tr("This will permanently delete:\n\n• %1").arg(catalog.folderForMediaItem(id));
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
		message = QObject::tr("This will permanently delete %1 items - %2:\n")
			.arg(selection.size()).arg(deletedKinds.join(", "));
		message += bulletedItemNameList(catalog, selection);
	}
	message += QObject::tr("\n\nThis cannot be undone. Continue?");

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
				if (!deleteFileIfPresent(sourcePath))
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

				const bool folderDeleted = deleteFolderRecursivelyIfPresent(folderPath);
				if (!folderDeleted)
				{
					failedParts << (folderPath.isEmpty()
						? QObject::tr("• Frame folder path is missing.")
						: QObject::tr("• Frame folder: %1").arg(folderPath));
					if (!sourcePath.isEmpty())
						failedParts << QObject::tr("• Source file not attempted: %1").arg(sourcePath);
				}
				else if (!sourcePath.isEmpty() && !deleteFileIfPresent(sourcePath))
				{
					failedParts << QObject::tr("• Source file: %1").arg(sourcePath);
				}
			}

			if (failedParts.empty())
				catalog.removeMediaItem(id);
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
