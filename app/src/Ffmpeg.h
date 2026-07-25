#pragma once

#include <QString>

#include <atomic>
#include <functional>
#include <vector>

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
		TimedOut,
		ExtractionFailed,
		NoFrames,
	};

	Status  status     = Status::Ok;
	int     frameCount = 0;   // valid on Ok
	int     exitCode   = 0;   // valid on ExtractionFailed
	QString errorOutput;      // stderr + stdout on ExtractionFailed
	[[nodiscard]] bool ok() const { return status == Status::Ok; }
};

// Blocking full-resolution extraction. Failure removes the entire partial output folder.
[[nodiscard]] SplitResult splitVideoIntoFrames(const QString& videoFilePath, const QString& outputFolder, const SplitOptions& options);

// Blocking single-frame extraction. ".tif" selects TIFF; other extensions select JPEG. Failure removes only
// the partial output file, never its parent folder.
[[nodiscard]] SplitResult extractFrame(const QString& videoFilePath, qint64 timestampMs, const QString& outputFilePath, int jpegQuality);

} // namespace Ffmpeg
