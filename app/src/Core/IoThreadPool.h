#pragma once

#include "threading/cworkerthread.h"

#include <stdint.h>

// The process-wide serial I/O executor: a single dedicated worker thread that all disk-touching background work
// posts to. Serializing reads onto one thread keeps a spinning disk from being seek-thrashed by parallel decodes
// (and lets OS read-ahead work per file), at the cost of not spreading the work across cores - fine for the small
// reads this is meant for (thumbnail decodes). The pool stays hidden in the .cpp: its thread count is fixed at
// construction, but exposing it would offer parallelFor(), which runs on the calling thread too and would break
// the one-thread serialization just the same.
namespace IoThreadPool
{
	// Posts task to the I/O thread. A non-zero tag groups tasks for retire().
	void enqueue(TaskType task, uint64_t tag = 0);

	// Removes tag's not-yet-started tasks and waits out any in-flight task: once it returns, no task with this tag
	// is running or queued. Call from the destructor of the object whose state the tasks capture. Tag must be non-zero.
	void retire(uint64_t tag);

	// Drops the queued tasks, joins the worker. Called in main() after the event loop exits, so the thread is gone
	// while QApplication still exists; the pool's static-teardown destructor then has nothing left to do.
	void finishAllThreads();
}
