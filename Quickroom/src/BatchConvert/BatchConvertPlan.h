#pragma once

#include "ImageEncoding/ImageEncoding.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QSize>
#include <QString>
#include <QStringList>
RESTORE_COMPILER_WARNINGS

#include <vector>

// What a batch conversion writes, and where
namespace BatchConvert {

enum class FitMode { Inside, Outside };
enum class ScaleDirection { Any, OnlyEnlarge, OnlyShrink };
enum class OutputLocation { Subfolder, Folder, SourceFolder };
enum class ClashResolution { Replace, KeepBoth, Skip };

struct ResizeOptions
{
	bool enabled = false;
	FitMode fit = FitMode::Inside;
	QSize box{ 1920, 1080 };
	ScaleDirection direction = ScaleDirection::Any;
	bool adaptToOrientation = true; // swaps the box's sides for an image whose orientation differs from the box's
};

struct Options
{
	ResizeOptions resize;
	QString mimeType; // empty: each source's own format where writable, else JPEG
	int quality = ImageEncoding::DefaultQuality;
	ImageEncoding::JpegSubsampling jpegSubsampling = ImageEncoding::JpegSubsampling::Chroma420;
	OutputLocation location = OutputLocation::Subfolder;
	QString subfolderName = QStringLiteral("converted"); // OutputLocation::Subfolder: created in each source's folder
	QString folder;                                      // OutputLocation::Folder
};

// The size to write an image of uprightSize at; uprightSize itself when resize is disabled
[[nodiscard]] QSize outputSize(QSize uprightSize, const ResizeOptions& resize);

// The format sourcePath is written in: options.mimeType, else the source's own format when writable, else JPEG
[[nodiscard]] QString outputMimeType(const QString& sourcePath, const Options& options);

struct Target
{
	QString sourcePath;
	QString targetPath;
	QString mimeType;
	bool replacesSource = false; // targetPath is sourcePath
	bool skipped = false;        // ClashResolution::Skip of a target that exists
};

struct Plan
{
	std::vector<Target> targets; // in the order of the sources
	int existingCount = 0;       // targets whose path exists on disk, before the resolution
	int replacedSourceCount = 0; // existing targets that are their own source
};

// A target path another target of the batch has taken, or another source of the batch, gets a counter: "name (2).ext".
// resolution applies to target paths that exist on disk; KeepBoth also counts past the names on disk.
// Lists each target folder once; a folder that does not exist yet holds no names.
[[nodiscard]] Plan planTargets(const QStringList& sourcePaths, const Options& options, ClashResolution resolution);

} // namespace BatchConvert
