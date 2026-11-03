#include "BatchConvert/BatchConvertJob.h"
#include "Core/CpuThreadPool.h"
#include "Core/IoThreadPool.h"
#include "FileOperations.h"
#include "ImageDecoding/ImageDecoding.h"
#include "ImageEncoding/ImageEncoding.h"
#include "Utils.h"
#include "assert/advanced_assert.h"
#include "fs.hpp"              // thin_io
#include "qt_paths.hpp"        // thin_io
#include "system/storagespeed.hpp"
#include "threading/cthreadpool.h"
#include "threading/thread_helpers.h"

DISABLE_COMPILER_WARNINGS
#include "resize/qimage_resize.h"

#include <QColorSpace>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QSet>
RESTORE_COMPILER_WARNINGS

#include <atomic>
#include <exception>
#include <filesystem>
#include <new>
#include <optional>
#include <utility>

namespace BatchConvert {

// What the stages of every target share
struct StageState
{
	explicit StageState(Options options) : options{ std::move(options) } {}

	const Options options;
	std::atomic_bool cancelled = false;
	std::shared_ptr<AsyncDelivery<size_t>> startDelivery; // of target indices
	std::shared_ptr<AsyncDelivery<TargetResult>> delivery;
};

namespace {

// One target's data, handed from stage to stage
struct TargetWork
{
	Target target;
	size_t targetIndex = 0;
	CThreadPool* writePool = nullptr; // null for the I/O pool
	ImageDecoding::EncodedImage source;
	int64_t sourceBytes = 0; // the size of source, kept after it is freed
	std::optional<thin_io::entry_times> sourceTimes;
	QByteArray output;
};

void deliverEnding(const StageState& state, TargetWork& work, Ending ending, QString error = {})
{
	// A failure after a cancel is the cancel's doing
	if (ending == Ending::Failed && state.cancelled)
		ending = Ending::Cancelled;
	if (ending == Ending::Failed && error.isEmpty())
		error = QObject::tr("Unknown error");
	TargetResult result{ std::move(work.target), work.targetIndex, ending, std::move(error) };
	if (ending == Ending::Written)
	{
		result.sourceBytes = work.sourceBytes;
		result.outputBytes = work.output.size();
	}
	state.delivery->deliver(std::move(result), state.delivery);
}

using Stage = void (*)(const std::shared_ptr<StageState>&, const std::shared_ptr<TargetWork>&);

// An exception ends the target as failed: the pool would contain it, and the target would never end
void runStage(Stage stage, const std::shared_ptr<StageState>& state, const std::shared_ptr<TargetWork>& work)
{
	try
	{
		stage(state, work);
	}
	catch (const std::bad_alloc&)
	{
		deliverEnding(*state, *work, Ending::Failed, QObject::tr("Not enough memory"));
	}
	catch (const std::exception& e)
	{
		deliverEnding(*state, *work, Ending::Failed, QString::fromLocal8Bit(e.what()));
	}
	catch (...)
	{
		deliverEnding(*state, *work, Ending::Failed);
	}
}

// image at size; null when out of memory. Single-threaded: the job runs one target per CPU worker.
QImage resizedImage(const QImage& image, QSize size)
{
	if (size == image.size())
		return image;

	QImage resized{ size, image.format() };
	if (resized.isNull())
		return {};
	if (!ImageProcessing::resize(resized, image))
		resized = image.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
	resized.setColorSpace(image.colorSpace());
	return resized;
}

void writeTarget(const std::shared_ptr<StageState>& state, const std::shared_ptr<TargetWork>& work)
{
	if (state->cancelled)
		return deliverEnding(*state, *work, Ending::Cancelled);

	const Target& target = work->target;
	// Where Trash is unavailable, the write replaces the source
	QString sourceInTrash;
	const auto trashSource = [&] { static_cast<void>(QFile::moveToTrash(target.sourcePath, &sourceInTrash)); };
	QString error = ImageEncoding::writeFileAtomically(target.targetPath, work->output, target.replacesSource ? std::function<void()>{ trashSource } : std::function<void()>{});
	if (!error.isEmpty())
	{
		// The source is back in place unless the move out of Trash fails too
		if (!sourceInTrash.isEmpty() && !QFile::rename(sourceInTrash, target.sourcePath))
			error = QObject::tr("%1 (the original is in Trash)").arg(error);
		return deliverEnding(*state, *work, Ending::Failed, QObject::tr("Cannot save %1: %2").arg(QDir::toNativeSeparators(target.targetPath), error));
	}

	if (work->sourceTimes)
	{
		thin_io::entry_times times;
		times.last_write = work->sourceTimes->last_write;
		// Best effort: the image is written either way
		static_cast<void>(thin_io::set_times(thin_io::native_path(target.targetPath).data(), times));
	}
	deliverEnding(*state, *work, Ending::Written);
}

void convertTarget(const std::shared_ptr<StageState>& state, const std::shared_ptr<TargetWork>& work)
{
	if (state->cancelled)
		return deliverEnding(*state, *work, Ending::Cancelled);

	// Posted before the ending: the context's queue keeps the order
	state->startDelivery->deliver(work->targetIndex, state->startDelivery);
	ImageDecoding::DecodedImage decoded = ImageDecoding::decode(work->source, [&state](double) { return !state->cancelled; });
	if (decoded.image.isNull())
		return deliverEnding(*state, *work, Ending::Failed, decoded.error);
	ImageDecoding::applyOrientation(decoded.image, decoded.orientation);

	const QImage image = resizedImage(decoded.image, outputSize(decoded.image.size(), state->options.resize));
	if (image.isNull())
		return deliverEnding(*state, *work, Ending::Failed, QObject::tr("Not enough memory to resize the image"));

	const ImageEncoding::EncodingOptions encoding{ .mimeType = work->target.mimeType, .quality = state->options.quality, .jpegSubsampling = state->options.jpegSubsampling };
	ImageEncoding::EncodedFile output = ImageEncoding::encodeImage(image, decoded.iccProfile, encoding, work->source.bytes);
	work->sourceBytes = work->source.bytes.size();
	work->source = {}; // the whole source file, for a camera raw
	if (output.bytes.isEmpty())
		return deliverEnding(*state, *work, Ending::Failed, output.error);

	work->output = std::move(output.bytes);
	auto write = [state, work] { runStage(writeTarget, state, work); };
	if (work->writePool)
		work->writePool->enqueue(std::move(write));
	else
		IoThreadPool::enqueue(work->target.targetPath, std::move(write));
}

void readTarget(const std::shared_ptr<StageState>& state, const std::shared_ptr<TargetWork>& work)
{
	if (state->cancelled)
		return deliverEnding(*state, *work, Ending::Cancelled);

	work->source = ImageDecoding::readEncodedImage(work->target.sourcePath, ImageDecoding::RawRendering::Full);
	if (!work->source.error.isEmpty())
		return deliverEnding(*state, *work, Ending::Failed, work->source.error);
	// Before the write, which may move the source to Trash
	work->sourceTimes = thin_io::get_times(thin_io::native_path(work->target.sourcePath).data());

	cpuThreadPool().enqueue([state, work] { runStage(convertTarget, state, work); });
}

bool isSlowStorage(const QString& path)
{
	return storageSpeedForPath(std::filesystem::path{ path.toStdU16String() }) != StorageSpeed::FastRandomAccess;
}

} // namespace

Job::Job(std::vector<Target> targets, Options options, std::function<void(size_t targetIndex)> onTargetStarted,
	std::function<void(const TargetResult&)> onTargetEnded, std::function<void()> onFinished, QObject* parent)
	: QObject{ parent }
	, _targets{ std::move(targets) }
	, _stageState{ std::make_shared<StageState>(std::move(options)) }
	, _onTargetStarted{ std::move(onTargetStarted) }
	, _onTargetEnded{ std::move(onTargetEnded) }
	, _onFinished{ std::move(onFinished) }
{
	_stageState->startDelivery = std::make_shared<AsyncDelivery<size_t>>(this, [this](size_t targetIndex) { _onTargetStarted(targetIndex); });
	_startRequest = AsyncRequest{ _stageState->startDelivery };
	_stageState->delivery = std::make_shared<AsyncDelivery<TargetResult>>(this, [this](TargetResult result) { endTarget(result); });
	_deliveryRequest = AsyncRequest{ _stageState->delivery };
}

Job::~Job()
{
	assert_r(_inFlight == 0);
}

void Job::start()
{
	assert_and_return_r(!_running && _nextTarget == 0, );

	QSet<QString> checkedFolderKeys;
	for (const Target& target : _targets)
	{
		const QString folder = QFileInfo{ target.targetPath }.absolutePath();
		const QString folderKey = pathComparisonKey(folder);
		if (target.skipped || checkedFolderKeys.contains(folderKey))
			continue;
		checkedFolderKeys.insert(folderKey);
		if (!QDir{}.mkpath(folder))
			_uncreatableFolderKeys.insert(folderKey);
	}

	_running = true;
	startTargets();
}

void Job::cancel()
{
	_stageState->cancelled = true;
	_nextTarget = _targets.size();
	if (_running && _inFlight == 0)
	{
		_running = false;
		_onFinished();
	}
}

void Job::startTargets()
{
	// Enough to keep every CPU worker busy while the next sources are read
	const int maxInFlight = 2 * static_cast<int>(CpuCount::get().logicalProcessorCount());
	while (_inFlight < maxInFlight && _nextTarget < _targets.size())
	{
		const size_t targetIndex = _nextTarget++;
		Target& target = _targets[targetIndex];
		if (target.skipped)
		{
			_onTargetEnded(TargetResult{ std::move(target), targetIndex, Ending::Skipped });
			continue;
		}
		if (const QString folder = QFileInfo{ target.targetPath }.absolutePath(); _uncreatableFolderKeys.contains(pathComparisonKey(folder)))
		{
			const QString error = QObject::tr("Cannot create the folder %1").arg(QDir::toNativeSeparators(folder));
			_onTargetEnded(TargetResult{ std::move(target), targetIndex, Ending::Failed, error });
			continue;
		}

		auto work = std::make_shared<TargetWork>();
		work->targetIndex = targetIndex;
		work->writePool = writePool(target);
		work->target = std::move(target);
		++_inFlight;
		IoThreadPool::enqueue(work->target.sourcePath, [state = _stageState, work] { runStage(readTarget, state, work); });
	}

	if (_inFlight == 0 && _running)
	{
		_running = false;
		_onFinished();
	}
}

void Job::endTarget(const TargetResult& result)
{
	--_inFlight;
	_onTargetEnded(result);
	startTargets();
}

CThreadPool* Job::writePool(const Target& target)
{
	const QString sourceFolder = QFileInfo{ target.sourcePath }.absolutePath();
	const QString targetFolder = QFileInfo{ target.targetPath }.absolutePath();
	const QString foldersKey = pathComparisonKey(sourceFolder) + '\n' + pathComparisonKey(targetFolder);
	auto ownPool = _ownWritePoolByFolders.find(foldersKey);
	if (ownPool == _ownWritePoolByFolders.end())
	{
		// Two volumes that cannot be told apart count as different
		const bool separateSlowVolumes = isSlowStorage(targetFolder) && isSlowStorage(sourceFolder) && !FileOperations::isOnSameVolume(target.sourcePath, targetFolder);
		ownPool = _ownWritePoolByFolders.insert(foldersKey, separateSlowVolumes);
	}
	if (!*ownPool)
		return nullptr;

	if (!_ownWritePool)
		_ownWritePool = std::make_unique<CThreadPool>(1, "batch-write");
	return _ownWritePool.get();
}

} // namespace BatchConvert
