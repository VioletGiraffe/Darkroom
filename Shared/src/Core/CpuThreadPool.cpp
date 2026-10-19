#include "Core/CpuThreadPool.h"

#include "threading/thread_helpers.h"

#include <algorithm>

CThreadPool& cpuThreadPool()
{
	static CThreadPool pool{ std::max(CpuCount::get().physicalCoreCount() - 1, 1u), "cpu" };
	return pool;
}

void cpuParallelFor(size_t count, const std::function<void(size_t index)>& body)
{
	cpuThreadPool().parallelFor(count, body);
}
