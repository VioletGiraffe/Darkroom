#pragma once

class QRunnable;

// The process-wide serial I/O executor: a single dedicated worker thread that all disk-touching background work
// posts to. Serializing reads onto one thread keeps a spinning disk from being seek-thrashed by parallel decodes
// (and lets OS read-ahead work per file), at the cost of not spreading the work across cores - fine for the small
// reads this is meant for (thumbnail decodes). The single-thread guarantee is owned inside the .cpp and deliberately
// not exposed as a QThreadPool&, so no consumer can widen the thread count and silently break serialization for
// everyone else sharing the disk.
namespace IoThreadPool
{
	// Posts task to the I/O thread. Honors the runnable's autoDelete flag, exactly like QThreadPool::start.
	void start(QRunnable* task);

	// Removes task if it is still queued (not started yet); returns false if it is already running or finished.
	bool tryTake(QRunnable* task);
}
