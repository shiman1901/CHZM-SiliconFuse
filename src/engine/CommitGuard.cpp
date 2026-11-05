#include "CommitGuard.h"

#include <windows.h>
#include <psapi.h>

namespace {

// GetPerformanceInfo returns counts in pages; PageSize comes from the same
// call, so the numbers stay consistent under memory pressure.
bool QueryPerf(PERFORMANCE_INFORMATION& info)
{
    info = PERFORMANCE_INFORMATION{};
    info.cb = sizeof(info);
    return GetPerformanceInfo(&info, sizeof(info)) != FALSE;
}

} // namespace

uint64_t FreeCommitMb()
{
    PERFORMANCE_INFORMATION info{};
    if (!QueryPerf(info)) return 0;
    if (info.CommitLimit <= info.CommitTotal) return 0;
    const uint64_t freePages = (uint64_t)(info.CommitLimit - info.CommitTotal);
    return freePages * (uint64_t)info.PageSize / (1024ull * 1024ull);
}

uint64_t TotalCommitLimitMb()
{
    PERFORMANCE_INFORMATION info{};
    if (!QueryPerf(info)) return 0;
    return (uint64_t)info.CommitLimit * (uint64_t)info.PageSize / (1024ull * 1024ull);
}
