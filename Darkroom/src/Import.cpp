#include "Import.h"
#include "Core/Catalog.h"
#include "Ffmpeg.h"
#include "DarkroomSettings.h"
#include "Utils.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QSettings>
RESTORE_COMPILER_WARNINGS

// False tells import to extract afresh. dstPreviewDir is always initially empty.
static bool copyPreviewFrames(const QString& srcPreviewDir, const QString& dstPreviewDir)
{
	const QDir src{ srcPreviewDir };
	const QStringList frames = listFrameImageFiles(src);
	if (frames.isEmpty() || !QDir{}.mkpath(dstPreviewDir))
		return false;

	bool copiedAny = false;
	for (const QString& frame : frames)
		copiedAny |= QFile::copy(src.filePath(frame), dstPreviewDir + "/" + frame);
	return copiedAny;
}

Import::Result Import::importVideo(Catalog& catalog, const QString& videoPath, const QString& storageFolderPath,
	const QString& stagedPreviewDir, bool overwriteExisting, qint64 stagedDurationMs)
{
	if (!QFileInfo::exists(videoPath))
		return { Status::Error, QObject::tr("Video file does not exist:\n%1").arg(videoPath) };
	if (catalog.mediaItemAtSourcePath(videoPath) != MediaItemId::None)
		return { Status::Error, QObject::tr("This video is already in the library:\n%1").arg(videoPath) };

	const QString outputFolder = catalog.frameFolderForNewVideo(storageFolderPath, videoPath);
	if (!QDir{}.mkpath(storageFolderPath))
		return { Status::Error, QObject::tr("Failed to create label folder:\n%1").arg(storageFolderPath) };

	if (QDir{ outputFolder }.exists())
	{
		if (!overwriteExisting)
			return { Status::FolderConflict };
		if (!QDir(outputFolder).removeRecursively())
			return { Status::Error, QObject::tr("Failed to delete folder: %1").arg(outputFolder) };
	}

	if (!QDir{}.mkpath(outputFolder))
		return { Status::Error, QObject::tr("Failed to create output folder:\n%1").arg(outputFolder) };

	// Full extraction is deferred; reuse the staging preview and its probe result when possible.
	qint64 durationMs = stagedDurationMs;
	if (stagedPreviewDir.isEmpty() || !copyPreviewFrames(stagedPreviewDir, Catalog::previewDirFor(outputFolder)))
	{
		const int previewFrameCount = QSettings{}.value(Settings::PreviewFrameCount, Defaults::PreviewFrameCount).toInt();
		durationMs = Ffmpeg::generatePreviewFrames(videoPath, Catalog::previewDirFor(outputFolder), previewFrameCount).durationMs;
	}

	const MediaItemId id = catalog.addMediaItem(videoPath, outputFolder, /*splitIntoFrames=*/false, durationMs);
	if (id == MediaItemId::None)
	{
		QString message = QObject::tr("The video could not be registered in the library:\n%1").arg(videoPath);
		if (!QDir(outputFolder).removeRecursively())
			message += "\n\n" + QObject::tr("Additionally, failed to clean up the created folder:\n%1").arg(outputFolder);
		return { Status::Error, message };
	}

	return { Status::Success, {}, id };
}

Import::PhotoResult Import::importPhoto(Catalog& catalog, const QString& labelPhotoFolder, const QString& photoPath, PhotoImportMode mode)
{
	const QFileInfo photoInfo(photoPath);
	if (!photoInfo.isFile())
		return { PhotoStatus::Error, QObject::tr("Photo file does not exist:\n%1").arg(photoPath), {} };

	if (mode == PhotoImportMode::Reference)
	{
		if (const MediaItemId existing = catalog.mediaItemAtSourcePath(photoPath); existing != MediaItemId::None)
			return { PhotoStatus::Success, {}, existing };

		const MediaItemId id = catalog.addPhoto(photoPath, /*labelDirAbs=*/{}, /*referenced=*/true);
		if (id == MediaItemId::None)
			return { PhotoStatus::Error, QObject::tr("The photo could not be registered in the library:\n%1").arg(photoPath), {} };
		return { PhotoStatus::Success, {}, id };
	}

	if (!QDir{}.mkpath(labelPhotoFolder))
		return { PhotoStatus::Error, QObject::tr("Failed to create photo folder:\n%1").arg(labelPhotoFolder), {} };

	const QString baseName = photoInfo.completeBaseName();
	const QString suffix   = photoInfo.suffix();
	QString destPath;
	bool adoptExisting = false;
	for (int attempt = 1; ; ++attempt)
	{
		if (attempt > 9999)
			return { PhotoStatus::Error, QObject::tr("Could not find a free file name for:\n%1\nin:\n%2").arg(photoPath, labelPhotoFolder), {} };

		const QString candidateName = attempt == 1 ? photoInfo.fileName()
		                                           : baseName + "_" + QString::number(attempt) + "." + suffix;
		const QString candidatePath = labelPhotoFolder + "/" + candidateName;
		// A tracked photo whose file is missing still owns its path.
		const MediaItemId trackedThere = catalog.mediaItemAtSourcePath(candidatePath);
		const bool occupied = QFile::exists(candidatePath);
		if (occupied && filesAreIdentical(photoPath, candidatePath))
		{
			if (trackedThere != MediaItemId::None)
				return { PhotoStatus::Success, {}, trackedThere };
			destPath = candidatePath;
			adoptExisting = true;
			break;
		}
		if (occupied || trackedThere != MediaItemId::None)
			continue;
		destPath = candidatePath;
		break;
	}

	if (!adoptExisting)
	{
		const bool isMove = mode == PhotoImportMode::Move;
		if (!(isMove ? QFile{ photoPath }.rename(destPath) : QFile::copy(photoPath, destPath)))
			return { PhotoStatus::Error, QObject::tr("Failed to %1:\n%2\nto:\n%3")
				.arg(isMove ? QObject::tr("move") : QObject::tr("copy"), photoPath, destPath), {} };
	}

	const MediaItemId registeredId = catalog.addPhoto(destPath, labelPhotoFolder, /*referenced=*/false);
	if (registeredId == MediaItemId::None)
	{
		QString message = QObject::tr("The photo could not be registered in the library:\n%1").arg(photoPath);
		if (!adoptExisting && !(mode == PhotoImportMode::Move ? QFile{ destPath }.rename(photoPath) : QFile::remove(destPath)))
			message += "\n\n" + QObject::tr("Additionally, the imported file could not be cleaned up and remains at:\n%1").arg(destPath);
		return { PhotoStatus::Error, message, {} };
	}

	return { PhotoStatus::Success, {}, registeredId };
}
