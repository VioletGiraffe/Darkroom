#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QString>
RESTORE_COMPILER_WARNINGS

#include <atomic>
#include <functional>
#include <vector>

class QObject;

// UI- and catalog-free ffmpeg operations.
namespace Ffmpeg {

struct PreviewJob
{
	QString videoFilePath;
	QString destinationFolder;
};

// durationMs remains available on ExtractionFailed; it is -1 when probing never succeeded.
struct PreviewResult
{
	enum class Status
	{
		Ok,
		FolderCreateFailed,
		ProbeFailed,
		ExtractionFailed,
		Cancelled,
	};

	Status status     = Status::Ok;
	qint64 durationMs = -1;
	[[nodiscard]] bool ok() const { return status == Status::Ok; }
};

// Each phase reports its own completed count against jobs.size(); probe failures also count as terminal extraction results.
enum class Phase
{
	Probing,
	Extracting,
};

// Creates each destinationFolder, probes durations, then extracts frameCount evenly-spaced previews using
// concurrent ffmpeg processes on the calling thread. Results preserve job order and failures do not abort the
// batch. Cancellation kills and reaps active processes, starts no more jobs, and leaves partial files for the
// caller to clean up. onProgress runs on the calling thread.
[[nodiscard]] std::vector<PreviewResult> generatePreviewFrames(const std::vector<PreviewJob>& jobs, int frameCount, int maxConcurrentProcesses,
	const std::atomic<bool>& cancelled, const std::function<void(int completedJobs, int totalJobs, Phase phase)>& onProgress = {});

[[nodiscard]] PreviewResult generatePreviewFrames(const QString& videoFilePath, const QString& destinationFolder, int frameCount);

struct SplitOptions
{
	bool tiff        = false;  // TIFF (deflate, lossless) output vs JPEG
	int  jpegQuality = 3;      // ffmpeg -qscale:v; JPEG only (ignored for TIFF)
	int  frameStep   = 1;      // keep every frameStep-th frame (1 = every frame)
};

struct SplitResult
{
	enum class Status
	{
		Ok,
		SourceMissing,
		FolderCreateFailed,
		StartFailed,
		TimedOut,  // single-frame extraction only
		ExtractionFailed,
		NoFrames,
		Cancelled,
	};

	Status  status     = Status::Ok;
	int     frameCount = 0;   // valid on Ok
	int     exitCode   = 0;   // valid on ExtractionFailed
	QString errorOutput;      // stderr + stdout on ExtractionFailed
	[[nodiscard]] bool ok() const { return status == Status::Ok; }
};

// Full-resolution extraction, blocking the calling thread. onProgress runs on it about every 100 ms with the fraction
// done, negative while the duration is unknown. Failure and cancellation remove the entire partial output folder.
[[nodiscard]] SplitResult splitVideoIntoFrames(const QString& videoFilePath, const QString& outputFolder, const SplitOptions& options,
	const std::atomic<bool>& cancelled, const std::function<void(double fractionDone)>& onProgress);

// Single-frame extraction in a background ffmpeg process. ".tif" selects TIFF; other extensions select JPEG.
// The destination must not exist; ffmpeg refuses to overwrite it. A failed ffmpeg may leave a partial output file.
// Call on context's thread. onFinished runs there, possibly before this returns.
// Destroying context first kills ffmpeg and removes its partial output; onFinished then never runs.
// A timeout kills ffmpeg, removes its partial output and reports TimedOut.
void extractSingleFrame(const QString& videoFilePath, qint64 timestampMs, const QString& outputFilePath, int jpegQuality,
	QObject* context, std::function<void(const SplitResult& result)> onFinished);

} // namespace Ffmpeg
