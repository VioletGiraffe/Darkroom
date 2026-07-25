#include "Ffmpeg.h"
#include "Utils.h"
#include "assert/advanced_assert.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStringList>

#include <vector>

namespace {

// Preview quality is independent of the archival full-split setting.
constexpr int kPreviewFrameJpegQuality = 5;

// Width follows the source aspect ratio; small sources may be upscaled.
constexpr int kPreviewFrameHeight = 360;

// Large sequential reads run alone to avoid disk and CPU contention. Tuned by eye.
constexpr qint64 kSoloExtractionAboveBytes = 500LL * 1024 * 1024;

void waitForFinishedOrKill(QProcess& process, int timeoutMs, const std::atomic<bool>& cancelled)
{
	// Slices bound cancellation latency.
	constexpr int sliceMs = 100;
	for (int remainingMs = timeoutMs; remainingMs > 0 && !cancelled; remainingMs -= sliceMs)
		if (process.waitForFinished(qMin(sliceMs, remainingMs)))
			return;

	process.kill();
	process.waitForFinished();  // reap the killed process rather than leaving it orphaned
}

// Starts each process window before consuming it in start order. Cancellation skips later windows but finishes
// the current one so its processes are killed and reaped.
template <typename StartFn, typename FinishFn>
void runInProcessWindows(const std::vector<int>& windowSizes, const std::atomic<bool>& cancelled, StartFn&& start, FinishFn&& finish)
{
	int windowStart = 0;
	for (const int windowCount : windowSizes)
	{
		if (cancelled)
			break;

		// QProcess is neither copyable nor movable, so construct the final vector size in place.
		std::vector<QProcess> processes(windowCount);
		for (int i = 0; i < windowCount; ++i)
			start(windowStart + i, processes[i]);

		for (int i = 0; i < windowCount; ++i)
			finish(windowStart + i, processes[i]);

		windowStart += windowCount;
	}
}

// No ffprobe binary ships with the app, so read ffmpeg's Duration banner from stderr. The deliberately missing
// output makes the process fail; its exit code is irrelevant.
void startDurationProbe(QProcess& probe, const QString& videoFilePath)
{
	probe.start(ffmpegPath(), { "-i", QDir::toNativeSeparators(videoFilePath) });
}

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

// Mirrors pickEvenlySpacedFrames' 10%-90% sampling window in the time domain.
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

QStringList buildExtractionArguments(const QString& videoFilePath, const QString& previewFolder, const std::vector<qint64>& timestampsMs)
{
	const QString nativeVideoPath = QDir::toNativeSeparators(videoFilePath);

	QStringList arguments;
	arguments << "-y";

	// Opening once per timestamp keeps -ss before -i, where it is a keyframe-index seek rather than a decode
	// from the start of the stream.
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
	const std::atomic<bool>& cancelled, const std::function<void(int, int, Phase)>& onProgress)
{
	const int total = static_cast<int>(jobs.size());
	std::vector<PreviewResult> results(total);
	if (total == 0)
		return results;

	const int maxProcesses = qMax(1, maxConcurrentProcesses);

	// Jobs that never reach a terminal state are precisely those cancelled between or during passes.
	int completed = 0;
	std::vector<bool> reachedTerminalState(total, false);
	const auto markTerminal    = [&](int i) { reachedTerminalState[i] = true; ++completed; };
	const auto reportCompleted = [&](int i) { markTerminal(i); if (onProgress) onProgress(completed, total, Phase::Extracting); };

	// Probing has its own progress count because success is not terminal yet.
	int probed = 0;
	const auto reportProbed = [&] { ++probed; if (onProgress) onProgress(probed, total, Phase::Probing); };

	// Probe windows are always full; only extraction needs the large-file solo rule. Failed probes receive no
	// arguments, so pass 2 contains only viable jobs.
	std::vector<int> probeWindows;
	for (int remaining = total; remaining > 0; remaining -= maxProcesses)
		probeWindows.push_back(qMin(maxProcesses, remaining));

	std::vector<bool> probeStarted(total, false);
	std::vector<QStringList> extractionArguments(total);
	runInProcessWindows(probeWindows, cancelled,
		[&](int i, QProcess& probe) {
			if (!QDir{}.mkpath(jobs[i].destinationFolder))
				return;
			startDurationProbe(probe, jobs[i].videoFilePath);
			probeStarted[i] = true;
		},
		[&](int i, QProcess& probe) {
			if (!probeStarted[i])
			{
				results[i].status = PreviewResult::Status::FolderCreateFailed;
				markTerminal(i);
				reportProbed();
				return;
			}
			const qint64 durationMs = parseProbedDurationMs(probe, cancelled);
			if (cancelled)
				return;
			if (durationMs <= 0)
			{
				results[i].status = PreviewResult::Status::ProbeFailed;
				markTerminal(i);
			}
			else
			{
				results[i].durationMs = durationMs;
				extractionArguments[i] = buildExtractionArguments(jobs[i].videoFilePath, jobs[i].destinationFolder,
					pickEvenlySpacedTimestampsMs(durationMs, frameCount));
			}
			reportProbed();
		});

	// Large sources get solo extraction windows; small sources pack up to maxProcesses.
	std::vector<int> jobsToExtract;
	jobsToExtract.reserve(total);
	for (int i = 0; i < total; ++i)
	{
		if (!extractionArguments[i].isEmpty())
			jobsToExtract.push_back(i);
	}

	std::vector<int> extractionWindows;
	int packed = 0;
	for (const int i : jobsToExtract)
	{
		if (const auto fileSize = QFileInfo(jobs[i].videoFilePath).size(); fileSize > kSoloExtractionAboveBytes)
		{
			if (packed > 0) { extractionWindows.push_back(packed); packed = 0; }
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
					return;
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
	static const std::atomic<bool> neverCancelled{ false };
	return generatePreviewFrames({ PreviewJob{ videoFilePath, destinationFolder } }, frameCount, /*maxConcurrentProcesses*/ 1, neverCancelled).front();
}

SplitResult splitVideoIntoFrames(const QString& videoFilePath, const QString& outputFolder, const SplitOptions& options)
{
	SplitResult result;

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

	// Every caller invokes this only after the process has stopped and the folder has been created.
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

	// Deflate is lossless and substantially better than TIFF's packbits default on photographic frames.
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

	if (!process.waitForFinished(300000))
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

SplitResult extractSingleFrame(const QString& videoFilePath, qint64 timestampMs, const QString& outputFilePath, int jpegQuality)
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

	QStringList arguments;
	arguments << "-n"
		// -ss before -i seeks by keyframe, then decodes forward to the exact timestamp.
		<< "-ss" << QString::number(timestampMs / 1000.0, 'f', 3)
		<< "-i" << QDir::toNativeSeparators(videoFilePath)
		<< "-map" << "0:v:0"
		<< "-frames:v" << "1"
		<< "-filter:v" << "format=pix_fmts=rgb24";

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

	if (!process.waitForFinished(60000))
	{
		process.kill();
		process.waitForFinished();  // reap the killed process rather than leaving it orphaned
		result.status = SplitResult::Status::TimedOut;
		return result;
	}

	if (process.exitCode() != 0)
	{
		result.exitCode    = process.exitCode();
		result.errorOutput = process.readAllStandardError() + "\n" + process.readAllStandardOutput();
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
