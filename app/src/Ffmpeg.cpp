#include "Ffmpeg.h"
#include "Utils.h"

#include <QDir>
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

// Waits for a process to finish, killing it if it overruns so a stuck ffmpeg (e.g. on a corrupt file) never
// lingers past the QProcess it was spawned from - important here since a batch can spawn many at once.
void waitForFinishedOrKill(QProcess& process, int timeoutMs)
{
	if (!process.waitForFinished(timeoutMs))
	{
		process.kill();
		process.waitForFinished();  // reap the killed process rather than leaving it orphaned
	}
}

// Runs `count` operations in windows of up to `concurrency` concurrent QProcess, all on the calling thread -
// the parallelism is the OS running the ffmpeg processes at once, not threads. For each operation in a
// window, start(index, process) launches it without waiting; once the whole window is started,
// finish(index, process) is called per operation (in start order) to wait on and consume its result.
template <typename StartFn, typename FinishFn>
void runInProcessWindows(int count, int concurrency, StartFn&& start, FinishFn&& finish)
{
	for (int windowStart = 0; windowStart < count; windowStart += concurrency)
	{
		const int windowCount = qMin(concurrency, count - windowStart);

		// The sizing constructor default-constructs each QProcess in place; QProcess is a QObject (neither
		// copyable nor movable), so a vector of them can only be built this way, not grown via push_back.
		std::vector<QProcess> processes(windowCount);
		for (int i = 0; i < windowCount; ++i)
			start(windowStart + i, processes[i]);

		for (int i = 0; i < windowCount; ++i)
			finish(windowStart + i, processes[i]);
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
qint64 parseProbedDurationMs(QProcess& probe)
{
	if (!probe.waitForStarted())
		return -1;
	waitForFinishedOrKill(probe, 30000);

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
QList<qint64> pickEvenlySpacedTimestampsMs(qint64 durationMs, int frameCount)
{
	const qint64 startMs = static_cast<qint64>(durationMs * 0.1);
	const qint64 endMs   = static_cast<qint64>(durationMs * 0.9);

	QList<qint64> out;
	out.reserve(frameCount);
	for (int i = 0; i < frameCount; ++i)
		out << ((frameCount == 1) ? startMs : startMs + i * (endMs - startMs) / (frameCount - 1));
	return out;
}

// Builds the argument list for the single multi-seek extraction that writes all of one video's preview
// frames.
QStringList buildExtractionArguments(const QString& videoFilePath, const QString& previewFolder, const QList<qint64>& timestampsMs)
{
	QStringList arguments;
	arguments << "-i" << QDir::toNativeSeparators(videoFilePath)
		<< "-an" << "-sn" << "-dn" // No audio, no subtitles, no data
		<< "-y"; // Overwrite output files without asking

	// One ffmpeg invocation, multiple "-ss T -frames:v 1 output" output groups: ffmpeg seeks per-output on
	// the same already-open input rather than spawning N processes or fully decoding the video.
	for (int i = 0; i < timestampsMs.size(); ++i)
	{
		const QString outputPath = previewFolder + QString("/%1.jpg").arg(i + 1, 4, 10, QChar('0'));
		arguments << "-ss" << QString::number(timestampsMs[i] / 1000.0, 'f', 3)
			<< "-frames:v" << "1"
			<< "-vf" << QString("scale=-2:%1").arg(kPreviewFrameHeight)
			<< "-qscale:v" << QString::number(kPreviewFrameJpegQuality)
			<< QDir::toNativeSeparators(outputPath);
	}
	return arguments;
}

} // namespace

namespace Ffmpeg {

void generatePreviewFrames(const QList<PreviewJob>& jobs, int frameCount, int maxConcurrentProcesses,
	const std::function<void(int, int)>& onProgress)
{
	const int total = jobs.size();
	if (total == 0)
		return;

	const int concurrency = qMax(1, maxConcurrentProcesses);

	int completed = 0;
	const auto reportOneCompleted = [&] { ++completed; if (onProgress) onProgress(completed, total); };

	// Pass 1: probe each job's duration and build its extraction arguments, up to `concurrency` probes at
	// once. A job whose preview folder can't be created or whose duration can't be probed (a corrupt file
	// typically fails here first) gets no arguments and is counted done now, so it never enters pass 2 and
	// the extraction windows there stay packed with only good jobs.
	std::vector<bool> probeStarted(total, false);
	std::vector<QStringList> extractionArguments(total);
	runInProcessWindows(total, concurrency,
		[&](int i, QProcess& probe) {
			if (!QDir{}.mkpath(jobs[i].outputFolder + "/preview"))
				return;  // leaves probeStarted[i] false -> treated as a skip in finish
			startDurationProbe(probe, jobs[i].videoFilePath);
			probeStarted[i] = true;
		},
		[&](int i, QProcess& probe) {
			const qint64 durationMs = probeStarted[i] ? parseProbedDurationMs(probe) : -1;
			if (durationMs <= 0)
				reportOneCompleted();  // best-effort: leave this job's preview/ empty rather than guessing seek points
			else
				extractionArguments[i] = buildExtractionArguments(jobs[i].videoFilePath, jobs[i].outputFolder + "/preview",
					pickEvenlySpacedTimestampsMs(durationMs, frameCount));
		});

	// Pass 2: run the extractions for the jobs that probed successfully, again up to `concurrency` at once.
	// Whatever frames each invocation writes stay in its preview/; no failure is surfaced (see header).
	std::vector<int> jobsToExtract;
	jobsToExtract.reserve(total);
	for (int i = 0; i < total; ++i)
		if (!extractionArguments[i].isEmpty())
			jobsToExtract.push_back(i);

	runInProcessWindows(static_cast<int>(jobsToExtract.size()), concurrency,
		[&](int k, QProcess& extract) {
			extract.start(ffmpegPath(), extractionArguments[jobsToExtract[k]]);
		},
		[&](int /*k*/, QProcess& extract) {
			if (extract.waitForStarted())
				waitForFinishedOrKill(extract, 60000);
			reportOneCompleted();
		});
}

void generatePreviewFrames(const QString& videoFilePath, const QString& outputFolder, int frameCount)
{
	generatePreviewFrames({ PreviewJob{ videoFilePath, outputFolder } }, frameCount, /*maxConcurrentProcesses*/ 1);
}

} // namespace Ffmpeg
