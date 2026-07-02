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
	if (stagedPreviewDir.isEmpty() || !copyPreviewFrames(stagedPreviewDir + "/preview", outputFolder + "/preview"))
	{
		const int previewFrameCount = QSettings{}.value(Settings::PreviewFrameCount, Defaults::PreviewFrameCount).toInt();
		Ffmpeg::generatePreviewFrames(videoPath, outputFolder, previewFrameCount);
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
