#include "Ffmpeg.h"
#include "Utils.h"
#include "assert/advanced_assert.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStringList>

#include <vector>

namespace {

// Fixed, independent of the user's full-split jpegQuality() setting - preview frames are small permanent
// thumbnails, not archival output, so there's no need to tie them to the full-split quality.
constexpr int kPreviewFrameJpegQuality = 5;

// Preview frames are downscaled to this height (width auto-computed by ffmpeg to preserve the source's
// aspect ratio - see the -vf scale argument below); upscaling a smaller source is fine, these are thumbnails.
constexpr int kPreviewFrameHeight = 360;

// In a batch, a source larger than this has its preview frames extracted in an ffmpeg process of its own, never
// sharing a window with another job: a big file's extraction is a long sequential read and a long decode at once,
// and running two of those together thrashes disk and CPU, whereas small clips only gain from packing. The batch
// is usually fed small clips, so this rarely trips - it just keeps the occasional large source from dragging the
// others down. Tuned by eye; easy to revisit.
constexpr qint64 kSoloExtractionAboveBytes = 500LL * 1024 * 1024;

// Waits for a process to finish, killing it if it overruns or if cancellation is requested, so a stuck ffmpeg
// (e.g. on a corrupt file) never lingers past the QProcess it was spawned from - important here since a batch
// can spawn many at once.
void waitForFinishedOrKill(QProcess& process, int timeoutMs, const std::atomic<bool>& cancelled)
{
	// Waited in slices rather than one blocking call so that a cancellation is acted on within a slice, instead
	// of after however long this process still had to run.
	constexpr int sliceMs = 100;
	for (int remainingMs = timeoutMs; remainingMs > 0 && !cancelled; remainingMs -= sliceMs)
		if (process.waitForFinished(qMin(sliceMs, remainingMs)))
			return;

	process.kill();
	process.waitForFinished();  // reap the killed process rather than leaving it orphaned
}

// Runs `windowSizes.size()` windows of concurrent QProcess, all on the calling thread - the parallelism is the OS
// running the ffmpeg processes at once, not threads. Operations are numbered 0.. in window order; for each,
// start(index, process) launches it without waiting, then once the whole window is started, finish(index,
// process) is called per operation (in start order) to wait on and consume its result. Cancellation stops
// further windows from starting; the current window is still finished through, because that is what kills and
// reaps its processes (see waitForFinishedOrKill) rather than leaving them running.
template <typename StartFn, typename FinishFn>
void runInProcessWindows(const std::vector<int>& windowSizes, const std::atomic<bool>& cancelled, StartFn&& start, FinishFn&& finish)
{
	int windowStart = 0;
	for (const int windowCount : windowSizes)
	{
		if (cancelled)
			break;

		// The sizing constructor default-constructs each QProcess in place; QProcess is a QObject (neither
		// copyable nor movable), so a vector of them can only be built this way, not grown via push_back.
		std::vector<QProcess> processes(windowCount);
		for (int i = 0; i < windowCount; ++i)
			start(windowStart + i, processes[i]);

		for (int i = 0; i < windowCount; ++i)
			finish(windowStart + i, processes[i]);

		windowStart += windowCount;
	}
}

// Starts (without waiting) an ffmpeg invocation that only opens the input, so its stderr banner carries the
// Duration line parsed by parseProbedDurationMs. No ffprobe binary ships with this app, hence probing via
// ffmpeg itself; the missing output file makes ffmpeg exit non-zero, which is why the duration is read from
// stderr rather than gated on the exit code.
void startDurationProbe(QProcess& probe, const QString& videoFilePath)
{
	probe.start(ffmpegPath(), { "-i", QDir::toNativeSeparators(videoFilePath) });
}

// Consumes a probe started by startDurationProbe. Returns the video duration in ms, or -1 if ffmpeg couldn't
// be run or its output didn't contain a parseable duration.
qint64 parseProbedDurationMs(QProcess& probe, const std::atomic<bool>& cancelled)
{
	if (!probe.waitForStarted())
		return -1;
	waitForFinishedOrKill(probe, 30000, cancelled);

	const QString stderrOutput = probe.readAllStandardError();
	static const QRegularExpression re(R"(Duration:\s*(\d+):(\d+):(\d+\.\d+))");
	const QRegularExpressionMatch match = re.match(stderrOutput);
	if (!match.hasMatch())
		return -1;

	const qint64 hours   = match.captured(1).toLongLong();
	const qint64 minutes = match.captured(2).toLongLong();
	const double seconds = match.captured(3).toDouble();
	return hours * 3600000 + minutes * 60000 + static_cast<qint64>(seconds * 1000);
}

// Mirrors pickEvenlySpacedFrames' 10%-90% sampling window (Utils.h), in the time domain: frameCount evenly
// spaced timestamps (ms) across a video of the given duration.
std::vector<qint64> pickEvenlySpacedTimestampsMs(qint64 durationMs, int frameCount)
{
	const qint64 startMs = static_cast<qint64>(durationMs * 0.1);
	const qint64 endMs   = static_cast<qint64>(durationMs * 0.9);

	std::vector<qint64> out;
	out.reserve(frameCount);
	for (int i = 0; i < frameCount; ++i)
		out.push_back((frameCount == 1) ? startMs : startMs + i * (endMs - startMs) / (frameCount - 1));
	return out;
}

// Builds the argument list for the single multi-seek extraction that writes all of one video's preview
// frames.
QStringList buildExtractionArguments(const QString& videoFilePath, const QString& previewFolder, const std::vector<qint64>& timestampsMs)
{
	const QString nativeVideoPath = QDir::toNativeSeparators(videoFilePath);

	QStringList arguments;
	arguments << "-y";

	// The same file is opened once per timestamp: -ss is only a real (keyframe-index) seek when it precedes -i. As an
	// output option it decodes and discards everything from the start of the stream instead.
	for (const qint64 timestampMs : timestampsMs)
		arguments << "-ss" << QString::number(timestampMs / 1000.0, 'f', 3) << "-i" << nativeVideoPath;

	for (int i = 0; i < static_cast<int>(timestampsMs.size()); ++i)
	{
		const QString outputPath = previewFolder + QString("/%1.jpg").arg(i + 1, 4, 10, QChar('0'));
		arguments << "-map" << QString("%1:v:0").arg(i)
			<< "-frames:v" << "1"
			<< "-vf" << QString("scale=-2:%1").arg(kPreviewFrameHeight)
			<< "-qscale:v" << QString::number(kPreviewFrameJpegQuality)
			<< QDir::toNativeSeparators(outputPath);
	}

	return arguments;
}

} // namespace

namespace Ffmpeg {

std::vector<PreviewResult> generatePreviewFrames(const std::vector<PreviewJob>& jobs, int frameCount, int maxConcurrentProcesses,
	const std::atomic<bool>& cancelled, const std::function<void(int, int)>& onProgress)
{
	const int total = static_cast<int>(jobs.size());
	std::vector<PreviewResult> results(total);   // one per job, jobs order; default { Ok, -1 } refined below
	if (total == 0)
		return results;

	const int maxProcesses = qMax(1, maxConcurrentProcesses);

	// Reaching a terminal state is tracked per job, not just counted: whatever the cancellation cut short is
	// exactly what never got here, and is marked Cancelled once both passes are done.
	int completed = 0;
	std::vector<bool> reachedTerminalState(total, false);
	const auto reportCompleted = [&](int i) { reachedTerminalState[i] = true; ++completed; if (onProgress) onProgress(completed, total); };

	// Pass 1: probe each job's duration and build its extraction arguments, up to maxProcesses probes at once
	// (probing is size-independent, so the solo rule below doesn't apply - every window is packed full). A job
	// whose destination folder can't be created (FolderCreateFailed) or whose duration can't be probed
	// (ProbeFailed - a corrupt file typically fails here first) gets no arguments and is counted done now, so it
	// never enters pass 2 and the extraction windows there stay packed with only good jobs.
	std::vector<int> probeWindows;
	for (int remaining = total; remaining > 0; remaining -= maxProcesses)
		probeWindows.push_back(qMin(maxProcesses, remaining));

	std::vector<bool> probeStarted(total, false);
	std::vector<QStringList> extractionArguments(total);
	runInProcessWindows(probeWindows, cancelled,
		[&](int i, QProcess& probe) {
			if (!QDir{}.mkpath(jobs[i].destinationFolder))
				return;  // leaves probeStarted[i] false -> FolderCreateFailed in finish
			startDurationProbe(probe, jobs[i].videoFilePath);
			probeStarted[i] = true;
		},
		[&](int i, QProcess& probe) {
			if (!probeStarted[i])
			{
				results[i].status = PreviewResult::Status::FolderCreateFailed;
				reportCompleted(i);
				return;
			}
			const qint64 durationMs = parseProbedDurationMs(probe, cancelled);
			if (cancelled)
				return;  // pass 2 will not run, so even a duration that came back is of no use: leave the job Cancelled
			if (durationMs <= 0)
			{
				results[i].status = PreviewResult::Status::ProbeFailed;
				reportCompleted(i);  // best-effort: leave this job's destination empty rather than guessing seek points
			}
			else
			{
				results[i].durationMs = durationMs;  // status stays Ok unless pass 2 downgrades it to ExtractionFailed
				extractionArguments[i] = buildExtractionArguments(jobs[i].videoFilePath, jobs[i].destinationFolder,
					pickEvenlySpacedTimestampsMs(durationMs, frameCount));
			}
		});

	// Pass 2: run the extractions for the jobs that probed successfully. Windows pack maxProcesses at a time,
	// except a source over kSoloExtractionAboveBytes gets a window of its own so its long read and decode don't
	// run beside another's. A non-zero exit (or a kill on timeout) marks the job ExtractionFailed but leaves its
	// probed duration intact.
	std::vector<int> jobsToExtract;
	jobsToExtract.reserve(total);
	for (int i = 0; i < total; ++i)
	{
		if (!extractionArguments[i].isEmpty())
			jobsToExtract.push_back(i);
	}

	std::vector<int> extractionWindows;
	int packed = 0;  // small jobs gathered for the current window but not yet emitted
	for (const int i : jobsToExtract)
	{
		if (const auto fileSize = QFileInfo(jobs[i].videoFilePath).size(); fileSize > kSoloExtractionAboveBytes)
		{
			if (packed > 0) { extractionWindows.push_back(packed); packed = 0; }  // close the run of small jobs before it
			extractionWindows.push_back(1);
		}
		else if (++packed == maxProcesses)
		{
			extractionWindows.push_back(packed);
			packed = 0;
		}
	}
	if (packed > 0)
		extractionWindows.push_back(packed);

	runInProcessWindows(extractionWindows, cancelled,
		[&](int k, QProcess& extract) {
			extract.start(ffmpegPath(), extractionArguments[jobsToExtract[k]]);
		},
		[&](int k, QProcess& extract) {
			bool extracted = false;
			if (extract.waitForStarted())
			{
				waitForFinishedOrKill(extract, 60000, cancelled);
				extracted = extract.exitStatus() == QProcess::NormalExit && extract.exitCode() == 0;
			}
			if (!extracted)
			{
				if (cancelled)
					return;  // killed by the cancellation rather than broken: Cancelled, not ExtractionFailed
				results[jobsToExtract[k]].status = PreviewResult::Status::ExtractionFailed;
			}
			reportCompleted(jobsToExtract[k]);
		});

	for (int i = 0; i < total; ++i)
		if (!reachedTerminalState[i])
			results[i].status = PreviewResult::Status::Cancelled;

	return results;
}

PreviewResult generatePreviewFrames(const QString& videoFilePath, const QString& destinationFolder, int frameCount)
{
	static const std::atomic<bool> neverCancelled{ false };  // nothing can reach this call to cancel it: it blocks its caller start to finish
	return generatePreviewFrames({ PreviewJob{ videoFilePath, destinationFolder } }, frameCount, /*maxConcurrentProcesses*/ 1, neverCancelled).front();
}

SplitResult splitVideoIntoFrames(const QString& videoFilePath, const QString& outputFolder, const SplitOptions& options)
{
	SplitResult result;

	// The source must be present before anything runs: otherwise ffmpeg fails on the missing input and its raw
	// stderr stands in for a clear "the file is gone" report (which the caller renders from SourceMissing).
	if (!QFileInfo::exists(videoFilePath))
	{
		result.status = SplitResult::Status::SourceMissing;
		return result;
	}

	if (!QDir{}.mkpath(outputFolder))
	{
		result.status = SplitResult::Status::FolderCreateFailed;
		return result;
	}

	// Removes outputFolder on any failure path below, so a failed extraction leaves no debris - this undoes the
	// mkpath above plus whatever ffmpeg partially wrote (callers always wipe the folder before calling anyway).
	// The folder exists and no ffmpeg process is left running by the time any call site gets here, so a failure is unexpected.
	const auto cleanupAfterFailure = [&outputFolder] { assert_r(QDir(outputFolder).removeRecursively()); };

	const QString baseName      = QFileInfo(videoFilePath).completeBaseName();
	const QString outputPattern = outputFolder + "/%04d_" + baseName + (options.tiff ? ".tif" : ".jpg");

	QStringList arguments;
	arguments << "-i" << QDir::toNativeSeparators(videoFilePath)
		<< "-an" << "-sn" << "-dn" // No audio, no subtitles, no data
		<< "-y"; // Overwrite output files without asking

	if (options.frameStep > 1)
	{
		// The comma inside mod(n,N) must be escaped: ffmpeg's filtergraph parser splits filters on commas and
		// doesn't track parentheses, so an unescaped one would truncate the select expression. The comma before
		// format= is left bare - it's the real select->format chain separator.
		arguments << "-filter:v" << QString("select=not(mod(n\\,%1)),format=pix_fmts=rgb24").arg(options.frameStep)
			<< "-fps_mode" << "vfr";
	}
	else
	{
		arguments << "-filter:v" << "format=pix_fmts=rgb24";
	}

	// Per-format encoder option: -compression_algo is TIFF-only (deflate - lossless, and far better than the
	// encoder's packbits default on photographic frames); -qscale:v is the JPEG quality knob, meaningless for TIFF.
	if (options.tiff)
		arguments << "-compression_algo" << "deflate";
	else
		arguments << "-qscale:v" << QString::number(options.jpegQuality);
	arguments << QDir::toNativeSeparators(outputPattern);

	QProcess process;
	process.start(ffmpegPath(), arguments);

	if (!process.waitForStarted())
	{
		cleanupAfterFailure();
		result.status = SplitResult::Status::StartFailed;
		return result;
	}

	if (!process.waitForFinished(300000)) // 5-minute timeout
	{
		process.kill();
		process.waitForFinished();  // reap the killed process rather than leaving it orphaned
		cleanupAfterFailure();
		result.status = SplitResult::Status::TimedOut;
		return result;
	}

	if (process.exitCode() != 0)
	{
		result.exitCode    = process.exitCode();
		result.errorOutput = process.readAllStandardError() + "\n" + process.readAllStandardOutput();
		cleanupAfterFailure();
		result.status = SplitResult::Status::ExtractionFailed;
		return result;
	}

	const int frameCount = static_cast<int>(QDir(outputFolder).entryList({ "*.jpg", "*.tif" }, QDir::Files).count());
	if (frameCount == 0)
	{
		cleanupAfterFailure();
		result.status = SplitResult::Status::NoFrames;
		return result;
	}

	result.frameCount = frameCount;
	return result;
}

SplitResult extractFrame(const QString& videoFilePath, qint64 timestampMs, const QString& outputFilePath, int jpegQuality)
{
	SplitResult result;

	if (!QFileInfo::exists(videoFilePath))
	{
		result.status = SplitResult::Status::SourceMissing;
		return result;
	}

	if (!QDir{}.mkpath(QFileInfo{ outputFilePath }.absolutePath()))
	{
		result.status = SplitResult::Status::FolderCreateFailed;
		return result;
	}

	// Unlike splitVideoIntoFrames' folder wipe, only the (possibly partial) output file is removed on failure -
	// the folder may be a user-chosen destination holding other files. ffmpeg can also fail before creating the
	// file at all, hence the existence check: only removing a file that is there must succeed.
	const auto cleanupAfterFailure = [&outputFilePath] {
		if (QFile::exists(outputFilePath))
			assert_r(QFile::remove(outputFilePath));
	};

	QStringList arguments;
	arguments << "-y"
		// -ss before -i: a keyframe-index seek, then decode up to the exact timestamp (see
		// buildExtractionArguments) - frame-accurate without decoding the whole stream.
		<< "-ss" << QString::number(timestampMs / 1000.0, 'f', 3)
		<< "-i" << QDir::toNativeSeparators(videoFilePath)
		<< "-map" << "0:v:0"
		<< "-frames:v" << "1"
		<< "-filter:v" << "format=pix_fmts=rgb24";

	// Same per-format encoder options as splitVideoIntoFrames (see the comment there).
	if (outputFilePath.endsWith(".tif", Qt::CaseInsensitive))
		arguments << "-compression_algo" << "deflate";
	else
		arguments << "-qscale:v" << QString::number(jpegQuality);
	arguments << QDir::toNativeSeparators(outputFilePath);

	QProcess process;
	process.start(ffmpegPath(), arguments);

	if (!process.waitForStarted())
	{
		result.status = SplitResult::Status::StartFailed;
		return result;
	}

	if (!process.waitForFinished(60000)) // generous for a seek + single-frame decode
	{
		process.kill();
		process.waitForFinished();  // reap the killed process rather than leaving it orphaned
		cleanupAfterFailure();
		result.status = SplitResult::Status::TimedOut;
		return result;
	}

	if (process.exitCode() != 0)
	{
		result.exitCode    = process.exitCode();
		result.errorOutput = process.readAllStandardError() + "\n" + process.readAllStandardOutput();
		cleanupAfterFailure();
		result.status = SplitResult::Status::ExtractionFailed;
		return result;
	}

	// A timestamp at/past the end of the stream makes ffmpeg exit cleanly having written nothing.
	if (!QFileInfo::exists(outputFilePath))
	{
		result.status = SplitResult::Status::NoFrames;
		return result;
	}

	result.frameCount = 1;
	return result;
}

} // namespace Ffmpeg
