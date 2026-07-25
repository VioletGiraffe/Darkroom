#include "Windows/FrameExtraction.h"
#include "Core/Catalog.h"
#include "Ffmpeg.h"
#include "Settings.h"
#include "Utils.h"

#include "assert/advanced_assert.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QObject>
#include <QScopeGuard>
#include <QSettings>
#include <QUuid>

#include <vector>

namespace {

Ffmpeg::SplitOptions configuredSplitOptions()
{
	return {
		.tiff = QSettings{}.value(Settings::UseTiff, Defaults::UseTiff).toBool(),
		.jpegQuality = QSettings{}.value(Settings::JpegQuality, Defaults::JpegQuality).toInt(),
		.frameStep = QSettings{}.value(Settings::FrameStep, Defaults::FrameStep).toInt(),
	};
}

bool extractFramesOrReport(const QString& videoFilePath, const QString& outputFolder, QWidget* dialogParent)
{
	const Ffmpeg::SplitResult result =
		Ffmpeg::splitVideoIntoFrames(videoFilePath, outputFolder, configuredSplitOptions());
	if (result.ok())
		return true;

	reportFfmpegFailure(dialogParent, result, videoFilePath, outputFolder);
	return false;
}

bool regeneratePreviewFromRealFrames(const QString& folderPath, int previewFrameCount)
{
	QDir folderDir(folderPath);
	const QStringList realFrames = listFrameImageFiles(folderDir);
	if (realFrames.empty())
		return false;

	const QString previewFolder = Catalog::previewDirFor(folderPath);
	if (!QDir{}.mkpath(previewFolder))
		return false;

	// Partial previews are useful, so attempt every copy and succeed if any landed.
	bool anyCopied = false;
	for (const QString& sourceFrame : pickEvenlySpacedFrames(folderDir, realFrames, previewFrameCount))
	{
		if (QFile::copy(sourceFrame, previewFolder + "/" + QFileInfo(sourceFrame).fileName()))
			anyCopied = true;
	}
	return anyCopied;
}

} // namespace

bool FrameExtraction::ensureFramesExtracted(
	Catalog& catalog, const MediaId& id, int previewFrameCount, QWidget* dialogParent)
{
	assert_and_return_r(catalog.containsMediaItem(id), false);
	assert_and_return_r(catalog.mediaType(id) == Catalog::MediaType::Video, false);
	assert_and_return_r(!catalog.folderForMediaItem(id).isEmpty(), false);
	if (catalog.isSplitIntoFrames(id))
		return true;

	return reextractVideoFrames(
		catalog, id, PreviewHandling::PreserveExisting, previewFrameCount, dialogParent);
}

bool FrameExtraction::reextractVideoFrames(Catalog& catalog, const MediaId& id,
	PreviewHandling previewHandling, int previewFrameCount, QWidget* dialogParent)
{
	assert_and_return_r(catalog.containsMediaItem(id), false);
	assert_and_return_r(catalog.mediaType(id) == Catalog::MediaType::Video, false);
	const QString videoFilePath = catalog.sourcePathForMediaItem(id);
	const QString outputFolder = catalog.folderForMediaItem(id);
	assert_and_return_r(!outputFolder.isEmpty(), false);  // QDir("") addresses the working directory
	const bool hadExistingFolder = QDir(outputFolder).exists();
	const QString preservedFolder = hadExistingFolder
		? QFileInfo(outputFolder).dir().filePath(".darkroom-resplit-" + QUuid::createUuid().toString(QUuid::Id128))
		: QString{};

	if (hadExistingFolder && !QDir{}.rename(outputFolder, preservedFolder))
	{
		QMessageBox::critical(dialogParent, QObject::tr("Error"),
			QObject::tr("Failed to preserve the existing frame folder before replacing it:\n%1").arg(outputFolder));
		return false;
	}

	// Until commit, every exit restores the previous folder with same-filesystem renames.
	auto rollback = qScopeGuard([&] {
		if (!deleteFolderRecursivelyIfPresent(outputFolder))
		{
			QString message = QObject::tr("Failed to discard the replacement frame folder:\n%1").arg(outputFolder);
			if (hadExistingFolder)
				message += "\n\n" + QObject::tr("The previous frame folder remains preserved at:\n%1").arg(preservedFolder);
			QMessageBox::critical(dialogParent, QObject::tr("Error"), message);
			return;
		}

		if (hadExistingFolder && !QDir{}.rename(preservedFolder, outputFolder))
		{
			QMessageBox::critical(dialogParent, QObject::tr("Error"),
				QObject::tr("Failed to restore the previous frame folder.\n\nPreserved folder:\n%1\n\nOriginal location:\n%2")
					.arg(preservedFolder).arg(outputFolder));
		}
	});

	if (!extractFramesOrReport(videoFilePath, outputFolder, dialogParent))
		return false;

	const QString preservedPreviewDir = Catalog::previewDirFor(preservedFolder);
	const bool preservePreview = previewHandling == PreviewHandling::PreserveExisting
		&& hadExistingFolder && QDir(preservedPreviewDir).exists();
	if (preservePreview)
	{
		if (!QDir{}.rename(preservedPreviewDir, Catalog::previewDirFor(outputFolder)))
		{
			QMessageBox::critical(dialogParent, QObject::tr("Error"),
				QObject::tr("Failed to carry the existing preview into the new frame folder.\n\nPreserved folder:\n%1\n\nNew folder:\n%2")
					.arg(preservedFolder).arg(outputFolder));
			return false;
		}
	}
	else
	{
		const Ffmpeg::PreviewResult result =
			Ffmpeg::generatePreviewFrames(videoFilePath, Catalog::previewDirFor(outputFolder), previewFrameCount);
		catalog.setDurationMs(id, result.durationMs);
	}

	rollback.dismiss();
	catalog.markSplitComplete(id);

	if (hadExistingFolder && !deleteFolderRecursivelyIfPresent(preservedFolder))
	{
		QMessageBox::warning(dialogParent, QObject::tr("Cleanup incomplete"),
			QObject::tr("The new frames are ready, but the previous frame folder could not be completely removed:\n%1")
				.arg(preservedFolder));
	}
	return true;
}

bool FrameExtraction::regeneratePreview(Catalog& catalog, const MediaId& id, int previewFrameCount)
{
	assert_and_return_r(catalog.containsMediaItem(id), false);
	assert_and_return_r(catalog.mediaType(id) == Catalog::MediaType::Video, false);
	const QString folder = catalog.folderForMediaItem(id);
	assert_and_return_r(!folder.isEmpty(), false);  // QDir("") addresses the working directory

	// Real frames are authoritative even if the stored split state disagrees.
	if (regeneratePreviewFromRealFrames(folder, previewFrameCount))
	{
		catalog.markSplitComplete(id);
		return true;
	}

	const QString source = catalog.sourcePathForMediaItem(id);
	if (!QFile::exists(source))
		return false;

	const QString previewDirPath = Catalog::previewDirFor(folder);
	if (!QDir{}.mkpath(previewDirPath))
		assert_and_return_unconditional_r("Failed to create preview folder " + previewDirPath.toStdString(), false);

	const Ffmpeg::PreviewResult result =
		Ffmpeg::generatePreviewFrames(source, previewDirPath, previewFrameCount);
	catalog.setDurationMs(id, result.durationMs);
	return result.ok();
}

bool FrameExtraction::reExportAllVideosInteractive(
	Catalog& catalog, int previewFrameCount, QWidget* dialogParent)
{
	if (QMessageBox::question(dialogParent, QObject::tr("Re-export all videos"),
			QObject::tr("This will delete and re-export all video frame folders where the source video is still available.\n\n"
				"This applies to all videos in the library.\n\nContinue?"),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
		return false;

	std::vector<MediaId> toReExport;
	for (const auto& [id, entry] : catalog.mediaItems().asKeyValueRange())
	{
		if (entry.type == Catalog::MediaType::Video && !entry.sourcePath.isEmpty() && QFile::exists(entry.sourcePath))
			toReExport.push_back(id);
	}

	if (toReExport.empty())
	{
		QMessageBox::information(dialogParent, QObject::tr("Re-export all videos"),
			QObject::tr("No folders with an available source video were found."));
		return false;
	}

	Catalog::BatchScope batch(catalog);

	QMessageBox progressBox(dialogParent);
	progressBox.setWindowTitle(QObject::tr("Re-exporting"));
	progressBox.setStandardButtons(QMessageBox::NoButton);
	progressBox.setModal(true);
	progressBox.show();

	for (size_t i = 0, total = toReExport.size(); i < total; ++i)
	{
		progressBox.setText(QObject::tr("Re-exporting video %1/%2...").arg(i + 1).arg(total));
		QApplication::processEvents();

		static_cast<void>(reextractVideoFrames(
			catalog, toReExport[i], PreviewHandling::Regenerate, previewFrameCount, dialogParent));
	}
	return true;
}
