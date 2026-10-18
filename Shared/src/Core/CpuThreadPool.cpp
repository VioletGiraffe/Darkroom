#include "Core/CpuThreadPool.h"

#include "threading/thread_helpers.h"

#include <algorithm>

CThreadPool& cpuThreadPool()
{
	static CThreadPool pool{ std::max(CpuCount::get().physicalCoreCount() - 1, 1u), "cpu" };
	return pool;
}
