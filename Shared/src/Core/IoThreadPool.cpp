#include "Core/IoThreadPool.h"

#include "system/storagespeed.hpp"

#include <filesystem>
#include <utility>

namespace {
	CThreadPool& serialPool()
	{
		static CThreadPool instance{ 1, "io" };  // exactly one worker: seek-penalty media must not see parallel reads
		return instance;
	}

	CThreadPool& parallelPool()
	{
		// Not per core: the workers mostly wait on the device; 6 saturated every SSD measured, even on one core (docs/image-read-performance.md)
		static CThreadPool instance{ 6, "io-fast" };
		return instance;
	}

	CThreadPool& poolForPath(const QString& filePath)
	{
		const bool fastMedia = storageSpeedForPath(std::filesystem::path{ filePath.toStdU16String() }) == StorageSpeed::FastRandomAccess;
		return fastMedia ? parallelPool() : serialPool();
	}
}

void IoThreadPool::enqueue(const QString& filePath, TaskType task, const uint64_t tag)
{
	poolForPath(filePath).enqueue(std::move(task), tag);
}

void IoThreadPool::parallelForAsync(const QString& filePath, const size_t count, std::function<void(size_t index)> fn, std::function<void()> onAllCompleted)
{
	// The serial pool's single worker drains every index itself, in order
	poolForPath(filePath).parallelForAsync(count, std::move(fn), std::move(onAllCompleted));
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
