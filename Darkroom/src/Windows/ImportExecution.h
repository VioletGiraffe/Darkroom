#pragma once

#include "Core/LabelId.h"
#include "Core/MediaItemId.h"
#include "Import.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QStringList>
RESTORE_COMPILER_WARNINGS

#include <vector>

class Catalog;
class QWidget;

// Interactive batch execution over the UI-free per-item Import workers.
namespace ImportExecution
{
	struct StagedVideo
	{
		QString path;
		QString previewDir;      // see Import::importVideo
		qint64  durationMs = -1;
	};

	// storageFolderPath must be a Catalog-verified video storage path. Returns, per video in input order, the
	// registered id, or None when it was not imported.
	[[nodiscard]] std::vector<MediaItemId> importVideosInteractive(Catalog& catalog, const std::vector<StagedVideo>& videos,
		const QString& storageFolderPath, QWidget* dialogParent);

	// Returns one result per processed path, preserving input order.
	[[nodiscard]] std::vector<Import::PhotoResult> importPhotosInteractive(
		Catalog& catalog, LabelId labelId, const QStringList& photoPaths, Import::PhotoImportMode mode, QWidget* dialogParent);
}
