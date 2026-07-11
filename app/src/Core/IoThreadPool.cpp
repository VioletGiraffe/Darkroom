#include "Core/IoThreadPool.h"

#include <utility>

namespace {
	CWorkerThreadPool& pool()
	{
		static CWorkerThreadPool instance{ 1, "io" };  // the whole point: exactly one I/O worker for the entire process
		return instance;
	}
}

void IoThreadPool::enqueue(TaskType task, const uint64_t tag)
{
	pool().enqueue(std::move(task), tag);
}

void IoThreadPool::retire(const uint64_t tag)
{
	pool().retire(tag);
}

void IoThreadPool::finishAllThreads()
{
	pool().finishAllThreads();
}
