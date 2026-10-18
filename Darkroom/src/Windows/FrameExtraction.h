#pragma once

#include "Core/MediaId.h"

class Catalog;
class QWidget;

// Configured full-frame extraction and preview-repair workflows. Ffmpeg owns raw process execution; this module
// owns catalog commit timing, transactional frame-folder replacement, progress UI, and failure reporting.
namespace FrameExtraction
{
	enum class PreviewHandling
	{
		PreserveExisting,
		Regenerate,
	};

	// No-op success when the catalog already records a complete extraction.
	[[nodiscard]] bool ensureFramesExtracted(
		Catalog& catalog, const MediaId& id, int previewFrameCount, QWidget* dialogParent);

	// Replaces the frame folder transactionally and marks the catalog complete only after the new folder commits.
	[[nodiscard]] bool reextractVideoFrames(Catalog& catalog, const MediaId& id, PreviewHandling previewHandling,
		int previewFrameCount, QWidget* dialogParent);

	// Regenerates from real frames when available, otherwise from the source video.
	[[nodiscard]] bool regeneratePreview(Catalog& catalog, const MediaId& id, int previewFrameCount);

	// Returns true after a confirmed non-empty batch, even when individual videos fail.
	[[nodiscard]] bool reExportAllVideosInteractive(Catalog& catalog, int previewFrameCount, QWidget* dialogParent);
}
