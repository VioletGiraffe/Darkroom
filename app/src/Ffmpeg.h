#pragma once

#include <QString>

#include <functional>
#include <vector>

// Thin wrapper over invoking the ffmpeg binary (see Utils.h's ffmpegPath()) for the one operation shared
// across import, Quick Import's staging previews, and the integrity tool's preview regeneration: pulling a
// handful of evenly-spaced still frames out of a video file.
namespace Ffmpeg {

// One video's preview-extraction request: pull evenly-spaced frames from videoFilePath into
// destinationFolder. The "preview" subfolder convention lives with the caller (see Catalog::previewDirFor);
// this module writes frames into exactly the folder it's handed and knows nothing about where that sits.
struct PreviewJob
{
	QString videoFilePath;
	QString destinationFolder;
};

// Per-job outcome. durationMs is the source video's duration in ms, read from the probe that precedes
// extraction (see generatePreviewFrames) - so it is valid (> 0) whenever the probe succeeded, which includes
// the ExtractionFailed case: a caller can still record the duration even when no frames were written. It stays
// -1 (unknown) only when the probe never ran or found no duration (FolderCreateFailed / ProbeFailed).
struct PreviewResult
{
	enum class Status
	{
		Ok,                 // frames extracted
		FolderCreateFailed, // destinationFolder couldn't be created; nothing ran
		ProbeFailed,        // ffmpeg couldn't read the input / no parseable duration (typically a corrupt file)
		ExtractionFailed,   // duration probed fine, but the frame-extraction ffmpeg exited non-zero or was killed
	};

	Status status     = Status::Ok;
	qint64 durationMs = -1;
	[[nodiscard]] bool ok() const { return status == Status::Ok; }
};

// Generates frameCount evenly-spaced preview frames for each job into its destinationFolder (created if
// needed), running up to maxConcurrentProcesses ffmpeg processes at once. Each ffmpeg is its own OS process,
// so this parallelizes without any worker threads: it starts a batch of processes and then waits on that
// batch, all on the calling thread. Work happens in two passes - first all duration probes, then all frame
// extractions - each batched at maxConcurrentProcesses; a job whose folder can't be created or whose
// duration can't be probed (the first thing to fail on a corrupt file) is skipped, leaving its destinationFolder
// empty and never entering the extraction pass.
//
// Returns one PreviewResult per job, in jobs order (result[i] describes jobs[i]). Best-effort in that a failed
// job never aborts the batch: its status/duration are reported for the caller to act on or ignore, not enforced.
//
// onProgress, if set, is invoked on the calling thread as each job reaches its terminal state (extracted or
// skipped), with (completedJobs, totalJobs) where totalJobs == jobs.size(); use it to drive UI. Because the
// call blocks the calling thread for the whole batch, a UI callback should pump events itself if it wants
// the display to update mid-batch.
[[nodiscard]] std::vector<PreviewResult> generatePreviewFrames(const std::vector<PreviewJob>& jobs, int frameCount, int maxConcurrentProcesses,
	const std::function<void(int completedJobs, int totalJobs)>& onProgress = {});

// Single-video convenience: the batch form with one job run one process at a time, returning its lone
// PreviewResult. Used by the import and re-split paths, which handle a single video.
[[nodiscard]] PreviewResult generatePreviewFrames(const QString& videoFilePath, const QString& destinationFolder, int frameCount);

} // namespace Ffmpeg
