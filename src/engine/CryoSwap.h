#pragma once

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <memoryapi.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

class VramPool;

// MEM_RESET_UNDO tells the kernel the contents that were discarded by MEM_RESET
// are wanted again. If the pages were never physically reused the data is still
// there; otherwise the call fails and we fall back to the GDDR6 copy.
#ifndef MEM_RESET_UNDO
#define MEM_RESET_UNDO 0x1000000
#endif

struct FreezeResult {
    bool ok = false;
    uint64_t bytes = 0;        // bytes pushed into GDDR6
    uint64_t wsBefore = 0;
    uint64_t wsAfter = 0;
    double ingressMs = 0.0;    // copy loop only
    double totalMs = 0.0;      // whole freeze (suspend, MEM_RESET, quota)
    int regions = 0;
    int threads = 0;
};

struct RestoreResult {
    bool ok = false;
    uint64_t bytes = 0;
    double totalMs = 0.0;
    double dmaMBps = 0.0;
    int dmaRegions = 0;
    int undoRegions = 0;
    bool checksumOk = true;
};

struct CryoStats {
    bool enabled = true;
    bool busy = false;

    int frozen = 0;            // processes currently suspended + offloaded
    int compressed = 0;        // cumulative freeze count
    int skippedAudio = 0;
    int skippedCritical = 0;
    int skippedSmall = 0;

    uint64_t savedBytes = 0;   // DDR4 physically released
    uint64_t hotBytes = 0;     // pages streamed into the VRAM pool
    uint64_t lastCycleMs = 0;
    double lastIngressMBps = 0.0;
    double lastEgressMBps = 0.0;
    double lastWakeMs = 0.0;
};

struct CachedChunk {
    uint64_t vramOffset = 0;
    uint32_t bytes = 0;
};

struct CachedRegion {
    uint64_t address = 0;
    uint64_t size = 0;
    uint64_t hash = 0;
    std::vector<CachedChunk> chunks;
};

struct CachedProcess {
    DWORD pid = 0;
    uint64_t wsBefore = 0;
    uint64_t bytes = 0;
    ULONGLONG frozenAt = 0;
    std::vector<CachedRegion> regions;
};

// CryoSwap moves private pages of background processes into the GDDR6 pool,
// tells the kernel to drop them (MEM_RESET: no pagefile write), clamps the
// working set and suspends the process. Focus brings it back: MEM_RESET_UNDO
// when the pages survived, otherwise a real DMA copy out of VRAM followed by
// WriteProcessMemory.
class CryoSwap {
public:
    CryoSwap();
    ~CryoSwap();

    // SE_INC_BASE_PRIORITY, SE_PROF_SINGLE_PROCESS, SE_DEBUG.
    static bool EnablePrivileges();

    bool Init(VramPool* pool);
    void Shutdown();

    void StartMonitor();
    void StopMonitor();

    void SetEnabled(bool on);
    bool Enabled() const { return enabled_.load(); }

    // Freezes one process into GDDR6 (suspended on return).
    bool CryoFreeze(DWORD pid, FreezeResult& result);
    // Brings it back and resumes its threads.
    bool CryoRestore(DWORD pid, bool forceDma, RestoreResult& result);

    int RunCycle(DWORD onlyPid = 0);
    int Reset();

    bool IsFrozen(DWORD pid) const;
    bool WakeNow(DWORD pid, double& wakeMs);

    // Streams pages into the pool (self-test bench).
    bool StreamWrite(uint64_t offset, const void* data, size_t bytes);

    CryoStats GetStats() const;
    std::vector<CachedProcess> SnapshotFrozen() const;

private:
    void MonitorLoop();
    void Log(const wchar_t* format, ...);

    bool IsCritical(const std::wstring& name) const;
    bool PlaceChunk(uint32_t bytes, uint64_t& vramOffset);
    void PublishCycle(int frozen, int skippedAudio, int skippedCritical, int skippedSmall,
                      uint64_t hotBytes, uint64_t savedBytes, uint64_t cycleMs,
                      double ingressMBps);

    VramPool* pool_ = nullptr;

    std::thread monitor_;
    std::atomic<bool> stop_{ false };
    std::atomic<bool> enabled_{ true };

    mutable std::mutex statsMutex_;
    CryoStats stats_;

    std::mutex cycleMutex_;
    mutable std::mutex cacheMutex_;
    std::vector<CachedProcess> cache_;
    std::vector<uint64_t> sliceFill_;
    uint32_t nextSlice_ = 0;
    uint64_t cryoUsed_ = 0;
};
