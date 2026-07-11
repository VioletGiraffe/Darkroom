#include "Core/IoThreadPool.h"

#include "system/storagespeed.hpp"

#include <filesystem>
#include <thread>
#include <utility>

namespace {
	CWorkerThreadPool& serialPool()
	{
		static CWorkerThreadPool instance{ 1, "io" };  // exactly one worker: seek-penalty media must not see parallel reads
		return instance;
	}

	CWorkerThreadPool& parallelPool()
	{
		const uint32_t threadCount = qBound(2u, std::thread::hardware_concurrency(), 6u);
		static CWorkerThreadPool instance{ threadCount, "io-fast" };
		return instance;
	}

	CWorkerThreadPool& poolForPath(const QString& filePath)
	{
		const bool fastMedia = storageSpeedForPath(std::filesystem::path{ filePath.toStdU16String() }) == StorageSpeed::FastRandomAccess;
		return fastMedia ? parallelPool() : serialPool();
	}
}

void IoThreadPool::enqueue(const QString& filePath, TaskType task, const uint64_t tag)
{
	poolForPath(filePath).enqueue(std::move(task), tag);
}

void IoThreadPool::retire(const uint64_t tag)
{
	parallelPool().retire(tag);
	serialPool().retire(tag);
}

void IoThreadPool::finishAllThreads()
{
	parallelPool().finishAllThreads();
	serialPool().finishAllThreads();
}
