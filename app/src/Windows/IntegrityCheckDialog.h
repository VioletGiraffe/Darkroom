#pragma once

#include "Core/Catalog.h"
#include "Core/CatalogIntegrity.h"
#include "Core/MediaId.h"

#include <QDialog>

#include <functional>
#include <memory>

// ============================================================================
// IntegrityCheckDialog - shows what CatalogIntegrity::scan found (untracked frame folders, untracked photo
// files, broken video entries, and photos whose source file is missing) and lets the user resolve each one:
// register an untracked folder against its source; add an untracked photo to the catalog; per broken video,
// re-import / regenerate preview / mark-fully-split / remove; per missing photo, locate the moved file
// (referenced only) or remove. Each section that admits a uniform batch also carries a blanket action over its
// rows - add all untracked photos; re-import / regenerate / remove all broken videos; locate (search a folder
// recursively and relink by identity) / remove all missing photos - each a loop over the same per-row callbacks.
// All UI logic lives behind the static scanAndShowUi() entry point - the dialog frame here, the sections' rows
// and handlers in IntegrityCheckSections.h - MainWindow only supplies the callbacks that actually touch the
// Catalog/disk.
// ============================================================================

class IntegrityCheckSections;

class IntegrityCheckDialog final : public QDialog
{
public:
	struct Callbacks
	{
		// Attempts to register an untracked folder at the given source path; returns whether it succeeded
		// (Catalog::addMediaItem refuses on an id clash with a different folder).
		std::function<bool(const QString& folderPath, const QString& sourcePath)> registerRequested;
		// Adopts an untracked image file (already sitting in <root>/Photos/<label>/) as an owned photo under that
		// label; returns whether it succeeded (refused on a name+size clash with an already-tracked item).
		std::function<bool(const QString& filePath)> adoptPhotoRequested;
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
	// Owns the section rows' shared state, so it must live as long as the dialog's buttons - see IntegrityCheckSections.h.
	std::unique_ptr<IntegrityCheckSections> m_sections;
};
