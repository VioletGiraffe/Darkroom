#include "Windows/FrameExtraction.h"
#include "Core/Catalog.h"
#include "Ffmpeg.h"
#include "DarkroomSettings.h"
#include "Settings.h"
#include "SharedSettings.h"
#include "Utils.h"

#include "assert/advanced_assert.h"
#include "compiler/compiler_warnings_control.h"
#include "threading/cinterruptablethread.h"

DISABLE_COMPILER_WARNINGS
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QObject>
#include <QProgressDialog>
#include <QScopeGuard>
#include <QSettings>
#include <QUuid>
RESTORE_COMPILER_WARNINGS

#include <atomic>
#include <functional>
#include <vector>

namespace {

Ffmpeg::SplitOptions configuredSplitOptions()
{
	return {
		.tiff = readSettingOrDarkroomDefault(Settings::UseTiff, Defaults::UseTiff).toBool(),
		.jpegQuality = readSettingOrDarkroomDefault(Settings::JpegQuality, Defaults::JpegQuality).toInt(),
		.frameStep = QSettings{}.value(Settings::FrameStep, Defaults::FrameStep).toInt(),
	};
}

// Each ffmpeg process is internally threaded; keep concurrent processes low.
constexpr int PREVIEW_EXTRACTION_CONCURRENCY = 2;

FrameExtraction::Outcome extractFramesOrReport(const QString& videoFilePath, const QString& outputFolder,
	const std::atomic<bool>& cancelled, const std::function<void(double)>& onProgress, QWidget* dialogParent)
{
	const Ffmpeg::SplitResult result =
		Ffmpeg::splitVideoIntoFrames(videoFilePath, outputFolder, configuredSplitOptions(), cancelled, onProgress);
	if (result.ok())
		return FrameExtraction::Outcome::Done;
	if (result.status == Ffmpeg::SplitResult::Status::Cancelled)
		return FrameExtraction::Outcome::Cancelled;

	reportFfmpegFailure(dialogParent, result, videoFilePath, outputFolder);
	return FrameExtraction::Outcome::Failed;
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

	return reextractVideoFrames(catalog, { id }, PreviewHandling::PreserveExisting, previewFrameCount, dialogParent).front()
		== Outcome::Done;
}

namespace {

// Replaces one video's frame folder transactionally and marks the catalog complete only after the new folder commits.
FrameExtraction::Outcome reextractOneVideo(Catalog& catalog, const MediaId& id, FrameExtraction::PreviewHandling previewHandling,
	int previewFrameCount, const std::atomic<bool>& cancelled, const std::function<void(double)>& onProgress, QWidget* dialogParent)
{
	using FrameExtraction::Outcome;
	using FrameExtraction::PreviewHandling;

	assert_and_return_r(catalog.containsMediaItem(id), Outcome::Failed);
	assert_and_return_r(catalog.mediaType(id) == Catalog::MediaType::Video, Outcome::Failed);
	const QString videoFilePath = catalog.sourcePathForMediaItem(id);
	const QString outputFolder = catalog.folderForMediaItem(id);
	assert_and_return_r(!outputFolder.isEmpty(), Outcome::Failed);  // QDir("") addresses the working directory
	const bool hadExistingFolder = QDir(outputFolder).exists();
	const QString preservedFolder = hadExistingFolder
		? QFileInfo(outputFolder).dir().filePath(".darkroom-resplit-" + QUuid::createUuid().toString(QUuid::Id128))
		: QString{};

	if (hadExistingFolder && !QDir{}.rename(outputFolder, preservedFolder))
	{
		QMessageBox::critical(dialogParent, QObject::tr("Error"),
			QObject::tr("Failed to preserve the existing frame folder before replacing it:\n%1").arg(outputFolder));
		return Outcome::Failed;
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

	if (const Outcome extraction = extractFramesOrReport(videoFilePath, outputFolder, cancelled, onProgress, dialogParent);
		extraction != Outcome::Done)
		return extraction;

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
			return Outcome::Failed;
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
	return Outcome::Done;
}

// The video folder of a tracked video; empty, with an assertion, otherwise.
QString trackedVideoFolder(const Catalog& catalog, const MediaId& id)
{
	assert_and_return_r(catalog.containsMediaItem(id), {});
	assert_and_return_r(catalog.mediaType(id) == Catalog::MediaType::Video, {});
	const QString folder = catalog.folderForMediaItem(id);
	assert_r(!folder.isEmpty());  // QDir("") addresses the working directory
	return folder;
}

// Generates the previews of ids[i] for each i in indices from their source videos, recording each result in outcomes[i].
// onGenerated runs for each success. A video whose source is missing fails without a job.
void generatePreviewsFromSources(Catalog& catalog, const std::vector<MediaId>& ids, const std::vector<size_t>& indices,
	int previewFrameCount, const QString& title, QWidget* dialogParent, std::vector<FrameExtraction::Outcome>& outcomes,
	const std::function<void(const MediaId&)>& onGenerated)
{
	using FrameExtraction::Outcome;

	std::vector<Ffmpeg::PreviewJob> jobs;
	std::vector<size_t> jobIndices;
	for (const size_t i : indices)
	{
		const QString source = catalog.sourcePathForMediaItem(ids[i]);
		if (!QFile::exists(source))
		{
			outcomes[i] = Outcome::Failed;
			continue;
		}
		jobs.push_back({ source, Catalog::previewDirFor(catalog.folderForMediaItem(ids[i])) });
		jobIndices.push_back(i);
	}

	const std::vector<Ffmpeg::PreviewResult> results =
		FrameExtraction::generatePreviewFramesInteractively(jobs, previewFrameCount, title, dialogParent);

	// Opened only after the runner: a BatchScope must not span its event loop.
	Catalog::BatchScope batch(catalog);
	for (size_t k = 0; k < results.size(); ++k)
	{
		const size_t i = jobIndices[k];
		catalog.setDurationMs(ids[i], results[k].durationMs);
		if (results[k].ok())
		{
			onGenerated(ids[i]);
			outcomes[i] = Outcome::Done;
		}
		else
			outcomes[i] = results[k].status == Ffmpeg::PreviewResult::Status::Cancelled ? Outcome::Cancelled : Outcome::Failed;
	}
}

} // namespace

std::vector<FrameExtraction::Outcome> FrameExtraction::reextractVideoFrames(Catalog& catalog, const std::vector<MediaId>& ids,
	PreviewHandling previewHandling, int previewFrameCount, QWidget* dialogParent)
{
	if (ids.empty())
		return {};

	constexpr int stepsPerVideo = 100;
	QProgressDialog progress(QString{}, QObject::tr("Cancel"), 0, static_cast<int>(ids.size()) * stepsPerVideo, dialogParent);
	progress.setWindowTitle(QObject::tr("Extracting frames"));
	progress.setModal(true);
	progress.setMinimumDuration(0);
	progress.setAutoClose(false);
	progress.setAutoReset(false);

	std::atomic<bool> cancelled = false;
	QObject::connect(&progress, &QProgressDialog::canceled, &progress, [&cancelled] { cancelled = true; });
	progress.show();

	std::vector<Outcome> outcomes(ids.size(), Outcome::Cancelled);
	for (size_t i = 0; i < ids.size() && !cancelled; ++i)
	{
		const int videoStart = static_cast<int>(i) * stepsPerVideo;
		progress.setLabelText(QObject::tr("Extracting frames of video %1 of %2:\n%3").arg(i + 1).arg(ids.size()).arg(catalog.displayName(ids[i])));
		progress.setValue(videoStart);
		QApplication::processEvents();

		outcomes[i] = reextractOneVideo(catalog, ids[i], previewHandling, previewFrameCount, cancelled,
			[&progress, videoStart](double fractionDone) {
				if (fractionDone >= 0)
					progress.setValue(videoStart + static_cast<int>(fractionDone * stepsPerVideo));
				// Explicit: a modal setValue processes events only when the value changes.
				QApplication::processEvents();
			}, &progress);
	}
	return outcomes;
}

std::vector<FrameExtraction::Outcome> FrameExtraction::regeneratePreviews(Catalog& catalog, const std::vector<MediaId>& ids,
	int previewFrameCount, QWidget* dialogParent)
{
	std::vector<Outcome> outcomes(ids.size(), Outcome::Failed);
	std::vector<size_t> needSource;
	{
		Catalog::BatchScope batch(catalog);
		for (size_t i = 0; i < ids.size(); ++i)
		{
			const QString folder = trackedVideoFolder(catalog, ids[i]);
			if (folder.isEmpty())
				continue;
			// Real frames are authoritative even if the stored split state disagrees.
			if (regeneratePreviewFromRealFrames(folder, previewFrameCount))
			{
				catalog.markSplitComplete(ids[i]);
				outcomes[i] = Outcome::Done;
			}
			else
				needSource.push_back(i);
		}
	}

	generatePreviewsFromSources(catalog, ids, needSource, previewFrameCount, QObject::tr("Regenerating previews"), dialogParent,
		outcomes, [](const MediaId&) {});
	return outcomes;
}

std::vector<FrameExtraction::Outcome> FrameExtraction::keepPreviewOnly(Catalog& catalog, const std::vector<MediaId>& ids,
	int previewFrameCount, QWidget* dialogParent)
{
	std::vector<Outcome> outcomes(ids.size(), Outcome::Failed);
	std::vector<size_t> needSource;
	{
		Catalog::BatchScope batch(catalog);
		for (size_t i = 0; i < ids.size(); ++i)
		{
			const QString folder = trackedVideoFolder(catalog, ids[i]);
			if (folder.isEmpty())
				continue;
			const bool hasRealFrames = !listFrameImageFiles(QDir(folder)).isEmpty();
			assert_r(!hasRealFrames);
			if (hasRealFrames)
				continue;

			if (listFrameImageFiles(QDir(Catalog::previewDirFor(folder))).isEmpty())
			{
				needSource.push_back(i);
				continue;
			}
			catalog.markPreviewOnly(ids[i]);
			outcomes[i] = Outcome::Done;
		}
	}

	generatePreviewsFromSources(catalog, ids, needSource, previewFrameCount, QObject::tr("Generating previews"), dialogParent,
		outcomes, [&catalog](const MediaId& id) { catalog.markPreviewOnly(id); });
	return outcomes;
}

bool FrameExtraction::reextractAllVideoFramesInteractive(
	Catalog& catalog, int previewFrameCount, QWidget* dialogParent)
{
	if (QMessageBox::question(dialogParent, QObject::tr("Re-extract frames of all videos"),
			QObject::tr("This will extract every frame of every video whose source is still available - including videos that "
				"so far have only a preview - replacing their frame folders and previews.\n\n"
				"This applies to all videos in the library.\n\nContinue?"),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
		return false;

	std::vector<MediaId> toReextract;
	for (const auto& [id, entry] : catalog.mediaItems().asKeyValueRange())
	{
		if (entry.type == Catalog::MediaType::Video && !entry.sourcePath.isEmpty() && QFile::exists(entry.sourcePath))
			toReextract.push_back(id);
	}

	if (toReextract.empty())
	{
		QMessageBox::information(dialogParent, QObject::tr("Re-extract frames of all videos"),
			QObject::tr("No videos with an available source were found."));
		return false;
	}

	static_cast<void>(reextractVideoFrames(catalog, toReextract, PreviewHandling::Regenerate, previewFrameCount, dialogParent));
	return true;
}

std::vector<Ffmpeg::PreviewResult> FrameExtraction::generatePreviewFramesInteractively(
	const std::vector<Ffmpeg::PreviewJob>& jobs, int frameCount, const QString& title, QWidget* parent)
{
	if (jobs.empty())
		return {};

	QProgressDialog progress(QObject::tr("Examining video %1/%2...").arg(0).arg(jobs.size()), QObject::tr("Cancel"),
		0, static_cast<int>(jobs.size()), parent);
	progress.setWindowTitle(title);
	progress.setModal(true);
	progress.setMinimumDuration(0);
	// Keep the dialog open until cancelled ffmpeg processes have been reaped.
	progress.setAutoClose(false);
	progress.setAutoReset(false);

	// results is written by this worker and read only after join().
	std::vector<Ffmpeg::PreviewResult> results;
	CInterruptableThread previewThread{ "preview-extraction" };

	QObject::connect(&progress, &QProgressDialog::canceled, parent, [&] {
		previewThread.requestCancellation();
		progress.setLabelText(QObject::tr("Cancelling..."));
	});

	previewThread.start([&](const std::atomic<bool>& cancelled) {
		results = Ffmpeg::generatePreviewFrames(jobs, frameCount, PREVIEW_EXTRACTION_CONCURRENCY, cancelled,
			[&](int done, int total, Ffmpeg::Phase phase) {
				QMetaObject::invokeMethod(&progress, [&progress, &previewThread, done, total, phase] {
					if (previewThread.cancellationRequested())
						return;
					if (phase == Ffmpeg::Phase::Probing)
					{
						progress.setLabelText(QObject::tr("Examining video %1/%2...").arg(done).arg(total));
						return;
					}
					progress.setValue(done);
					progress.setLabelText(QObject::tr("Generating preview %1/%2...").arg(done).arg(total));
				}, Qt::QueuedConnection);
			});
		QMetaObject::invokeMethod(&progress, [&progress] { progress.accept(); }, Qt::QueuedConnection);
	});

	progress.exec();

	// Cover completion, Cancel, Esc, and window close before joining the worker.
	previewThread.requestCancellation();
	previewThread.join();
	return results;
}
