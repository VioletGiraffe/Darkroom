#pragma once

#include "Core/LabelId.h"
#include "Core/MediaId.h"
#include "Import.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QHash>
#include <QStringList>
RESTORE_COMPILER_WARNINGS

#include <vector>

class Catalog;
class QWidget;

// Interactive batch execution over the UI-free per-item Import workers.
namespace ImportExecution
{
	// storageFolderPath must be a Catalog-verified video storage path.
	void importVideosInteractive(Catalog& catalog, QStringList videoPaths, const QString& storageFolderPath,
		const QHash<MediaId, QString>& stagedPreviewDirs, const QHash<MediaId, qint64>& stagedDurations, QWidget* dialogParent);

	// Returns one result per processed path, preserving input order.
	[[nodiscard]] std::vector<Import::PhotoResult> importPhotosInteractive(
		Catalog& catalog, LabelId labelId, const QStringList& photoPaths, Import::PhotoImportMode mode, QWidget* dialogParent);
}
