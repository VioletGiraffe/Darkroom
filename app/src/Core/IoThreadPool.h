#pragma once

#include "compiler/compiler_warnings_control.h"
#include "threading/cthreadpool.h"

DISABLE_COMPILER_WARNINGS
#include <QString>
RESTORE_COMPILER_WARNINGS

#include <stdint.h>

// Process-wide disk I/O router. Fast random-access volumes use a bounded parallel pool; slow, external,
// network, and unknown storage share one serial worker to avoid seek thrash. Pools stay private because their
// parallelFor() would violate serial-pool routing by also running work on the caller.
namespace IoThreadPool
{
	// A non-zero tag groups tasks for retire().
	void enqueue(const QString& filePath, TaskType task, uint64_t tag = 0);

	// Removes queued tasks and waits out an in-flight task. On return no work with this non-zero tag remains.
	void retire(uint64_t tag);

	// Drops queued tasks and joins workers before QApplication teardown.
	void finishAllThreads();
}
