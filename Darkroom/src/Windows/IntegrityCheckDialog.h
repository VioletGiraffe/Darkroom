#pragma once

#include "Core/Catalog.h"
#include "Core/CatalogIntegrity.h"
#include "Core/MediaItemId.h"
#include "Windows/FrameExtraction.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QDialog>
#include <QSet>
#include <QStringList>
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
		// Both refuse a file that is tracked, or has the same bytes as a tracked one.
		std::function<bool(const QString& folderPath, const QString& sourcePath, bool splitIntoFrames)> registerRequested;
		std::function<bool(const QString& filePath)> adoptPhotoRequested;
		// Returns the paths that are gone afterwards.
		std::function<QSet<QString>(const QStringList& paths)> trashRequested;
		// Opens the Import dialog with paths staged; returns when it closes.
		std::function<void(const QStringList& paths)> importRequested;
		// The ffmpeg actions take a batch and return an outcome per id, in order.
		std::function<std::vector<FrameExtraction::Outcome>(const std::vector<MediaItemId>& ids)> reextractFramesRequested;
		std::function<std::vector<FrameExtraction::Outcome>(const std::vector<MediaItemId>& ids)> regeneratePreviewRequested;
		std::function<std::vector<FrameExtraction::Outcome>(const std::vector<MediaItemId>& ids)> keepPreviewOnlyRequested;
		std::function<bool(MediaItemId id)> markSplitRequested;
		// Locate preserves storage and the id; refused when another item is recorded at newSourcePath.
		std::function<bool(MediaItemId id, const QString& newSourcePath)> locateRequested;
		// Remove from library: reports its own failures; an id still in the catalog afterwards was not removed.
		std::function<void(const std::vector<MediaItemId>& ids)> removeEntriesRequested;
		std::function<bool(MediaItemId id)> refreshSourceSizeRequested;
		std::function<bool(MediaItemId id)> removeInvalidLabelReferencesRequested;
	};

	// Returns whether findings existed; a clean scan shows only an information box.
	static bool scanAndShowUi(Catalog& catalog, const QString& rootFolder, Callbacks callbacks, QWidget* parent);

private:
	IntegrityCheckDialog(Catalog& catalog, CatalogIntegrity::IntegrityReport report, Callbacks callbacks, QWidget* parent);
	~IntegrityCheckDialog() override;

	Callbacks _callbacks;
	std::unique_ptr<IntegrityCheckTabs> _tabs;  // borrows _callbacks
};
