#pragma once

#include "threading/cworkerthread.h"

#include <QString>

#include <stdint.h>

// The process-wide disk I/O executor, routing each task by the storage medium under its path: volumes with fast
// random access (internal SSD, RAM disk) get a small parallel pool, everything else (HDD, external, network,
// unclassifiable) shares a single dedicated worker - serializing reads onto one thread keeps a spinning disk from
// being seek-thrashed by parallel decodes (and lets OS read-ahead work per file). The pools stay hidden in the
// .cpp: exposing them would offer parallelFor(), which runs on the calling thread too and would break the
// one-thread serialization of the slow pool.
namespace IoThreadPool
{
	// Posts task to the pool serving the volume that filePath resides on (unclassifiable paths go to the serial
	// pool). A non-zero tag groups tasks for retire().
	void enqueue(const QString& filePath, TaskType task, uint64_t tag = 0);

	// Removes tag's not-yet-started tasks and waits out any in-flight task: once it returns, no task with this tag
	// is running or queued. Call from the destructor of the object whose state the tasks capture. Tag must be non-zero.
	void retire(uint64_t tag);

	// Drops the queued tasks, joins the workers. Called in main() after the event loop exits, so the threads are gone
	// while QApplication still exists; the pools' static-teardown destructors then have nothing left to do.
	void finishAllThreads();
}
