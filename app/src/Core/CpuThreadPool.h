#pragma once

#include "threading/cthreadpool.h"

// The process-wide pool for CPU-bound background work, one thread short of the physical core count because
// parallelFor() also runs work on the calling thread. Disk reads belong in IoThreadPool, which routes them by storage speed.
[[nodiscard]] CThreadPool& cpuThreadPool();
