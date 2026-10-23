#pragma once

#include "Core/Catalog.h"
#include "Core/CatalogIntegrity.h"
#include "Core/MediaId.h"
#include "Windows/FrameExtraction.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QDialog>
RESTORE_COMPILER_WARNINGS

#include <functional>
#include <memory>
#include <vector>

// Presents CatalogIntegrity findings and recovery actions. MainWindow supplies all callbacks that mutate the catalog
// or disk; the dialog only batches their persistence. Tab UI lives in IntegrityCheckTabs.

class IntegrityCheckTabs;

class IntegrityCheckDialog final : public QDialog
{
public:
	struct Callbacks
	{
		std::function<bool(const QString& folderPath, const QString& sourcePath)> registerRequested;
		std::function<bool(const QString& filePath)> adoptPhotoRequested;
		// The ffmpeg actions take a batch and return an outcome per id, in order.
		std::function<std::vector<FrameExtraction::Outcome>(const std::vector<MediaId>& ids)> reextractFramesRequested;
		std::function<std::vector<FrameExtraction::Outcome>(const std::vector<MediaId>& ids)> regeneratePreviewRequested;
		std::function<std::vector<FrameExtraction::Outcome>(const std::vector<MediaId>& ids)> keepPreviewOnlyRequested;
		std::function<bool(const MediaId& id)> markSplitRequested;
		// Locate callbacks preserve storage and may refuse an identity collision.
		// Returns the video's id for the new source, or an invalid id when refused.
		std::function<MediaId(const MediaId& id, const QString& newSourcePath)> locateSourceRequested;
		std::function<bool(const MediaId& id)> removeEntryRequested;
		std::function<bool(const MediaId& id, const QString& newSourcePath)> locatePhotoRequested;
		std::function<bool(const MediaId& id)> removeInvalidLabelReferencesRequested;
	};

	// Returns whether findings existed; a clean scan shows only an information box.
	static bool scanAndShowUi(Catalog& catalog, const QString& rootFolder, Callbacks callbacks, QWidget* parent);

private:
	IntegrityCheckDialog(Catalog& catalog, CatalogIntegrity::IntegrityReport report, Callbacks callbacks, QWidget* parent);
	~IntegrityCheckDialog() override;

	Callbacks _callbacks;
	std::unique_ptr<IntegrityCheckTabs> _tabs;  // borrows _callbacks
};
