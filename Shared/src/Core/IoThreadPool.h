#pragma once

#include "compiler/compiler_warnings_control.h"
#include "threading/cthreadpool.h"

DISABLE_COMPILER_WARNINGS
#include <QString>
RESTORE_COMPILER_WARNINGS

#include <stddef.h>
#include <stdint.h>

#include <functional>

// Process-wide disk I/O router. Fast random-access volumes use a bounded parallel pool; slow, external,
// network, and unknown storage share one serial worker to avoid seek thrash. Pools stay private because their
// blocking parallelFor() would violate serial-pool routing by also running work on the caller.
namespace IoThreadPool
{
	// A non-zero tag groups tasks for retire().
	void enqueue(const QString& filePath, TaskType task, uint64_t tag = 0);

	// Runs fn(0) .. fn(count - 1) on the pool routed for filePath, then onAllCompleted once on a worker; never on the caller.
	// Fast storage runs the calls concurrently, slow storage one after another on the serial worker.
	void parallelForAsync(const QString& filePath, size_t count, std::function<void(size_t index)> fn, std::function<void()> onAllCompleted);

	// Removes queued tasks and waits out an in-flight task. On return no work with this non-zero tag remains.
	void retire(uint64_t tag);

	// Drops queued tasks and joins workers before QApplication teardown.
	void finishAllThreads();
}
