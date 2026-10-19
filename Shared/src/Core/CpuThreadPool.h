#pragma once

#include "threading/cthreadpool.h"

#include <cstddef>
#include <functional>

// The process-wide pool for CPU-bound background work, one thread short of the physical core count because
// parallelFor() also runs work on the calling thread. Disk reads belong in IoThreadPool, which routes them by storage speed.
[[nodiscard]] CThreadPool& cpuThreadPool();

// cpuThreadPool().parallelFor() as a plain function, matching ImageProcessing::ParallelForFn
void cpuParallelFor(size_t count, const std::function<void(size_t index)>& body);
