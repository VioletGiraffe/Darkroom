#pragma once

#include <QString>

#include <functional>
#include <vector>

// Thin wrapper over invoking the ffmpeg binary (see Utils.h's ffmpegPath()). Two operations, both pure - they
// run ffmpeg and report the outcome, leaving all UI and catalog updates to the caller: pulling a handful of
// evenly-spaced still frames for a card preview (generatePreviewFrames), and extracting every frame of a video
// into a folder (splitVideoIntoFrames).
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

// Output-format and sampling knobs for splitVideoIntoFrames - the choices the caller pulls from settings.
struct SplitOptions
{
	bool tiff        = false;  // TIFF (deflate, lossless) output vs JPEG
	int  jpegQuality = 3;      // ffmpeg -qscale:v; JPEG only (ignored for TIFF)
	int  frameStep   = 1;      // keep every frameStep-th frame (1 = every frame)
};

// The outcome of one full-frame split. On failure the relevant field carries the detail the caller surfaces:
// exitCode + errorOutput for ExtractionFailed; the other statuses are self-describing.
struct SplitResult
{
	enum class Status
	{
		Ok,                  // frames extracted
		SourceMissing,       // the input video file isn't there
		FolderCreateFailed,  // outputFolder couldn't be created
		StartFailed,         // the ffmpeg binary couldn't be launched (not on PATH / misconfigured)
		TimedOut,            // ffmpeg overran its timeout and was killed
		ExtractionFailed,    // ffmpeg exited non-zero
		NoFrames,            // ffmpeg exited cleanly but wrote no frames
	};

	Status  status     = Status::Ok;
	int     frameCount = 0;   // frames written (valid when Ok)
	int     exitCode   = 0;   // ffmpeg's exit code (ExtractionFailed)
	QString errorOutput;      // ffmpeg's stderr+stdout (ExtractionFailed), for the user-facing error
	[[nodiscard]] bool ok() const { return status == Status::Ok; }
};

// Extracts frames from videoFilePath into outputFolder (created if needed) at full resolution - every frame, or
// every frameStep-th - with one ffmpeg process, blocking until it finishes. This is the raw material behind a
// video's frame folder, as opposed to the small seeked previews above. On any failure the partial output folder
// is removed, so a failed split leaves no debris. Returns the outcome; touches neither UI nor catalog - the
// caller renders the result and registers the video.
[[nodiscard]] SplitResult splitVideoIntoFrames(const QString& videoFilePath, const QString& outputFolder, const SplitOptions& options);

} // namespace Ffmpeg
