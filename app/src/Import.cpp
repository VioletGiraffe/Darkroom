#include "Import.h"
#include "Core/Catalog.h"
#include "Core/MediaId.h"
#include "Ffmpeg.h"
#include "Settings.h"
#include "Utils.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QSettings>

// Copies preview frames from an existing preview/ dir into a fresh one at the destination (created as needed),
// returning true only if at least one frame was copied. A false return - nothing to reuse - tells import
// to fall back to a fresh ffmpeg extraction. The destination is always a just-created empty folder here
// (callers wipe/recreate the output folder first), so QFile::copy never has to overwrite.
static bool copyPreviewFrames(const QString& srcPreviewDir, const QString& dstPreviewDir)
{
	const QDir src{ srcPreviewDir };
	const QStringList frames = src.entryList(IMAGE_FILE_FILTERS, QDir::Files);
	if (frames.isEmpty() || !QDir{}.mkpath(dstPreviewDir))
		return false;

	bool copiedAny = false;
	for (const QString& frame : frames)
		copiedAny |= QFile::copy(src.filePath(frame), dstPreviewDir + "/" + frame);
	return copiedAny;
}

Import::Result Import::importVideo(const QString& videoPath, const QString& collectionPath, const QString& stagedPreviewDir, bool overwriteExisting)
{
	QFileInfo videoInfo(videoPath);
	if (!videoInfo.exists())
		return { Status::Error, QObject::tr("Video file does not exist:\n%1").arg(videoPath) };

	// Create output folder
	const QString baseName = videoInfo.completeBaseName();
	const QString outputFolder = collectionPath + "/" + baseName;
	if (!QDir{}.mkpath(collectionPath))
		return { Status::Error, QObject::tr("Failed to create collection folder:\n%1").arg(collectionPath) };

	if (QDir{ outputFolder }.exists())
	{
		if (!overwriteExisting)
			return { Status::FolderConflict };
		// A failed wipe aborts the item rather than proceed and mix the stale frames with the new ones
		if (!QDir(outputFolder).removeRecursively())
			return { Status::Error, QObject::tr("Failed to delete folder: %1").arg(outputFolder) };
	}

	if (!QDir{}.mkpath(outputFolder))
		return { Status::Error, QObject::tr("Failed to create output folder:\n%1").arg(outputFolder) };

	// The full frame set is extracted on demand (see MainWindow::ensureFramesSplit), not here - import only
	// needs a few permanent preview frames up front, then registers the video right away. Quick Import already
	// extracted exactly these for its staging card, so reuse them by copy; fall back to a fresh ffmpeg pass only
	// when there's nothing staged to reuse (not reached via Quick Import, or staging's probe produced no frames).
	// The staged scratch dir holds those frames directly; the frame folder nests its own under preview/.
	if (stagedPreviewDir.isEmpty() || !copyPreviewFrames(stagedPreviewDir, Catalog::previewDirFor(outputFolder)))
	{
		const int previewFrameCount = QSettings{}.value(Settings::PreviewFrameCount, Defaults::PreviewFrameCount).toInt();
		Ffmpeg::generatePreviewFrames(videoPath, Catalog::previewDirFor(outputFolder), previewFrameCount);
	}

	if (!Catalog::instance().addMediaItem(MediaId::fromFile(videoPath), videoPath, outputFolder, /*splitIntoFrames=*/false))
	{
		QString message = QObject::tr("An item with the same name and file size is already tracked in a different collection:\n%1").arg(videoPath);
		if (!QDir(outputFolder).removeRecursively())
			message += "\n\n" + QObject::tr("Additionally, failed to clean up the created folder:\n%1").arg(outputFolder);
		return { Status::Error, message };
	}

	return {};
}

Import::PhotoResult Import::importPhoto(const QString& photoPath, const QString& labelDisplayName, PhotoImportMode mode)
{
	const QFileInfo photoInfo(photoPath);
	if (!photoInfo.isFile())
		return { PhotoStatus::Error, QObject::tr("Photo file does not exist:\n%1").arg(photoPath), {} };

	Catalog& catalog = Catalog::instance();

	if (mode == PhotoImportMode::Reference)
	{
		const MediaId id = MediaId::fromFile(photoPath);
		// A same-path re-import (this exact file already tracked in place) falls through to the upsert below;
		// any other tracked use of the id is a collision Reference mode cannot resolve - there is no owned
		// copy whose name a rename could change.
		if (catalog.containsMediaItem(id) && !(catalog.isReferenced(id) && catalog.sourcePathForMediaItem(id) == photoPath))
			return { PhotoStatus::IdCollision, {}, {} };

		if (!catalog.addPhoto(id, photoPath, /*labelDirAbs=*/{}, /*referenced=*/true))
			return { PhotoStatus::Error, QObject::tr("An item with the same name and file size is already tracked:\n%1").arg(photoPath), {} };
		return { PhotoStatus::Success, {}, id };
	}

	// Owned import: land the file in the label's photo dir, auto-renaming around collisions.
	const QString destDir = photosRootFolder() + "/" + labelDisplayName;
	if (!QDir{}.mkpath(destDir))
		return { PhotoStatus::Error, QObject::tr("Failed to create photo folder:\n%1").arg(destDir), {} };

	// Pick a destination name that is free both on disk and in the catalog - the id is name+size and a
	// copy/move preserves the size, so one rename resolves a path collision and an id collision alike. A
	// byte-identical file already at the destination (including this very file on a re-import) is adopted
	// as-is instead of being copied again.
	const QString baseName = photoInfo.completeBaseName();
	const QString suffix   = photoInfo.suffix();
	QString destPath;
	bool adoptExisting = false;
	for (int attempt = 1; ; ++attempt)
	{
		if (attempt > 9999)
			return { PhotoStatus::Error, QObject::tr("Could not find a free file name for:\n%1\nin:\n%2").arg(photoPath, destDir), {} };

		const QString candidateName = attempt == 1 ? photoInfo.fileName()
		                                           : baseName + "_" + QString::number(attempt) + "." + suffix;
		const QString candidatePath = destDir + "/" + candidateName;
		if (QFile::exists(candidatePath))
		{
			if (!filesAreIdentical(photoPath, candidatePath))
				continue;  // the name is taken by different content - try the next numbered name
			destPath = candidatePath;
			adoptExisting = true;
			break;
		}
		if (catalog.containsMediaItem(MediaId::fromNameAndSize(candidateName, photoInfo.size())))
			continue;  // the id is taken by an item stored elsewhere (or a drifted record) - a fresh name sidesteps it
		destPath = candidatePath;
		break;
	}

	if (!adoptExisting)
	{
		const bool isMove = mode == PhotoImportMode::Move;
		if (!(isMove ? QFile::rename(photoPath, destPath) : QFile::copy(photoPath, destPath)))
			return { PhotoStatus::Error, QObject::tr("Failed to %1:\n%2\nto:\n%3")
				.arg(isMove ? QObject::tr("move") : QObject::tr("copy"), photoPath, destPath), {} };
	}

	const MediaId registeredId = MediaId::fromFile(destPath);
	if (!catalog.addPhoto(registeredId, destPath, destDir, /*referenced=*/false))
	{
		// Undo the relocation rather than leave an untracked copy behind (mirrors importVideo's cleanup).
		if (!adoptExisting)
		{
			if (mode == PhotoImportMode::Move)
				QFile::rename(destPath, photoPath);
			else
				QFile::remove(destPath);
		}
		return { PhotoStatus::Error, QObject::tr("An item with the same name and file size is already tracked elsewhere:\n%1").arg(photoPath), {} };
	}

	return { PhotoStatus::Success, {}, registeredId };
}
