#pragma once

#include <QString>

#include <functional>
#include <vector>

// Thin wrapper over invoking the ffmpeg binary (see Utils.h's ffmpegPath()) for the one operation shared
// across import, the legacy-preview backfill, and Quick Import's staging previews: pulling a handful of
// evenly-spaced still frames out of a video file.
namespace Ffmpeg {

// One video's preview-extraction request: pull evenly-spaced frames from videoFilePath into
// outputFolder/preview/.
struct PreviewJob
{
	QString videoFilePath;
	QString outputFolder;
};

// Generates frameCount evenly-spaced preview frames for each job into its outputFolder/preview/ (created if
// needed), running up to maxConcurrentProcesses ffmpeg processes at once. Each ffmpeg is its own OS process,
// so this parallelizes without any worker threads: it starts a batch of processes and then waits on that
// batch, all on the calling thread. Work happens in two passes - first all duration probes, then all frame
// extractions - each batched at maxConcurrentProcesses; a job whose folder can't be created or whose
// duration can't be probed (the first thing to fail on a corrupt file) is skipped, leaving its preview/
// empty and never entering the extraction pass. Best-effort throughout - callers never gate on this.
//
// onProgress, if set, is invoked on the calling thread as each job reaches its terminal state (extracted or
// skipped), with (completedJobs, totalJobs) where totalJobs == jobs.size(); use it to drive UI. Because the
// call blocks the calling thread for the whole batch, a UI callback should pump events itself if it wants
// the display to update mid-batch.
void generatePreviewFrames(const std::vector<PreviewJob>& jobs, int frameCount, int maxConcurrentProcesses,
	const std::function<void(int completedJobs, int totalJobs)>& onProgress = {});

// Single-video convenience: the batch form with one job run one process at a time. Used by the import
// and re-split paths, which handle a single video.
void generatePreviewFrames(const QString& videoFilePath, const QString& outputFolder, int frameCount);

} // namespace Ffmpeg
