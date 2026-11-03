#pragma once

#include "BatchConvert/BatchConvertPlan.h"
#include "Core/AsyncDelivery.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
RESTORE_COMPILER_WARNINGS

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class CThreadPool;

namespace BatchConvert {

enum class Ending { Written, Skipped, Failed, Cancelled };

struct TargetResult
{
	Target target;
	size_t targetIndex = 0; // in the plan, which keeps the order of the sources
	Ending ending = Ending::Written;
	QString error; // set when Failed
	// Set when Written
	int64_t sourceBytes = 0;
	int64_t outputBytes = 0;
};

struct StageState;

// Writes the targets of a plan. Each target goes through three stages:
// - reading, on the I/O pool for its source;
// - decoding, resizing and encoding, on the CPU pool;
// - writing, on the I/O pool for its target. When the target and its source are on two different slow volumes, the write goes
//   to a one-thread pool of the job's own instead: the I/O pool has one serial worker for all slow storage.
// A bounded number of targets is in flight: reads would otherwise run ahead of the encoding and hold every source in memory.
// Must not be destroyed while running: in-flight stages refer to the job's write pool.
class Job final : public QObject
{
public:
	// The callbacks run on this object's thread.
	// onTargetStarted runs when a target's decoding starts, after its read. A skipped target only ends.
	Job(std::vector<Target> targets, Options options, std::function<void(size_t targetIndex)> onTargetStarted,
		std::function<void(const TargetResult&)> onTargetEnded, std::function<void()> onFinished, QObject* parent);
	~Job() override;

	// Creates the target folders, then starts the targets. The targets of a folder that cannot be created fail at once.
	void start();
	// Starts no more targets; the ones in flight end at their next stage without writing. onFinished follows.
	void cancel();
	[[nodiscard]] bool isRunning() const noexcept { return _running; }

private:
	void startTargets();
	void endTarget(const TargetResult& result);
	// Null for the I/O pool
	[[nodiscard]] CThreadPool* writePool(const Target& target);

private:
	std::vector<Target> _targets;
	size_t _nextTarget = 0;
	int _inFlight = 0;
	bool _running = false;
	std::shared_ptr<StageState> _stageState;
	std::unique_ptr<CThreadPool> _ownWritePool; // created on the first target that needs it
	QHash<QString, bool> _ownWritePoolByFolders; // writePool() by source and target folder
	QSet<QString> _uncreatableFolderKeys; // by pathComparisonKey
	std::function<void(size_t targetIndex)> _onTargetStarted;
	std::function<void(const TargetResult&)> _onTargetEnded;
	std::function<void()> _onFinished;
	AsyncRequest _startRequest;
	AsyncRequest _deliveryRequest;
};

} // namespace BatchConvert
