#pragma once

#include "Core/Catalog.h"
#include "Core/CatalogIntegrity.h"
#include "Core/MediaId.h"

#include <QDialog>

#include <functional>
#include <memory>

// Presents CatalogIntegrity findings and per-row/batch recovery actions. MainWindow supplies all callbacks
// that mutate the catalog or disk; section UI lives in IntegrityCheckSections.

class IntegrityCheckSections;

class IntegrityCheckDialog final : public QDialog
{
public:
	struct Callbacks
	{
		std::function<bool(const QString& folderPath, const QString& sourcePath)> registerRequested;
		std::function<bool(const QString& filePath)> adoptPhotoRequested;
		std::function<bool(const MediaId& id)> reimportRequested;
		std::function<bool(const MediaId& id)> regeneratePreviewRequested;
		std::function<bool(const MediaId& id)> markSplitRequested;
		// Locate callbacks preserve storage and may refuse an identity collision.
		std::function<bool(const MediaId& id, const QString& newSourcePath)> locateSourceRequested;
		std::function<bool(const MediaId& id)> removeEntryRequested;
		std::function<bool(const MediaId& id, const QString& newSourcePath)> locatePhotoRequested;
	};

	// Returns whether findings existed; a clean scan shows only an information box.
	static bool scanAndShowUi(const Catalog& catalog, const QString& rootFolder, Callbacks callbacks, QWidget* parent);

private:
	IntegrityCheckDialog(const Catalog& catalog, const CatalogIntegrity::IntegrityReport& report, Callbacks callbacks, QWidget* parent);
	~IntegrityCheckDialog() override;

	Callbacks _callbacks;
	// Button callbacks in the section widgets borrow this state.
	std::unique_ptr<IntegrityCheckSections> _sections;
};
