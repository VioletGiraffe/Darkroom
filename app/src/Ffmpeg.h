#pragma once

#include <QString>

#include <atomic>
#include <functional>
#include <vector>

// Thin wrapper over invoking the ffmpeg binary (see Utils.h's ffmpegPath()). Every operation is pure: it runs
// ffmpeg and reports the outcome, leaving all UI and catalog updates to the caller.
namespace Ffmpeg {

// One video's preview-extraction request. The "preview" subfolder convention lives with the caller (see
// Catalog::previewDirFor); frames go into exactly the folder handed over here.
struct PreviewJob
{
	QString videoFilePath;
	QString destinationFolder;
};

// Per-job outcome. durationMs (the source's length in ms) is valid whenever the probe succeeded - including
// under ExtractionFailed, so a caller can record the duration even when no frames were written. It stays -1 for
// FolderCreateFailed / ProbeFailed / Cancelled.
struct PreviewResult
{
	enum class Status
	{
		Ok,                 // frames extracted
		FolderCreateFailed, // destinationFolder couldn't be created; nothing ran
		ProbeFailed,        // ffmpeg couldn't read the input / no parseable duration (typically a corrupt file)
		ExtractionFailed,   // duration probed fine, but the frame-extraction ffmpeg exited non-zero or was killed
		Cancelled,          // never started, or killed mid-run, because cancellation was requested - not a failure
	};

	Status status     = Status::Ok;
	qint64 durationMs = -1;
	[[nodiscard]] bool ok() const { return status == Status::Ok; }
};

// Which of generatePreviewFrames' two passes an onProgress report comes from. Probing counts jobs leaving the
// probe pass, probed or failed; Extracting counts jobs reaching their terminal state, so it carries the pass-1
// failures too. Both count against jobs.size().
enum class Phase
{
	Probing,
	Extracting,
};

// Generates frameCount evenly-spaced preview frames for each job into its destinationFolder (created if needed).
// Parallel without worker threads - each ffmpeg is its own OS process, so this starts a window of up to
// maxConcurrentProcesses of them and then waits on that window, all on the calling thread. Two passes: all
// duration probes, then all extractions, except that a large source extracts in a window of its own so its long
// read and decode don't contend with another's. A job whose folder can't be created or whose duration can't be
// probed (the first thing to fail on a corrupt file) never enters the extraction pass, leaving its
// destinationFolder empty.
//
// Returns one PreviewResult per job, in jobs order. Best-effort: a failed job never aborts the batch, its status
// and duration are reported for the caller to act on or ignore.
//
// Setting `cancelled` (from another thread - the call blocks its own for the whole batch) kills the running
// ffmpeg processes instead of waiting them out, starts no further ones, and returns Cancelled for every job that
// hadn't already finished cleanly - the ones that had keep their Ok result. Partial frames from a killed
// extraction stay in destinationFolder for the caller to remove.
//
// onProgress, if set, is invoked as each job leaves the probe pass and again as each reaches its terminal state,
// with (completedJobs, totalJobs, phase) where totalJobs == jobs.size(). Each phase counts to totalJobs of its
// own, so the two are not one continuous run of numbers. It runs on the calling thread, which is not the GUI
// thread if the caller followed the cancellation contract above - so a UI callback must marshal to it.
[[nodiscard]] std::vector<PreviewResult> generatePreviewFrames(const std::vector<PreviewJob>& jobs, int frameCount, int maxConcurrentProcesses,
	const std::atomic<bool>& cancelled, const std::function<void(int completedJobs, int totalJobs, Phase phase)>& onProgress = {});

// Single-video convenience: the batch form with one job, returning its lone PreviewResult. Used by the import
// and re-split paths.
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
// every frameStep-th - with one blocking ffmpeg process. This is the raw material behind a video's frame folder,
// as opposed to the small seeked previews above. On any failure the partial output folder is removed, so a
// failed split leaves no debris.
[[nodiscard]] SplitResult splitVideoIntoFrames(const QString& videoFilePath, const QString& outputFolder, const SplitOptions& options);

// Extracts the single frame at timestampMs into outputFilePath (parent folder created if needed), with one
// blocking ffmpeg process. The format follows the file's extension - ".tif" = TIFF, anything else JPEG at
// jpegQuality - using splitVideoIntoFrames' encoder options, so an extracted frame matches split output. On
// failure any partial output file is removed; the folder is left alone (it may be a user-chosen destination
// holding other files). Reuses SplitResult; frameCount is 1 on success.
[[nodiscard]] SplitResult extractFrame(const QString& videoFilePath, qint64 timestampMs, const QString& outputFilePath, int jpegQuality);

} // namespace Ffmpeg
