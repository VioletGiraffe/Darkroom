#pragma once

#include "Core/MediaId.h"
#include "Ffmpeg.h"

#include <vector>

class Catalog;
class QString;
class QWidget;

// Configured full-frame extraction and preview-repair workflows. Ffmpeg owns raw process execution; this module
// owns catalog commit timing, transactional frame-folder replacement, progress UI, and failure reporting.
// Batch functions show one modal, cancellable progress dialog and return an outcome per id, in order.
namespace FrameExtraction
{
	enum class PreviewHandling
	{
		PreserveExisting,
		Regenerate,
	};

	enum class Outcome
	{
		Done,
		Failed,
		Cancelled,
	};

	// Runs the jobs in parallel ffmpeg processes on a worker thread.
	[[nodiscard]] std::vector<Ffmpeg::PreviewResult> generatePreviewFramesInteractively(
		const std::vector<Ffmpeg::PreviewJob>& jobs, int frameCount, const QString& title, QWidget* parent);

	// No-op success when the catalog already records a complete extraction.
	[[nodiscard]] bool ensureFramesExtracted(
		Catalog& catalog, const MediaId& id, int previewFrameCount, QWidget* dialogParent);

	// One video at a time on the calling thread. Each frame folder is replaced transactionally and marked complete only
	// after it commits; cancelling rolls back the current video.
	[[nodiscard]] std::vector<Outcome> reextractVideoFrames(Catalog& catalog, const std::vector<MediaId>& ids,
		PreviewHandling previewHandling, int previewFrameCount, QWidget* dialogParent);

	// Regenerates from real frames when available, otherwise from the source video.
	[[nodiscard]] std::vector<Outcome> regeneratePreviews(Catalog& catalog, const std::vector<MediaId>& ids, int previewFrameCount,
		QWidget* dialogParent);

	// Returns fully split videos whose real frames are gone to preview-only. Keeps an existing preview, otherwise
	// generates one from the source video.
	[[nodiscard]] std::vector<Outcome> keepPreviewOnly(Catalog& catalog, const std::vector<MediaId>& ids, int previewFrameCount,
		QWidget* dialogParent);

	// Returns true after a confirmed non-empty batch, even when individual videos fail.
	[[nodiscard]] bool reextractAllVideoFramesInteractive(Catalog& catalog, int previewFrameCount, QWidget* dialogParent);
}
