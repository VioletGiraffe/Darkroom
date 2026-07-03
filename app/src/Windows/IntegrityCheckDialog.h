#pragma once

#include "Core/Catalog.h"
#include "Core/CatalogIntegrity.h"
#include "Core/MediaId.h"

#include <QDialog>

#include <functional>

// ============================================================================
// IntegrityCheckDialog - shows what CatalogIntegrity::scan found (untracked frame folders, broken video
// entries, and photos whose source file is missing) and lets the user resolve each one: register an untracked
// folder against its source; per broken video, re-import / regenerate preview / mark-fully-split / remove; per
// missing photo, locate the moved file (referenced only) or remove. All UI logic lives here behind the static
// scanAndShowUi() entry point - MainWindow only supplies the callbacks that actually touch the Catalog/disk.
// ============================================================================

class IntegrityCheckDialog final : public QDialog
{
public:
	struct Callbacks
	{
		// Attempts to register an untracked folder at the given source path; returns whether it succeeded
		// (Catalog::addMediaItem refuses on an id clash with a different folder).
		std::function<bool(const QString& folderPath, const QString& sourcePath)> registerRequested;
		// Re-extracts a video's frames from its (present) source back into its folder (also regenerating the
		// preview); returns whether frames actually exist there afterwards. The GHOST recovery.
		std::function<bool(const MediaId& id)> reimportRequested;
		// Rebuilds a video's preview so its card renders again - from its real frames if present (which also
		// marks the entry fully split), else from its source video. True if a preview exists afterwards. INVISIBLE.
		std::function<bool(const MediaId& id)> regeneratePreviewRequested;
		// Marks a video as fully split when its real frames exist but the entry was still flagged preview-only. STALE.
		std::function<bool(const MediaId& id)> markSplitRequested;
		// Drops an entry from the catalog outright; always succeeds.
		std::function<bool(const MediaId& id)> removeEntryRequested;
		// Repoints a referenced photo (whose source file moved) at a newly-located file; returns whether it
		// succeeded - refused on an id clash with a different tracked item (via Catalog::applyRename).
		std::function<bool(const MediaId& id, const QString& newSourcePath)> locatePhotoRequested;
	};

	// Scans the catalog for drift against disk and, if anything was found, shows this dialog so the user can
	// resolve each finding. Just an information box if the catalog is clean.
	static void scanAndShowUi(Callbacks callbacks, QWidget* parent);

private:
	IntegrityCheckDialog(const CatalogIntegrity::IntegrityReport& report, Callbacks callbacks, QWidget* parent);
	~IntegrityCheckDialog() override;

	Callbacks m_callbacks;
};
