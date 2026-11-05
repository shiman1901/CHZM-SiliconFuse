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

#include "StorageBridge.h"

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
    int regions = 0;
    int threads = 0;

    // Sub-timers (ms).
    double suspendMs = 0.0;     // NtSuspendProcess + thread snapshot check
    double scanZeroMs = 0.0;    // VirtualQueryEx region scan
    double rpmReadMs = 0.0;     // ReadProcessMemory
    double vramWriteMs = 0.0;   // AVX2 streaming into BAR1
    double mirrorWriteMs = 0.0; // fail-safe copy to StorageBridge
    double resetTrimMs = 0.0;   // MEM_RESET + HARDWS quota + EmptyWorkingSet
    double hashMs = 0.0;        // checksum of the copied pages
    double ingressMs = 0.0;     // rpmReadMs + vramWriteMs + hashMs
    double overheadMs = 0.0;    // totalMs - sum(phases)
    double totalMs = 0.0;
    bool mirrorDurable = false; // every mirrored byte landed on the NVMe

    uint64_t origHash = 0;      // combined checksum of all cached regions
};

struct RestoreResult {
    bool ok = false;
    uint64_t bytes = 0;
    double dmaMBps = 0.0;
    int dmaRegions = 0;
    int mirrorRegions = 0;      // regions served by the StorageBridge mirror
    uint64_t mirrorBytes = 0;
    int undoRegions = 0;
    bool checksumOk = true;
    bool safeLockout = false;   // both sources failed, process left suspended

    // Sub-timers (ms).
    double dmaGpuMs = 0.0;      // copy engine + fence wait
    double fallbackMs = 0.0;    // StorageBridge mirror reads (evicted VRAM)
    double hashVerifyMs = 0.0;  // checksum of the DMA'd pages
    double wpmWriteMs = 0.0;    // WriteProcessMemory
    double resumeMs = 0.0;      // quota release + NtResumeProcess
    double overheadMs = 0.0;    // totalMs - sum(phases)
    double totalMs = 0.0;

    uint64_t restoredHash = 0;
};

struct CryoStats {
    bool enabled = true;
    bool busy = false;

    int frozen = 0;            // processes currently suspended + offloaded
    int trimmed = 0;           // processes soft-trimmed (live mode)
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
    uint64_t mirrorOffset = 0;
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

// Live-mode record: the RAM copy is untouched, VRAM only holds a hot cache.
struct TrimmedProcess {
    DWORD pid = 0;
    std::wstring name;
    uint64_t wsBefore = 0;
    uint64_t wsAfter = 0;
    uint64_t bytes = 0;
    uint64_t privateBefore = 0;
    uint64_t privateAfter = 0;
    uint64_t faultsBefore = 0;
    uint64_t faultsAfter = 0;
    int64_t systemDelta = 0;
    uint32_t priorityBefore = 5;
    uint32_t priorityApplied = 5;
    ULONGLONG trimmedAt = 0;
    std::vector<CachedChunk> chunks;
    std::vector<WIN32_MEMORY_RANGE_ENTRY> ranges;
};

enum class LiveTrimMode {
    SafeAutopilot,      // priority + EcoQoS only, no eviction at all
    AggressiveReclaim,  // + one-shot soft trim + GDDR6 hot snapshot
};

struct LiveMetrics {
    uint64_t trimmedWsSum = 0;
    uint64_t trimmedPrivateWsSum = 0;
    int64_t systemAvailableDelta = 0;
    int64_t commitDelta = 0;
    uint64_t gddr6SnapshotBytes = 0;
    uint64_t hardFaultDelta = 0;
    double restoreSettingsMs = 0.0;
    uint64_t elapsedMs = 0;
    int processes = 0;
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

    bool Init(VramPool* pool, StorageBridge* bridge);
    void Shutdown();

    void StartMonitor();
    void StopMonitor();

    void SetEnabled(bool on);
    bool Enabled() const { return enabled_.load(); }

    // Freezes one process into GDDR6 (suspended on return).
    bool CryoFreeze(DWORD pid, FreezeResult& result);
    // Brings it back and resumes its threads.
    bool CryoRestore(DWORD pid, bool forceDma, RestoreResult& result);

    // Live mode: soft trim, no suspend, no MEM_RESET (GUI default).
    int RunCycle(LiveTrimMode mode = LiveTrimMode::AggressiveReclaim, DWORD onlyPid = 0);
    int TrimPids(const std::vector<DWORD>& pids, LiveTrimMode mode);
    int Reset();
    // Fail-safe: bring every frozen process back (used on exit and crashes).
    int RestoreAllFrozen();
    // Live mode: lift quotas + DMA warm + prefetch for a whole exe group.
    int WakeGroup(const std::wstring& name, double& ms);
    int WakePids(const std::vector<DWORD>& pids, double& restoreSettingsMs);
    int WakeAllTrimmed();
    LiveMetrics LastLiveMetrics() const;
    bool IsTrimmed(DWORD pid) const;
    std::wstring TrimmedName(DWORD pid) const;

    bool IsFrozen(DWORD pid) const;
    bool WakeNow(DWORD pid, double& wakeMs);
    // Chaos hook: makes the mirror write fail halfway through a freeze.
    void InjectFreezeAbort(bool on) { abortInjected_.store(on); }
    bool SafeLockout() const { return safeLockout_.load(); }
    void ClearLockout() { safeLockout_.store(false); }

    // Streams pages into the pool (self-test bench).
    bool StreamWrite(uint64_t offset, const void* data, size_t bytes);

    CryoStats GetStats() const;
    std::vector<CachedProcess> SnapshotFrozen() const;

private:
    void MonitorLoop();
    void Log(const wchar_t* format, ...);

    bool IsCritical(const std::wstring& name) const;
    bool PlaceAndWrite(const uint8_t* data, uint32_t bytes, uint64_t& vramOffset);
    bool TrimProcessLive(DWORD pid, const std::wstring& name, HANDLE process,
                         LiveTrimMode mode, TrimmedProcess& out);
    int WakeTrimmedEntry(const TrimmedProcess& entry, double& dmaMBps);
    void RestoreLiveSettings(HANDLE process);
    void JournalSync();
    void JournalSetState(DWORD pid, uint32_t state);
    void PublishCycle(int frozen, int skippedAudio, int skippedCritical, int skippedSmall,
                      uint64_t hotBytes, uint64_t savedBytes, uint64_t cycleMs,
                      double ingressMBps);

    VramPool* pool_ = nullptr;
    StorageBridge* bridge_ = nullptr;

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
    uint64_t mirrorUsed_ = 0;
    std::atomic<bool> abortInjected_{ false };
    std::atomic<bool> safeLockout_{ false };
    std::vector<JournalEntry> journal_;
    std::mutex journalMutex_;
    std::vector<TrimmedProcess> trimmed_;
    mutable std::mutex trimmedMutex_;
    LiveMetrics liveMetrics_;
};
